[CmdletBinding()]
param(
    [string]$ManifestPath = (Join-Path $PSScriptRoot 'components.json'),
    [ValidateSet('Install','Modify','Repair','Reconfigure','Uninstall','Recover','Portable')][string]$Operation = 'Install',
    [string]$Profile,
    [string[]]$ComponentId,
    [string]$InstallRoot,
    [string]$StateDirectory = (Join-Path $env:LOCALAPPDATA 'StarDataParadise/Setup'),
    [hashtable]$Instance,
    [string]$AdminUser = 'local-admin',
    [SecureString]$AdminPassword,
    [string]$ResponseFile,
    [string]$PortableDirectory,
    [switch]$AllowDevelopment,
    [switch]$Silent,
    [switch]$UiSmoke,
    [scriptblock]$OnProgress,
    [scriptblock]$IsCancelled
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'PackagePlan.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'SetupActions.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'InstancePlan.psm1') -Force

function Read-JsonFile([string]$Path, [long]$MaxBytes) {
    $item = Get-Item -LiteralPath $Path
    if ($item.PSIsContainer -or $item.Length -gt $MaxBytes -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'The setup input is not a regular, bounded local file.'
    }
    Get-Content -LiteralPath $item.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Convert-Instance([object]$Value) {
    $result = @{}
    if ($null -eq $Value) { return $result }
    $allowed = @('instanceName','instanceRoot','tlsCertificate','tlsPrivateKey','tlsCa',
        'dataDirectory','logDirectory','tempDirectory','backupDirectory','bind','port','allowRemote','installService','firewall')
    if ($Value -is [System.Collections.IDictionary]) {
        foreach ($key in $Value.Keys) {
            if ($key -notin $allowed) { throw 'The setup instance contains an unsupported field.' }
            $result[$key] = $Value[$key]
        }
    } else {
        foreach ($field in $Value.PSObject.Properties) {
            if ($field.Name -notin $allowed) { throw 'The setup instance contains an unsupported field.' }
            $result[$field.Name] = $field.Value
        }
    }
    $result
}

function Read-SdpResponse([string]$Path) {
    $response = Read-JsonFile $Path 64KB
    if ($null -eq $response.PSObject.Properties['schema'] -or $response.schema -ne 1) {
        throw 'Unsupported setup response schema.'
    }
    $allowed = @('schema','operation','profile','componentIds','installRoot','stateDirectory',
        'instance','adminUser','portableDirectory','allowDevelopment')
    $normalized = @{}
    foreach ($name in $allowed) { $normalized[$name] = $null }
    foreach ($field in $response.PSObject.Properties) {
        if ($field.Name -notin $allowed) { throw 'The setup response contains an unsupported field.' }
        $normalized[$field.Name] = $field.Value
    }
    $normalized
}

function Get-SdpSelection([hashtable]$Options) {
    $selection = @{ ManifestPath = $Options.ManifestPath; AllowDevelopment = [bool]$Options.AllowDevelopment }
    if ($Options.ComponentId) {
        if ($Options.Profile) { throw 'Choose a setup preset or a custom component selection.' }
        $selection.ComponentId = [string[]]$Options.ComponentId
    } else {
        if (!$Options.Profile) { throw 'Choose a setup preset or a custom component selection.' }
        $selection.Profile = $Options.Profile
    }
    Resolve-SdpPackagePlan @selection
}

function Get-SdpProductName([object]$Plan,[object]$Product) {
    $component = @($Plan.components | Where-Object { $_.productCode -eq $Product.productCode } | Select-Object -First 1)
    if ($component.Count -ne 1) { throw 'The selected product has no known application component.' }
    switch (($component[0].id -split '\.')[0]) {
        server { 'Server' }
        workbench { 'Workbench' }
        cli { 'CLI' }
        default { throw 'The selected MSI does not correspond to a StarDataParadise application.' }
    }
}

function Get-SdpInstallPaths([object]$Plan,[string]$Root) {
    $paths = @{}
    if ($Root -and ![IO.Path]::IsPathRooted($Root)) { throw 'Use an absolute program directory.' }
    foreach ($product in $Plan.products) {
        $name = Get-SdpProductName $Plan $product
        $existing = Get-SdpSetupProductState -Product $product
        if (!$Root -and $existing.installed) {
            $path = $existing.installPath
        } else {
            $base = if ($Root) { $Root } elseif ($product.requiresElevation) {
                Join-Path $env:ProgramFiles 'StarDataParadise'
            } else {
                Join-Path $env:LOCALAPPDATA 'Programs/StarDataParadise'
            }
            $path = Join-Path $base $name
        }
        $paths[$product.productCode] = [IO.Path]::GetFullPath($path)
    }
    $paths
}

function Invoke-SdpBinaryCheck([string]$Program,[string]$Argument) {
    if (!(Test-Path -LiteralPath $Program -PathType Leaf)) { throw 'An expected installed executable is missing.' }
    $spec = New-Object Diagnostics.ProcessStartInfo
    $spec.FileName = $Program
    $spec.Arguments = $Argument
    $spec.WorkingDirectory = Split-Path -Parent $Program
    $spec.UseShellExecute = $false
    $spec.CreateNoWindow = $true
    $spec.RedirectStandardOutput = $true
    $spec.RedirectStandardError = $true
    if ([IO.Path]::GetFileName($Program) -eq 'StarDataParadise.exe') {
        $spec.EnvironmentVariables['QT_QPA_PLATFORM'] = 'windows'
        $spec.EnvironmentVariables.Remove('QT_QPA_PLATFORM_PLUGIN_PATH')
        $spec.EnvironmentVariables.Remove('QT_PLUGIN_PATH')
    }
    $process = [Diagnostics.Process]::Start($spec)
    try {
        $out = $process.StandardOutput.ReadToEndAsync()
        $err = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(20000)) { $process.Kill(); throw 'The installed application health check timed out.' }
        if ($process.ExitCode -ne 0) { throw "Installed application health check returned $($process.ExitCode)." }
        $out.GetAwaiter().GetResult().Trim()
    } finally { $process.Dispose() }
}

function Install-SdpPortable([hashtable]$Options) {
    if (!$Options.PortableDirectory -or ![IO.Path]::IsPathRooted($Options.PortableDirectory) -or
        (Test-Path -LiteralPath $Options.PortableDirectory)) { throw 'Choose a new absolute directory for portable Workbench.' }
    $bundleRoot = Split-Path -Parent (Get-Item -LiteralPath $Options.ManifestPath).FullName
    $record = Read-JsonFile (Join-Path $bundleRoot 'offline-bundle.json') 64KB
    if ($record.schema -ne 1 -or $record.portable.file -ne 'Workbench-portable-win-x64.zip') { throw 'No portable Workbench archive is available.' }
    if (!$Options.AllowDevelopment) { throw 'The local development bundle requires explicit approval.' }
    $archive = Join-Path $bundleRoot $record.portable.file
    $item = Get-Item -LiteralPath $archive
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
        (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $record.portable.sha256) {
        throw 'Portable Workbench checksum mismatch.'
    }
    $destination = [IO.Path]::GetFullPath($Options.PortableDirectory)
    $temporary = $destination + '.staging-' + [guid]::NewGuid().ToString('n')
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    try {
        Expand-Archive -LiteralPath $archive -DestinationPath $temporary
        [void](Invoke-SdpBinaryCheck (Join-Path $temporary 'StarDataParadise.exe') '--smoke-test')
        [IO.Directory]::Move($temporary, $destination)
    } finally {
        if (Test-Path -LiteralPath $temporary -PathType Container) {
            $created = Get-Item -LiteralPath $temporary
            if ($created.FullName -ieq $temporary -and !(($created.Attributes) -band [IO.FileAttributes]::ReparsePoint)) {
                Remove-Item -LiteralPath $created.FullName -Recurse -Force
            }
        }
    }
    [pscustomobject]@{ ok = $true; action = 'Portable'; path = $destination; restartRequired = $false }
}

function Invoke-SdpSetup([hashtable]$Options) {
    if ($Options.Operation -eq 'Portable') { return Install-SdpPortable $Options }
    $plan = Get-SdpSelection $Options
    $paths = Get-SdpInstallPaths $plan $Options.InstallRoot
    $server = @($plan.products | Where-Object { (Get-SdpProductName $plan $_) -eq 'Server' } | Select-Object -First 1)
    $serverCode = $null
    $instanceFile = $null
    $instancePlan = $null
    $known = $null
    $instanceSettings = @{}
    if (![IO.Path]::IsPathRooted($Options.StateDirectory)) { throw 'Use an absolute setup state directory.' }
    $stateRoot = [IO.Path]::GetFullPath($Options.StateDirectory)
    if ((Test-Path -LiteralPath $stateRoot) -and
        ((Get-Item -LiteralPath $stateRoot).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'The setup state directory must not be a reparse point.'
    }
    if ($server.Count -eq 1) {
        $serverCode = $server[0].productCode
        $instanceFile = Join-Path $stateRoot (($serverCode -replace '[{}]','') + '.instance.json')
        $known = if (Test-Path -LiteralPath $instanceFile) { Read-JsonFile $instanceFile 64KB }
        $instanceSettings = if ($Options.Instance -and $Options.Instance.Count) {
            Convert-Instance $Options.Instance
        } elseif ($known) { Convert-Instance $known.settings } else { @{} }
        if ($instanceSettings.Count) {
            $config = $instanceSettings.Clone()
            $config.ProgramPath = Join-Path $paths[$serverCode] 'StarDataParadiseServer.exe'
            $config.Components = @($plan.components | ForEach-Object { $_.id })
            $instancePlan = New-SdpInstancePlan @config
        }
    }
    $preflight = Get-SdpSetupPreflight -Plan $plan -InstallRoot $Options.InstallRoot -Instance $instancePlan
    if (!$preflight.ok) { throw ('Setup preflight failed. ' + ($preflight.issues -join ' ')) }
    New-Item -ItemType Directory -Path $stateRoot -Force | Out-Null
    $logs = Join-Path $stateRoot 'logs'
    New-Item -ItemType Directory -Path $logs -Force | Out-Null
    $journal = Join-Path $stateRoot 'active-operation.json'
    if ($Options.Operation -eq 'Recover') {
        return Invoke-SdpSetupRecovery -Plan $plan -JournalPath $journal -LogDirectory $logs
    }
    $update = $Options.Operation -in @('Modify','Reconfigure')
    $configurationExists = $instancePlan -and (Test-Path -LiteralPath $instancePlan.configPath -PathType Leaf)
    if ($configurationExists -and $update) {
        [void](Test-SdpInstanceConfiguration -Plan $instancePlan)
    }
    $binaryCheck = ${function:Invoke-SdpBinaryCheck}
    $productName = ${function:Get-SdpProductName}
    $configurationCheck = Get-Command Test-SdpInstanceConfiguration
    $initializeInstance = Get-Command Initialize-SdpInstance
    $removal = {
        param($Product, $RemovedFeatures)
        if ($Product.productCode -ne $serverCode) { return }
        if ($RemovedFeatures -contains 'Decision') {
            if (!$instancePlan) { throw 'Select the existing Server instance to check active Decision dependencies.' }
            [void](& $configurationCheck -Plan $instancePlan -DisableDecision)
        }
    }.GetNewClosure()
    $change = @{ instance = $null }
    $validate = {
        foreach ($product in $plan.products) {
            $path = $paths[$product.productCode]
            switch (& $productName $plan $product) {
                Server {
                    $summary = & $binaryCheck (Join-Path $path 'StarDataParadiseServer.exe') '--self-test-install' | ConvertFrom-Json
                    if (!$summary.sql -or !$summary.transaction -or !$summary.history -or
                        (($product.features -contains 'DeepLearning') -and !$summary.mlp_compiled)) {
                        throw 'The installed Server SQL, transaction, History or selected module check failed.'
                    }
                }
                Workbench { [void](& $binaryCheck (Join-Path $path 'StarDataParadise.exe') '--smoke-test') }
                CLI { [void](& $binaryCheck (Join-Path $path 'StarDataParadiseCLI.exe') '--version') }
            }
        }
        if ($instancePlan -and ($update -or ($Options.Operation -eq 'Install' -and !$configurationExists))) {
            $setupParameters = @{ Plan = $instancePlan; AdminUser = $Options.AdminUser;
                AdminPassword = $Options.AdminPassword; UpdateConfiguration = $update }
            $change.instance = & $initializeInstance @setupParameters
        } elseif ($configurationExists) {
            [void](& $configurationCheck -Plan $instancePlan)
        }
    }.GetNewClosure()
    $arguments = @{ Plan = $plan; Action = $Options.Operation; InstallPaths = $paths; LogDirectory = $logs;
        JournalPath = $journal; ValidateRemoval = $removal }
    if ($Options.Operation -ne 'Uninstall') { $arguments.ValidateInstalled = $validate }
    if ($instancePlan) { $arguments.ReservedPaths = @($instancePlan.instanceRoot, $instancePlan.dataDirectory,
        $instancePlan.logDirectory, $instancePlan.tempDirectory, $instancePlan.backupDirectory) }
    if ($Options.OnProgress) { $arguments.OnProgress = $Options.OnProgress }
    if ($Options.IsCancelled) { $arguments.IsCancelled = $Options.IsCancelled }
    $serviceOwned = $known -and $known.serviceOwned -eq $true
    $firewallOwned = $known -and $known.firewallOwned -eq $true
    $removedIntegration = $false
    if ($Options.Operation -eq 'Uninstall' -and $instancePlan -and ($serviceOwned -or $firewallOwned)) {
        $state = Get-SdpSetupProductState -Product $server[0]
        if ($state.localFeatures -contains 'Decision') { & $removal $server[0] @('Decision') }
        Remove-SdpInstanceIntegration -Plan $instancePlan -ServiceOwned:$serviceOwned -FirewallOwned:$firewallOwned
        $removedIntegration = $true
    }
    try {
        $result = Invoke-SdpSetupActions @arguments
    } catch {
        if ($removedIntegration) {
            [void](Initialize-SdpInstance -Plan $instancePlan -UpdateConfiguration)
        }
        throw
    }
    if ($change.instance -or $removedIntegration) {
        $receipt = [ordered]@{ settings = $instanceSettings;
            serviceOwned = !$removedIntegration -and ($serviceOwned -or $change.instance.serviceInstalled);
            firewallOwned = !$removedIntegration -and ($firewallOwned -or $change.instance.firewallInstalled) }
        $receipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $instanceFile -Encoding UTF8
    }
    $result
}

try {
    if ($ResponseFile) {
        $response = Read-SdpResponse $ResponseFile
        if ($response.operation) { $Operation = $response.operation }
        if ($Operation -notin @('Install','Modify','Repair','Reconfigure','Uninstall','Recover','Portable')) { throw 'Unsupported setup operation.' }
        if ($response.profile) { $Profile = $response.profile }
        if ($response.componentIds) { $ComponentId = @($response.componentIds) }
        if ($response.installRoot) { $InstallRoot = $response.installRoot }
        if ($response.stateDirectory) { $StateDirectory = $response.stateDirectory }
        if ($response.instance) { $Instance = Convert-Instance $response.instance }
        if ($response.adminUser) { $AdminUser = $response.adminUser }
        if ($response.portableDirectory) { $PortableDirectory = $response.portableDirectory }
        if ($response.allowDevelopment -is [bool] -and $response.allowDevelopment) { $AllowDevelopment = $true }
        $Silent = $true
    }
    $options = @{ ManifestPath = $ManifestPath; Operation = $Operation; Profile = $Profile; ComponentId = $ComponentId;
        InstallRoot = $InstallRoot; StateDirectory = $StateDirectory; Instance = $Instance; AdminUser = $AdminUser;
        AdminPassword = $AdminPassword; AllowDevelopment = [bool]$AllowDevelopment; PortableDirectory = $PortableDirectory;
        OnProgress = $OnProgress; IsCancelled = $IsCancelled }
    if (!$Silent -or $UiSmoke) {
        Import-Module (Join-Path $PSScriptRoot 'SetupWindow.psm1') -Force
        Show-SdpSetupWindow -Options $options -ResolvePlan ${function:Get-SdpSelection} -Apply ${function:Invoke-SdpSetup} -Smoke:$UiSmoke
        exit 0
    }
    $result = Invoke-SdpSetup $options
    $result | ConvertTo-Json -Depth 8 -Compress
    if ($result.restartRequired) { exit 3010 }
    exit 0
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    if ($_.Exception -is [OperationCanceledException]) { exit 1602 }
    exit 2
}
