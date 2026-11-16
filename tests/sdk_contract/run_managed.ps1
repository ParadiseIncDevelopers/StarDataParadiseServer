param(
    [Parameter(Mandatory)][ValidateSet('dotnet','java')][string] $Sdk,
    [Parameter(Mandatory)][ValidateSet('current','frozen')][string] $Mode,
    [Parameter(Mandatory)][string] $Build,
    [Parameter(Mandatory)][string] $Output,
    [Parameter(Mandatory)][string] $Server,
    [Parameter(Mandatory)][string] $OpenSsl,
    [Parameter(Mandatory)][string] $Contract,
    [string] $Python = 'python',
    [string] $JavaHome,
    [string] $FrozenArtifact,
    [string] $ExpiryHelper
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'managed_evidence.ps1')
$fixture = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../cross_sdk_fixture.py')).Path
$Build = (Resolve-Path -LiteralPath $Build).Path
$Server = (Resolve-Path -LiteralPath $Server).Path
$OpenSsl = (Resolve-Path -LiteralPath $OpenSsl).Path
$Contract = (Resolve-Path -LiteralPath $Contract).Path
if ($Sdk -eq 'java') { $JavaHome = (Resolve-Path -LiteralPath $JavaHome).Path }
if ($Mode -eq 'frozen') { $FrozenArtifact = (Resolve-Path -LiteralPath $FrozenArtifact).Path }
if ($ExpiryHelper) { $ExpiryHelper = (Resolve-Path -LiteralPath $ExpiryHelper).Path }
if (Test-Path -LiteralPath $Output) { throw 'Provide a fresh consumer output directory.' }
$Output = (New-Item -ItemType Directory -Path $Output).FullName
$source = Join-Path $Build 'source'

function Invoke-Fixture([string] $Name, [string[]] $Consumer) {
    $scratch = Join-Path $Output "$Name-fixture"
    $options = @('-B', $fixture, '--server', $Server, '--openssl', $OpenSsl, '--contract', $Contract,
                 '--scratch', $scratch, '--drop-op', 'tx_commit')
    if ($Mode -eq 'current') { $options += @('--drop-read', '--require-observed') }
    if ($ExpiryHelper) { $options += @('--expiry-helper', $ExpiryHelper) }
    Invoke-SdkEvidence $Output $Name $Python ($options + '--' + $Consumer)
}

if ($Sdk -eq 'dotnet') {
    $env:DOTNET_CLI_HOME = Join-Path $Output 'dotnet-home'
    $env:DOTNET_NOLOGO = '1'
    $env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
    $smoke = (New-Item -ItemType Directory -Path (Join-Path $Output 'smoke')).FullName
    Copy-Item -LiteralPath (Join-Path $source 'global.json') -Destination $Output
    foreach ($file in 'Smoke.csproj','App.config') {
        Copy-Item -LiteralPath (Join-Path $source "smoke/$file") -Destination $smoke
    }
    $baseProgram = Join-Path $source 'smoke/Program.cs'
    $program = Join-Path $smoke 'Program.cs'
    $dependencies = Join-Path $Build 'framework-feed'
    if ($Mode -eq 'current') {
        $packages = Join-Path $source 'out/packages'
        Invoke-SdkEvidence $Output 'prepare-adapter' $Python @(
            '-B', (Join-Path $PSScriptRoot 'prepare_managed_adapter.py'), 'dotnet', $baseProgram, $program
        )
    } else {
        $packages = $FrozenArtifact
        Copy-Item -LiteralPath $baseProgram -Destination $program
    }
    Push-Location -LiteralPath $Output
    try {
        foreach ($target in 'net10','net48') {
            $framework = $target -eq 'net48'
            $property = "-p:FrameworkFixture=$($framework.ToString().ToLowerInvariant())"
            $env:NUGET_PACKAGES = Join-Path $Output "$target-installed-deps"
            Invoke-SdkEvidence $Output "$target-restore" 'dotnet' @(
                'restore', 'smoke/Smoke.csproj', $property, '--source', $packages, '--source', $dependencies, '--force', '--nologo'
            )
            Invoke-SdkEvidence $Output "$target-build" 'dotnet' @(
                'build', 'smoke/Smoke.csproj', '-c', 'Release', $property, '--no-restore', '--nologo'
            )
            $consumer = if ($framework) {
                @((Join-Path $smoke 'bin/Release/net48/Smoke.exe'))
            } else {
                @('dotnet', (Join-Path $smoke 'bin/Release/net10.0/Smoke.dll'))
            }
            Invoke-Fixture "$target-tls" $consumer
        }
    } finally {
        Pop-Location
    }
    $testedArtifacts = Get-ChildItem -LiteralPath $packages -File -Filter '*.nupkg' | Where-Object {
        $_.Name -match '^StarDataParadise(?:Drive|NetFramework)\.2\.0\.0-preview\.1\.nupkg$'
    }
} else {
    $installed = Join-Path $Build 'installed'
    $gson = Join-Path $installed 'gson.jar'
    $consumer = Join-Path $installed 'consumer.jar'
    if ($Mode -eq 'current') {
        $jdbc = Join-Path $installed 'jdbc.jar'
        $adapter = Join-Path $Output 'Acceptance.java'
        $consumer = (New-Item -ItemType Directory -Path (Join-Path $Output 'classes')).FullName
        $baseProgram = Join-Path $source 'src/installed/java/com/stardataparadise/sdk/installed/Acceptance.java'
        Invoke-SdkEvidence $Output 'prepare-adapter' $Python @(
            '-B', (Join-Path $PSScriptRoot 'prepare_managed_adapter.py'), 'java', $baseProgram, $adapter
        )
        Invoke-SdkEvidence $Output 'javac-adapter' (Join-Path $JavaHome 'bin/javac.exe') @(
            '-encoding', 'UTF-8', '-Werror', '-classpath', "$jdbc;$gson", '-d', $consumer, $adapter
        )
    } else {
        $jdbc = $FrozenArtifact
    }
    Invoke-Fixture 'jdbc-tls' @(
        (Join-Path $JavaHome 'bin/java.exe'), '-classpath', "$consumer;$jdbc;$gson", 'com.stardataparadise.sdk.installed.Acceptance'
    )
    $testedArtifacts = Get-Item -LiteralPath $jdbc
}

$testedArtifacts | Sort-Object Name | ForEach-Object { "$((Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant())  $($_.Name)" } |
    Set-Content -LiteralPath (Join-Path $Output 'tested-SHA256SUMS.txt') -Encoding ascii