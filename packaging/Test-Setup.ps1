#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ManifestPath,
    [Parameter(Mandatory)][ValidatePattern('^(?:s45|m9-stage45)-[a-z0-9-]+$')][string]$TestFamily,
    [Parameter(Mandatory)][string]$Python,
    [Parameter(Mandatory)][string]$OpenSsl,
    [Parameter(Mandatory)][string]$RemoteServer,
    [Parameter(Mandatory)][string]$WorkbenchConnectionTest,
    [string]$Scratch = (Join-Path $PSScriptRoot '..\out\sdp-m9\stage45\validation')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'PackagePlan.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'SetupActions.psm1') -Force

function Require([bool]$Condition, [string]$Message) {
    if (!$Condition) { throw $Message }
}

function Invoke-TestChild([string]$Program, [string[]]$Arguments, [string]$InputLine = '') {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Program
    $start.WorkingDirectory = $run
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardError = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardInput = $true
    foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    $start.Environment['QT_QPA_PLATFORM'] = 'windows'
    foreach ($name in @('QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QML2_IMPORT_PATH')) {
        $null = $start.Environment.Remove($name)
    }
    $process = [Diagnostics.Process]::Start($start)
    try {
        if ($InputLine) { $process.StandardInput.WriteLine($InputLine) }
        $process.StandardInput.Close()
        $output = $process.StandardOutput.ReadToEndAsync()
        $errors = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(55000)) {
            $process.Kill($true)
            $process.WaitForExit()
            throw 'The lifecycle fixture process timed out.'
        }
        $combined = $output.GetAwaiter().GetResult() + $errors.GetAwaiter().GetResult()
        Require (!$combined.Contains($password)) 'A fixture password appeared in process output.'
        Require ($process.ExitCode -eq 0) "Lifecycle fixture failed with exit $($process.ExitCode): $combined"
        $combined.Trim()
    } finally { $process.Dispose() }
}

function Invoke-Wizard([hashtable]$Options, [int]$Expected = 0, [string]$Message = '') {
    # Match the normal Setup script's invocation in a fresh PowerShell process.
    Set-StrictMode -Off
    $script:actionNumber++
    $response = Join-Path $run "response-$actionNumber.json"
    $record = [ordered]@{ schema=1; installRoot=$programRoot; stateDirectory=$state; allowDevelopment=$true }
    foreach ($key in $Options.Keys) {
        if ($key -notin @('AdminPassword', 'IsCancelled')) { $record[$key] = $Options[$key] }
    }
    $record | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $response -Encoding utf8
    $argument = @{ ManifestPath = $ManifestPath; ResponseFile = $response; Silent = $true }
    if ($Options.ContainsKey('AdminPassword')) { $argument.AdminPassword = $Options.AdminPassword }
    if ($Options.ContainsKey('IsCancelled')) { $argument.IsCancelled = $Options.IsCancelled }
    $previous = [Console]::Error
    $errors = [IO.StringWriter]::new()
    try {
        [Console]::SetError($errors)
        $result = & $offlineSetup @argument
        $exitCode = $LASTEXITCODE
    } finally {
        [Console]::SetError($previous)
    }
    $problem = $errors.ToString().Trim()
    $errors.Dispose()
    Require ($problem -notlike "*$password*" -and ([string]$result) -notlike "*$password*") 'Setup exposed the fixture password.'
    Require ($exitCode -eq $Expected) "Setup $($record.operation) returned $exitCode instead of $Expected. $problem"
    if ($Message) { Require ($problem -match $Message) "Unexpected Setup rejection: $problem" }
    if ($Expected -eq 0) { [void]($result | ConvertFrom-Json -Depth 12) }
    Write-Output "Setup $($record.operation) returned $exitCode for response $actionNumber."
}

function Product-State([string]$Name) {
    Get-SdpSetupProductState -Product $products[$Name]
}

function Test-ServerFixture([string]$Mode) {
    $arguments = @('-B', $fixture, $Mode, '--server', $serverExe, '--config', $config, '--tls', $tls)
    Invoke-TestChild $Python $arguments $password
}

function Read-InstanceHashes {
    $directory = Join-Path $instanceRoot 'data'
    $result = [ordered]@{}
    foreach ($file in (Get-ChildItem -LiteralPath $directory -Recurse -File | Sort-Object FullName)) {
        if ($file.Name -eq '.sdp.lock') { continue }
        $name = [IO.Path]::GetRelativePath($directory, $file.FullName)
        $result[$name] = (Get-FileHash -LiteralPath $file.FullName).Hash
    }
    Require ($result.Count -gt 0) 'The live fixture has no persistent files.'
    $result | ConvertTo-Json -Compress
}

function Test-RemoteWorkbench([string]$AppRoot) {
    $stage = Join-Path $run 'remote-workbench'
    Copy-Item -LiteralPath $AppRoot -Destination $stage -Recurse
    $smoke = Join-Path $stage 'workbench_connection_smoke.exe'
    Copy-Item -LiteralPath $WorkbenchConnectionTest -Destination $smoke
    $driver = Join-Path $workbenchRepo 'tests/workbench_smoke.py'
    Invoke-TestChild $Python @('-B', $driver, '--client', $smoke, '--server', $RemoteServer,
        '--openssl', $OpenSsl, '--scratch', (Join-Path $run 'remote-fixture'))
}

$ManifestPath = (Resolve-Path -LiteralPath $ManifestPath).Path
$offlineSetup = (Resolve-Path -LiteralPath (Join-Path (Split-Path -Parent $ManifestPath) 'Setup.ps1')).Path
$Python = (Resolve-Path -LiteralPath $Python).Path
$OpenSsl = (Resolve-Path -LiteralPath $OpenSsl).Path
$RemoteServer = (Resolve-Path -LiteralPath $RemoteServer).Path
$WorkbenchConnectionTest = (Resolve-Path -LiteralPath $WorkbenchConnectionTest).Path
$workbenchRepo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../StarDataParadise')).Path
$fixture = Join-Path $PSScriptRoot '../tests/setup_lifecycle_fixture.py'
$run = (New-Item -ItemType Directory -Path (Join-Path $Scratch ([guid]::NewGuid().ToString('N'))) -Force).FullName
$programRoot = Join-Path $run 'program'
$state = Join-Path $run 'state'
$instanceRoot = Join-Path $run 'instance'
$config = Join-Path $instanceRoot 'server.json'
$tls = Join-Path $run 'tls'
$serverExe = Join-Path $programRoot 'Server/StarDataParadiseServer.exe'
$workbenchDir = Join-Path $programRoot 'Workbench'
$workbenchExe = Join-Path $workbenchDir 'StarDataParadise.exe'
$cliNative = Join-Path $programRoot 'CLI/StarDataParadiseDLLler.dll'
$password = [Convert]::ToHexString([Security.Cryptography.RandomNumberGenerator]::GetBytes(32))
$secure = ConvertTo-SecureString $password -AsPlainText -Force
$actionNumber = 0
$complete = Resolve-SdpPackagePlan -ManifestPath $ManifestPath -Profile complete -AllowDevelopment
$products = @{}
$locations = @{}
foreach ($item in $complete.products) {
    $isolated = !$item.requiresElevation -and [IO.Path]::GetFileName($item.artifactPath).EndsWith("-test-$TestFamily.msi")
    Require $isolated 'Tests require per-user MSI artifacts from their own TestFamily.'
    Require (!(Get-SdpSetupProductState -Product $item).installed) 'A reserved MSI product is already installed.'
    $component = $complete.components | Where-Object { $_.productCode -eq $item.productCode } | Select-Object -First 1
    $name = ($component.id -split '\.')[0]
    $products[$name] = $item
    $locations[$item.productCode] = Join-Path $programRoot (@{ server='Server'; workbench='Workbench'; cli='CLI' }[$name])
}
Require ($products.Count -eq 3) 'The lifecycle test bundle must have three independent applications.'
Write-Output "Lifecycle evidence $run"

try {
    $clientPlan = Resolve-SdpPackagePlan -ManifestPath $ManifestPath -ComponentId @('workbench.core', 'cli.core') -AllowDevelopment
    $first = $clientPlan.products[0]
    $observation = [pscustomobject]@{ FirstInstalled = $false }
    $cancel = {
        if ((Get-SdpSetupProductState -Product $first).installed) { $observation.FirstInstalled = $true }
        return $observation.FirstInstalled
    }.GetNewClosure()
    $cancelOptions = @{ operation='Install'; componentIds=@('workbench.core','cli.core'); IsCancelled=$cancel }
    Invoke-Wizard $cancelOptions -Expected 1602 -Message 'cancel'
    $rolledBack = $observation.FirstInstalled -and !(Product-State 'workbench').installed -and
        !(Product-State 'cli').installed -and !(Test-Path -LiteralPath $workbenchExe) -and !(Test-Path -LiteralPath $cliNative)
    Require $rolledBack 'Cancellation did not roll back a real completed MSI.'
    Require (!(Test-Path -LiteralPath (Join-Path $state 'active-operation.json'))) 'Successful rollback left a pending setup journal.'

    Invoke-Wizard @{ operation='Install'; profile='workbench-only' }
    $onlyWorkbench = (Product-State 'workbench').installed -and !(Product-State 'server').installed -and !(Product-State 'cli').installed
    Require $onlyWorkbench 'Workbench-only installation selected an extra local product.'
    Test-RemoteWorkbench $workbenchDir
    Write-Output 'S45-T03 Workbench-only installed as a normal user and used an independent TLS Server.'
    Invoke-Wizard @{ operation='Install'; profile='cli-only' }

    Invoke-TestChild $Python @('-B', $fixture, 'certificates', '--openssl', $OpenSsl, '--tls', $tls)
    $instance = @{
        instanceName='setup-test'; instanceRoot=$instanceRoot; tlsCertificate=(Join-Path $tls 'server.pem')
        tlsPrivateKey=(Join-Path $tls 'server.key'); tlsCa=(Join-Path $tls 'ca.pem'); port=0
    }
    $installServer = @{ operation='Install'; profile='server-only'; instance=$instance; AdminPassword=$secure }
    Invoke-Wizard $installServer
    Test-ServerFixture 'seed'
    Require ((Product-State 'server').localFeatures -contains 'Core') 'Server MSI Core did not install.'
    $cliDigest = (Get-FileHash -LiteralPath $cliNative).Hash
    Invoke-Wizard @{ operation='Uninstall'; profile='workbench-only' }
    $independent = !(Product-State 'workbench').installed -and !(Test-Path -LiteralPath $workbenchExe) -and
        (Product-State 'cli').installed -and (Get-FileHash -LiteralPath $cliNative).Hash -eq $cliDigest -and
        (Product-State 'server').installed
    Require $independent 'Workbench removal touched a separately owned product.'

    $installed = (Resolve-Path -LiteralPath $serverExe).Path
    Require ($installed.StartsWith($programRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) 'Repair target is outside its test installation.'
    Remove-Item -LiteralPath $serverExe
    Invoke-Wizard @{ operation='Repair'; profile='server-only' }
    Test-ServerFixture 'verify'

    Invoke-Wizard @{ operation='Reconfigure'; profile='server-only'; instance=$instance }
    Test-ServerFixture 'verify'
    $history = Read-InstanceHashes
    Invoke-Wizard @{ operation='Uninstall'; profile='server-only' }
    Require (!(Product-State 'server').installed -and !(Test-Path -LiteralPath $serverExe)) 'Server binaries did not uninstall.'
    Require ((Read-InstanceHashes) -ceq $history) 'Uninstall changed existing instance files.'

    Invoke-Wizard $installServer
    Test-ServerFixture 'verify'
    $cpu = @('server.core','server.ml-cpu')
    Invoke-Wizard @{ operation='Modify'; componentIds=$cpu }
    Require ((Product-State 'server').localFeatures -contains 'CpuLearning') 'Modify did not add the real MSI CPU feature.'
    Test-ServerFixture 'train'
    $history = Read-InstanceHashes
    Invoke-Wizard @{ operation='Modify'; profile='server-only' }
    $modelPreserved = (Product-State 'server').localFeatures -notcontains 'CpuLearning' -and (Read-InstanceHashes) -ceq $history
    Require $modelPreserved 'Removing CPU ML altered stored model or History files.'
    Invoke-Wizard @{ operation='Modify'; componentIds=@('server.core','server.ml-cpu','server.decision') }
    Test-ServerFixture 'model'
    Test-ServerFixture 'rule'
    Invoke-Wizard @{ operation='Modify'; componentIds=$cpu } -Expected 2 -Message 'Decision.*active|active.*Decision'
    Invoke-Wizard @{ operation='Uninstall'; profile='server-only' } -Expected 2 -Message 'Decision.*active|active.*Decision'
    Require ((Product-State 'server').localFeatures -contains 'Decision') 'A live Decision dependency was removed.'
    Test-ServerFixture 'verify'
    Write-Output 'S45-T01/T02 Offline install, feature changes, repair, rollback, independent removal, and instance/model/History preservation passed.'

    $portable = Join-Path $run 'portable'
    Invoke-Wizard @{ operation='Portable'; profile='workbench-only'; portableDirectory=$portable }
    Require ((Test-Path -LiteralPath (Join-Path $portable 'StarDataParadise.exe')) -and
        !(Product-State 'workbench').installed) 'Portable extraction installed an MSI or omitted Workbench.'
    Write-Output 'S45-T03 Portable Workbench passed its app-local Windows launch.'

    foreach ($file in (Get-ChildItem -LiteralPath $run -Recurse -File | Where-Object {
        $_.Name -like 'response-*.json' -or $_.Extension -eq '.log'
    })) {
        Require ((Get-Content -LiteralPath $file.FullName -Raw) -notlike "*$password*") 'Setup stored the fixture password in a response or log.'
    }
} finally {
    # Cleanup is restricted to this generated MSI family. Keep instance data as evidence.
    $cleanup = @{
        Plan=$complete; Action='Uninstall'; InstallPaths=$locations; LogDirectory=(Join-Path $run 'fixture-removal')
        JournalPath=(Join-Path $run 'fixture-cleanup.json'); ReservedPaths=@($instanceRoot)
        ValidateRemoval={ param($Product, $RemovedFeatures) }
    }
    [void](Invoke-SdpSetupActions @cleanup)
    foreach ($item in $complete.products) {
        Require (!(Get-SdpSetupProductState -Product $item).installed) 'The reserved MSI test product was not cleaned up.'
    }
    $secure.Dispose()
}
