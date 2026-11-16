param(
    [Parameter(Mandatory)][ValidateSet('dotnet','java')][string] $Sdk,
    [Parameter(Mandatory)][string] $Repository,
    [Parameter(Mandatory)][string] $Output,
    [string] $FrameworkFeed,
    [string] $Gradle,
    [string] $JavaHome,
    [string] $JavaDependencies
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'managed_evidence.ps1')
$Repository = (Resolve-Path -LiteralPath $Repository).Path
if (Test-Path -LiteralPath $Output) { throw 'Provide a fresh build output directory.' }
if ($Sdk -eq 'dotnet') {
    $FrameworkFeed = (Resolve-Path -LiteralPath $FrameworkFeed).Path
} else {
    $Gradle = (Resolve-Path -LiteralPath $Gradle).Path
    $JavaHome = (Resolve-Path -LiteralPath $JavaHome).Path
    $JavaDependencies = (Resolve-Path -LiteralPath $JavaDependencies).Path
}

$Output = (New-Item -ItemType Directory -Path $Output).FullName
$revision = & git -C $Repository rev-parse --verify HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve SDK HEAD.' }
Set-Content -LiteralPath (Join-Path $Output 'revision.txt') -Value $revision -Encoding ascii
$archive = Join-Path $Output 'source.zip'
$source = Join-Path $Output 'source'
Invoke-SdkEvidence $Output 'archive' 'git' @('-C', $Repository, 'archive', '--format=zip', '-o', $archive, $revision)
Expand-Archive -LiteralPath $archive -DestinationPath $source

Push-Location -LiteralPath $source
try {
    if ($Sdk -eq 'dotnet') {
        $env:DOTNET_CLI_HOME = Join-Path $Output 'dotnet-home'
        $env:DOTNET_NOLOGO = '1'
        $env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
        $packages = (New-Item -ItemType Directory -Path 'out/packages').FullName
        $dependencyCache = (New-Item -ItemType Directory -Path (Join-Path $Output 'framework-feed')).FullName
        $dependencies = @(Get-ChildItem -LiteralPath $FrameworkFeed -Recurse -File -Filter '*.nupkg')
        if (!$dependencies) { throw 'Framework feed contains no local packages.' }
        foreach ($dep in $dependencies) { Copy-Item -LiteralPath $dep.FullName -Destination $dependencyCache }
        Invoke-SdkEvidence $Output 'dotnet-version' 'dotnet' @('--version')
        foreach ($target in 'net10','net48') {
            $project = if ($target -eq 'net48') { 'framework/StarDataParadiseNetFramework.csproj' } else { 'src/StarDataParadise.SDK.NET.csproj' }
            $env:NUGET_PACKAGES = Join-Path $Output "$target-build-deps"
            Invoke-SdkEvidence $Output "$target-package-restore" 'dotnet' @('restore', $project, '--source', $dependencyCache, '--nologo')
            Invoke-SdkEvidence $Output "$target-pack" 'dotnet' @('pack', $project, '-c', 'Release', '--no-restore', '-o', $packages, '--nologo')
        }
        $outputs = Get-ChildItem -LiteralPath $packages -File -Filter '*.nupkg'
        if ($outputs.Count -ne 2) { throw 'Expected two NuGet provider packages.' }
    } else {
        $env:JAVA_HOME = $JavaHome
        $env:GRADLE_USER_HOME = Join-Path $Output 'gradle-home'
        Invoke-SdkEvidence $Output 'jdk-version' (Join-Path $JavaHome 'bin/java.exe') @('-version')
        Invoke-SdkEvidence $Output 'gradle-build-package' $Gradle @(
            '--offline', '--no-daemon', '--max-workers=1', '--project-cache-dir', 'out/gradle-project',
            "-PsdpDependencyDir=$JavaDependencies", "-Dorg.gradle.java.home=$JavaHome",
            'build', 'publishSdkPublicationToSdkArtifactsRepository'
        )
        $maven = Join-Path $source 'out/maven/com/stardataparadise/stardataparadise-jdbc/0.2.0'
        $outputs = Get-ChildItem -LiteralPath $maven -File | Where-Object { $_.Extension -in '.jar','.pom','.module' }
        if ($outputs.Count -ne 5) { throw 'Expected five Java Maven artifacts.' }
        $jar = Join-Path $maven 'stardataparadise-jdbc-0.2.0.jar'
        $compiledJar = Join-Path $source 'out/build/libs/StarDataParadise.SDK.Java-0.2.0.jar'
        if ((Get-FileHash -LiteralPath $jar).Hash -ne (Get-FileHash -LiteralPath $compiledJar).Hash) { throw 'Published main JAR differs from the built JAR.' }
        $installed = (New-Item -ItemType Directory -Path (Join-Path $Output 'installed')).FullName
        Copy-Item -LiteralPath $jar -Destination (Join-Path $installed 'jdbc.jar')
        Copy-Item -LiteralPath (Join-Path $JavaDependencies 'gson-2.13.2.jar') -Destination (Join-Path $installed 'gson.jar')
        Copy-Item -LiteralPath (Join-Path $source 'out/build/libs/StarDataParadise.SDK.Java-0.2.0-installed-consumer.jar') -Destination (Join-Path $installed 'consumer.jar')
    }
    $outputs | Sort-Object Name | ForEach-Object { "$((Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant())  $($_.Name)" } |
        Set-Content -LiteralPath (Join-Path $Output 'SHA256SUMS.txt') -Encoding ascii
} finally {
    Pop-Location
}