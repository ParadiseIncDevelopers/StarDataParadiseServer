Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'PackagePlan.psm1')

function ConvertTo-SetupProductCode([string]$Value) {
    $code = [guid]::Empty
    if (-not [guid]::TryParseExact($Value, 'B', [ref]$code) -or $code -eq [guid]::Empty) {
        throw 'A valid MSI product or upgrade code is required.'
    }
    return $code.ToString('B').ToUpperInvariant()
}

function Get-SdpSetupLocalPath([string]$Path) {
    if ($Path -notmatch '^[A-Za-z]:[\\/]' -or $Path -match '["\x00-\x1f]') {
        throw 'An absolute, local Windows path is required.'
    }
    $full = [IO.Path]::GetFullPath($Path)
    if ($full -eq [IO.Path]::GetPathRoot($full)) { throw 'A drive root cannot be used here.' }
    $full = $full.TrimEnd([char[]]@('\', '/'))
    $cursor = $full
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "A setup path is a reparse point: $cursor"
            }
        }
        $cursor = [IO.Path]::GetDirectoryName($cursor)
    }
    return $full
}

function Test-SdpSetupPathOverlap([string]$First, [string]$Second) {
    return $First.Equals($Second, [StringComparison]::OrdinalIgnoreCase) -or
        $First.StartsWith($Second + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $Second.StartsWith($First + '\', [StringComparison]::OrdinalIgnoreCase)
}

function Invoke-SetupInstallerMember($Installer, [string]$Name, [object[]]$Arguments = @()) {
    return ,($Installer.GetType().InvokeMember($Name, [Reflection.BindingFlags]::GetProperty, $null, $Installer, $Arguments))
}

function Remove-SetupComReference($Instance) {
    if ($null -ne $Instance -and [Runtime.InteropServices.Marshal]::IsComObject($Instance)) {
        [void] [Runtime.InteropServices.Marshal]::FinalReleaseComObject($Instance)
    }
}

function Get-SdpSetupProductState {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)]$Product)

    $code = ConvertTo-SetupProductCode $Product.productCode
    $metadata = Get-SdpMsiPackageInfo -Path $Product.artifactPath
    if ($metadata.productCode -ine $code -or $metadata.scope -cne $Product.scope -or $metadata.architecture -ine 'x64') {
        throw "The MSI package identity or scope changed for $code."
    }
    $upgradeCode = ConvertTo-SetupProductCode $metadata.upgradeCode
    $features = @($metadata.features.Keys | Sort-Object)
    foreach ($feature in $features) {
        if ($feature -cnotmatch '^[A-Za-z_][A-Za-z0-9_.]{0,37}$') { throw "Invalid MSI feature ID for $code." }
    }
    $installer = $related = $null
    try {
        $installer = New-Object -ComObject WindowsInstaller.Installer
        $state = [int](Invoke-SetupInstallerMember $installer 'ProductState' @($code))
        if ($state -notin -1, 5) { throw "MSI product $code is registered in an unsupported state ($state)." }
        $installed = $state -eq 5
        $path = ''
        $local = @()
        if ($installed) {
            $actualVersion = [string](Invoke-SetupInstallerMember $installer 'ProductInfo' @($code, 'VersionString'))
            if ($actualVersion -cne $metadata.version) { throw "Installed MSI version mismatch for $code." }
            $location = [string](Invoke-SetupInstallerMember $installer 'ProductInfo' @($code, 'InstallLocation'))
            if ($location) { $path = Get-SdpSetupLocalPath $location }
            foreach ($feature in $features) {
                $featureState = [int](Invoke-SetupInstallerMember $installer 'FeatureState' @($code, [string]$feature))
                if ($featureState -eq 3) { $local += $feature }
                elseif ($featureState -ne 2) { throw "Unsupported installed state $featureState for MSI $code feature '$feature'." }
            }
            if (-not $local.Count) { throw "No local features are registered for MSI $code." }
        }
        $family = @()
        $related = Invoke-SetupInstallerMember $installer 'RelatedProducts' @($upgradeCode)
        $count = [int](Invoke-SetupInstallerMember $related 'Count')
        for ($index = 0; $index -lt $count; $index++) {
            $other = [string](Invoke-SetupInstallerMember $related 'Item' @($index))
            if ($other -and $other -ine $code) { $family += ConvertTo-SetupProductCode $other }
        }
        [pscustomobject]@{ productCode = $code; upgradeCode = $upgradeCode; scope = $metadata.scope; installed = $installed;
            installPath = $path; localFeatures = @($local); allFeatures = $features; relatedProductCodes = $family;
            dataOwner = $Product.dataOwner; artifactPath = $Product.artifactPath }
    }
    finally {
        Remove-SetupComReference $related
        Remove-SetupComReference $installer
    }
}

function Test-SetupFeatures([string[]]$First, [string[]]$Second) {
    if ($First.Count -ne $Second.Count) { return $false }
    foreach ($feature in $First) { if ($Second -cnotcontains $feature) { return $false } }
    return $true
}

function Get-SetupArtifactHash($Plan, $Product) {
    $records = @($Plan.artifacts | Where-Object { $_.artifactPath -ieq $Product.artifactPath -and $_.kind -ceq 'msi' })
    if ($records.Count -ne 1 -or $records[0].sha256 -notmatch '^[0-9a-fA-F]{64}$') {
        throw "No verified offline artifact record exists for $($Product.productCode)."
    }
    return $records[0].sha256.ToUpperInvariant()
}

function Assert-SetupArtifact($Product, [string]$ExpectedHash) {
    $artifact = Get-SdpSetupLocalPath $Product.artifactPath
    if ((Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash -ine $ExpectedHash) {
        throw "MSI SHA-256 changed since the offline plan was verified: $($Product.productCode)."
    }
    return $artifact
}

function New-SetupMsiArguments($Product, [string]$Action, [string]$InstallPath, [string]$Artifact, [string]$Log,
    [string[]]$Desired, [string[]]$Current) {

    $arguments = if ($Action -eq 'Uninstall') { '/x ' + (ConvertTo-SetupProductCode $Product.productCode) }
        else { '/i "' + $Artifact + '"' }
    $arguments += ' /qn /norestart REBOOT=ReallySuppress /L*v "' + $Log + '"'
    $arguments += ' INSTALLFOLDER="' + $InstallPath + '" ARPINSTALLLOCATION="' + $InstallPath + '"'
    if ($Action -eq 'Uninstall') { return $arguments }
    if ($Action -eq 'Repair') { return $arguments + ' REINSTALL=' + ($Current -join ',') + ' REINSTALLMODE=amus' }
    $arguments += ' ADDLOCAL=' + ($Desired -join ',')
    $removed = @($Current | Where-Object { $Desired -cnotcontains $_ })
    if ($removed.Count) { $arguments += ' REMOVE=' + ($removed -join ',') }
    return $arguments
}

function Invoke-SetupMsiCommand($Product, [string]$Action, [string]$InstallPath, [string]$Hash, [string]$Log,
    [string[]]$Desired, [string[]]$Current) {

    $artifact = Assert-SetupArtifact $Product $Hash
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = Join-Path ([Environment]::SystemDirectory) 'msiexec.exe'
    $start.Arguments = New-SetupMsiArguments $Product $Action $InstallPath $artifact $Log $Desired $Current
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $process = [Diagnostics.Process]::Start($start)
    try {
        $process.WaitForExit()
        $code = $process.ExitCode
    }
    finally { $process.Dispose() }
    if ($code -notin 0, 3010) { throw "Windows Installer $Action for $($Product.productCode) exited $code. See $Log" }
    return ($code -eq 3010)
}

function Write-SetupJournal([string]$Path, $Journal) {
    $suffix = [guid]::NewGuid().ToString('N')
    $temporary = "$Path.$suffix.tmp"
    $backup = "$Path.$suffix.previous"
    $data = [Text.UTF8Encoding]::new($false).GetBytes(($Journal | ConvertTo-Json -Depth 8))
    try {
        $stream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try {
            $stream.Write($data, 0, $data.Length)
            $stream.Flush($true)
        }
        finally { $stream.Dispose() }
        if ([IO.File]::Exists($Path)) { [IO.File]::Replace($temporary, $Path, $backup) }
        else { [IO.File]::Move($temporary, $Path) }
    }
    finally {
        if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
        if ([IO.File]::Exists($backup)) { [IO.File]::Delete($backup) }
    }
}

function Enter-SetupLock([string]$JournalPath) {
    [void] [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($JournalPath))
    $lockPath = Get-SdpSetupLocalPath "$JournalPath.lock"
    try { return [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None) }
    catch { throw 'Another setup or recovery operation already uses this journal.' }
}

function Assert-SetupAfter($Product, $After, [bool]$ShouldBeInstalled, [string]$Path, [string[]]$Features) {
    if ($After.installed -ne $ShouldBeInstalled -or ($ShouldBeInstalled -and
        ($After.installPath -ine $Path -or -not (Test-SetupFeatures @($After.localFeatures) $Features)))) {
        throw "Windows Installer did not report the expected install path or local features for $($Product.productCode)."
    }
}

function Restore-SetupJournal($Plan, $Journal, [string]$LogDirectory) {
    if ($Journal.schema -ne 1 -or $Journal.id -notmatch '^[0-9a-f]{32}$' -or
        @($Journal.products).Count -gt 64 -or @($Journal.attempted).Count -gt @($Journal.products).Count) {
        throw 'Setup recovery journal has an unsupported format.'
    }
    $products = @{}
    foreach ($product in $Plan.products) { $products[(ConvertTo-SetupProductCode $product.productCode)] = $product }
    $snapshots = @{}
    foreach ($entry in $Journal.products) {
        $code = ConvertTo-SetupProductCode $entry.productCode
        if ($snapshots.ContainsKey($code) -or -not $products.ContainsKey($code) -or $entry.scope -cne $products[$code].scope -or
            $entry.sha256 -ine (Get-SetupArtifactHash $Plan $products[$code]) -or $entry.installed -isnot [bool]) {
            throw "Setup recovery requires the same verified product bundle and scope for $code."
        }
        $path = Get-SdpSetupLocalPath $entry.installPath
        $state = Get-SdpSetupProductState $products[$code]
        foreach ($feature in @($entry.localFeatures)) {
            if ($state.allFeatures -cnotcontains $feature) { throw "A recorded MSI feature no longer exists for $code." }
        }
        if ($entry.installed -and -not @($entry.localFeatures).Count) { throw "No prior features were recorded for $code." }
        $snapshots[$code] = [pscustomobject]@{ product = $products[$code]; entry = $entry; path = $path }
    }
    $attempted = @($Journal.attempted)
    if (@($attempted | Select-Object -Unique).Count -ne $attempted.Count) { throw 'Duplicate setup recovery records.' }
    $failed = @()
    $restart = $false
    for ($index = $attempted.Count - 1; $index -ge 0; $index--) {
        $code = ConvertTo-SetupProductCode $attempted[$index]
        if (-not $snapshots.ContainsKey($code)) { throw "Setup recovery references an unowned product: $code" }
        $snapshot = $snapshots[$code]
        try {
            $product = $snapshot.product
            $entry = $snapshot.entry
            $before = Get-SdpSetupProductState $product
            if ($before.installed -and $before.installPath -ine $snapshot.path) {
                throw "The observed install path for $code does not match the recorded path."
            }
            if ($before.installed -eq $entry.installed -and (Test-SetupFeatures @($before.localFeatures) @($entry.localFeatures))) { continue }
            if (-not $before.installed -and @($before.relatedProductCodes).Count) {
                throw "A different MSI product in this family prevents restoring $code."
            }
            $action = if (-not $entry.installed) { 'Uninstall' } elseif ($before.installed) { 'Modify' } else { 'Install' }
            $log = Join-Path $LogDirectory ("$($Journal.id)-rollback-$index.log")
            if (Invoke-SetupMsiCommand $product $action $snapshot.path $entry.sha256 $log @($entry.localFeatures) @($before.localFeatures)) {
                $restart = $true
            }
            Assert-SetupAfter $product (Get-SdpSetupProductState $product) $entry.installed $snapshot.path @($entry.localFeatures)
        }
        catch { $failed += $_.Exception.Message }
    }
    if ($failed.Count) { throw ('Setup recovery is incomplete. ' + ($failed -join ' ')) }
    return $restart
}

function Invoke-SdpSetupRecovery {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)]$Plan, [Parameter(Mandatory = $true)][string]$JournalPath,
        [Parameter(Mandatory = $true)][string]$LogDirectory)

    $ErrorActionPreference = 'Stop'
    $journalFile = Get-SdpSetupLocalPath $JournalPath
    $logs = Get-SdpSetupLocalPath $LogDirectory
    [void] [IO.Directory]::CreateDirectory($logs)
    $lock = Enter-SetupLock $journalFile
    try {
        if (-not [IO.File]::Exists($journalFile)) { return [pscustomobject]@{ recovered = $false; restartRequired = $false } }
        $file = Get-Item -LiteralPath $journalFile
        if ($file.Length -gt 256KB) { throw 'Setup recovery journal is too large.' }
        $journal = Get-Content -LiteralPath $journalFile -Raw -Encoding UTF8 | ConvertFrom-Json
        $restart = Restore-SetupJournal $Plan $journal $logs
        [IO.File]::Delete($journalFile)
        return [pscustomobject]@{ recovered = $true; restartRequired = $restart }
    }
    finally { $lock.Dispose() }
}

function Invoke-SdpSetupActions {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]$Plan,
        [Parameter(Mandatory = $true)][ValidateSet('Install', 'Modify', 'Repair', 'Reconfigure', 'Uninstall')][string]$Action,
        [System.Collections.IDictionary]$InstallPaths = @{},
        [Parameter(Mandatory = $true)][string]$LogDirectory,
        [Parameter(Mandatory = $true)][string]$JournalPath,
        [string[]]$ReservedPaths = @(),
        [scriptblock]$OnProgress,
        [scriptblock]$IsCancelled,
        [scriptblock]$ValidateRemoval,
        [scriptblock]$ValidateInstalled
    )

    $ErrorActionPreference = 'Stop'
    $journalFile = Get-SdpSetupLocalPath $JournalPath
    $logs = Get-SdpSetupLocalPath $LogDirectory
    [void] [IO.Directory]::CreateDirectory($logs)
    $lock = Enter-SetupLock $journalFile
    try {
        if ([IO.File]::Exists($journalFile)) { throw 'A previous setup operation needs recovery before a new operation can start.' }
        $reserved = @($journalFile, $logs)
        foreach ($path in $ReservedPaths) { $reserved += Get-SdpSetupLocalPath $path }
        $steps = @()
        $products = @($Plan.products)
        if ($products.Count -gt 64) { throw 'Too many MSI products in one operation.' }
        if ($Action -eq 'Uninstall') { [array]::Reverse($products) }
        $planned = @{}
        foreach ($product in $products) {
            $hash = Get-SetupArtifactHash $Plan $product
            $null = Assert-SetupArtifact $product $hash
            $before = Get-SdpSetupProductState $product
            if ($planned.ContainsKey($before.productCode)) { throw "An MSI product is duplicated in the plan: $($before.productCode)" }
            $planned[$before.productCode] = $true
            if (-not $before.installed -and $Action -eq 'Uninstall') { continue }
            $path = $before.installPath
            $requestedPath = [string]$InstallPaths[$before.productCode]
            if ($requestedPath) {
                $requestedPath = Get-SdpSetupLocalPath $requestedPath
                if ($before.installed -and $path -ine $requestedPath) { throw "Installed location cannot be changed for $($before.productCode)." }
                $path = $requestedPath
            }
            if (-not $path) { throw "No verified owned install path is available for $($before.productCode)." }
            foreach ($other in $reserved) {
                if (Test-SdpSetupPathOverlap $path $other) { throw "The install path overlaps protected data or another setup path: $path" }
            }
            $reserved += $path
            if (-not $before.installed) {
                if ($Action -ne 'Install') { throw "$Action requires an existing MSI product: $($before.productCode)" }
                if (@($before.relatedProductCodes).Count) { throw "A different version already exists in MSI family $($before.upgradeCode)." }
                if ((Test-Path -LiteralPath $path) -and ((-not (Test-Path -LiteralPath $path -PathType Container)) -or
                    @(Get-ChildItem -LiteralPath $path -Force | Select-Object -First 1).Count)) {
                    throw "Fresh MSI installation requires an empty install directory: $path"
                }
            }
            $desired = @($product.features)
            if ($Action -in 'Repair', 'Reconfigure') { $desired = @($before.localFeatures) }
            if ($Action -eq 'Uninstall') { $desired = @() }
            foreach ($feature in $desired) {
                if ($before.allFeatures -cnotcontains $feature) { throw "Unknown MSI feature '$feature' for $($before.productCode)." }
            }
            if ($Action -ne 'Uninstall' -and (-not $desired.Count -or @($desired | Select-Object -Unique).Count -ne $desired.Count)) {
                throw "The MSI selection is empty or duplicated for $($before.productCode)."
            }
            $sameFeatures = Test-SetupFeatures @($before.localFeatures) $desired
            if ($Action -eq 'Install' -and $before.installed -and -not $sameFeatures) {
                throw "Use Modify to change installed features for $($before.productCode)."
            }
            $removed = @($before.localFeatures | Where-Object { $desired -cnotcontains $_ })
            if ($removed.Count) {
                if ($ValidateRemoval) { $null = & $ValidateRemoval $product $removed }
                elseif ($before.dataOwner -ceq 'instance' -and $removed -ccontains 'Decision') {
                    throw 'An active Decision rule check is required before removing this server feature.'
                }
            }
            $run = $Action -eq 'Repair' -or ($Action -ne 'Reconfigure' -and (-not $sameFeatures -or -not $before.installed))
            $steps += [pscustomobject]@{ product = $product; before = $before; path = $path; sha256 = $hash; desired = $desired; run = $run }
        }
        $journal = [pscustomobject]@{ schema = 1; id = [guid]::NewGuid().ToString('N'); action = $Action; attempted = @();
            products = @($steps | ForEach-Object { [pscustomobject]@{ productCode = $_.before.productCode; scope = $_.before.scope;
                sha256 = $_.sha256; installed = $_.before.installed; installPath = $_.path; localFeatures = @($_.before.localFeatures) } }) }
        $logsWritten = @()
        $restart = $false
        $after = @()
        Write-SetupJournal $journalFile $journal
        try {
            for ($index = 0; $index -lt $steps.Count; $index++) {
                if ($IsCancelled -and (& $IsCancelled)) { throw [OperationCanceledException]::new('Setup was cancelled.') }
                $step = $steps[$index]
                $product = $step.product
                $code = $step.before.productCode
                Assert-SetupAfter $product (Get-SdpSetupProductState $product) $step.before.installed $step.path @($step.before.localFeatures)
                if ($step.run) {
                    if ($OnProgress) { $null = & $OnProgress ([pscustomobject]@{ phase = 'msi'; productCode = $code; completed = $index; total = $steps.Count; action = $Action }) }
                    $journal.attempted += $code
                    Write-SetupJournal $journalFile $journal
                    $log = Join-Path $logs ("$($journal.id)-$index.log")
                    $logsWritten += $log
                    if (Invoke-SetupMsiCommand $product $Action $step.path $step.sha256 $log $step.desired @($step.before.localFeatures)) {
                        $restart = $true
                    }
                }
                $state = Get-SdpSetupProductState $product
                Assert-SetupAfter $product $state ($Action -ne 'Uninstall') $step.path $step.desired
                $after += $state
            }
            if ($IsCancelled -and (& $IsCancelled)) { throw [OperationCanceledException]::new('Setup was cancelled.') }
            if ($OnProgress) { $null = & $OnProgress ([pscustomobject]@{ phase = 'validate'; completed = $steps.Count; total = $steps.Count; action = $Action }) }
            if ($ValidateInstalled) { $null = & $ValidateInstalled $after }
            [IO.File]::Delete($journalFile)
            return [pscustomobject]@{ action = $Action; restartRequired = $restart; products = $after; logs = $logsWritten }
        }
        catch {
            $operationError = $_
            try {
                $null = Restore-SetupJournal $Plan $journal $logs
                [IO.File]::Delete($journalFile)
            }
            catch { throw "$($operationError.Exception.Message) $($_.Exception.Message) Recovery journal: $journalFile" }
            throw $operationError
        }
    }
    finally { $lock.Dispose() }
}

Export-ModuleMember -Function Get-SdpSetupProductState, Invoke-SdpSetupActions, Invoke-SdpSetupRecovery
