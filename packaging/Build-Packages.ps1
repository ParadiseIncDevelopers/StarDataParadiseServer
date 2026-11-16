[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Wix,
    [Parameter(Mandatory)][string]$ServerRelease,
    [Parameter(Mandatory)][string]$WorkbenchRelease,
    [Parameter(Mandatory)][string]$CliRelease,
    [Parameter(Mandatory)][string]$NativeArchive,
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [Parameter(Mandatory)][string]$Dependencies,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$TestFamily,
    [switch]$TestPerUser
)

$ErrorActionPreference = 'Stop'
$server = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $server
$native = Join-Path $root 'StarDataParadiseDLLler'
$repos = [ordered]@{ server = $server; workbench = (Join-Path $root 'StarDataParadise'); cli = (Join-Path $root 'StarDataParadiseCli') }
$releases = @{ server = $ServerRelease; workbench = $WorkbenchRelease; cli = $CliRelease }
$recipes = @(foreach ($id in $repos.Keys) {
    $recipe = Get-Content -LiteralPath (Join-Path $repos[$id] 'packaging/payload.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($recipe.id -ne $id) { throw "Recipe product ID does not match $id." }
    $recipe
})
$sdk = Get-Content -LiteralPath (Join-Path $native 'packaging/client-runtime.json') -Raw -Encoding UTF8 | ConvertFrom-Json

function Get-Digest([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Copy-PayloadFile([string]$Source, [string]$FeatureRoot, [string]$Destination) {
    if ([IO.Path]::IsPathRooted($Destination) -or ($Destination -split '[\\/]+') -contains '..') {
        throw "Invalid payload path $Destination."
    }
    $item = Get-Item -LiteralPath $Source
    if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Payload must be a regular file: $Source"
    }
    $target = Join-Path $FeatureRoot $Destination
    if (Test-Path -LiteralPath $target) { throw "Duplicate payload file $Destination." }
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath $item.FullName -Destination $target
}

if ((Get-Digest $NativeArchive) -ne $sdk.archiveSha256) { throw 'Native archive SHA-256 mismatch.' }
if ($TestPerUser -and [string]::IsNullOrWhiteSpace($TestFamily)) { throw 'A per-user test build requires a test family.' }
$output = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $output -Force | Out-Null
if (Test-Path -LiteralPath (Join-Path $output 'components.json')) { throw 'Choose a new output directory.' }
$staging = Join-Path (Split-Path -Parent $output) ('payload-' + [guid]::NewGuid().ToString('n'))
New-Item -ItemType Directory -Path $staging | Out-Null
$nativeOutput = Join-Path $staging 'native'
Expand-Archive -LiteralPath $NativeArchive -DestinationPath $nativeOutput
$nativeDll = Join-Path $nativeOutput $sdk.runtime
$nativeHash = Get-Digest $nativeDll
$sdkArtifacts = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'sdk-artifacts.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$crtFiles = @('concrt140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll', 'msvcp140_atomic_wait.dll',
    'msvcp140_codecvt_ids.dll', 'msvcp140.dll', 'vccorlib140.dll', 'vcruntime140_1.dll',
    'vcruntime140_threads.dll', 'vcruntime140.dll')
$licenseFiles = [ordered]@{
    'openssl-3.5.9/LICENSE.txt' = 'OpenSSL-3.5.9.txt'
    'asio-asio-1-38-2/LICENSE_1_0.txt' = 'Asio-1.38.2.txt'
    'json/LICENSE.MIT' = 'nlohmann-json-3.12.0.txt'
}

$components = @(foreach ($recipe in $recipes) {
    $productRoot = Join-Path $staging $recipe.id
    $inputs = @{ repo = $repos[$recipe.id]; release = $releases[$recipe.id]; server = $server;
        sdk = $native; licenses = $PSScriptRoot }
    foreach ($feature in $recipe.features) {
        $featureRoot = Join-Path $productRoot $feature.id
        New-Item -ItemType Directory -Path $featureRoot -Force | Out-Null
        foreach ($file in $feature.files) {
            $inputRoot = $inputs[$file[0]]
            if (!$inputRoot) { throw "Unknown recipe input: $($file[0])" }
            Copy-PayloadFile (Join-Path $inputRoot $file[1]) $featureRoot $file[2]
        }
        if ($feature.id -eq 'Core') {
            foreach ($file in $crtFiles) { Copy-PayloadFile (Join-Path $RuntimeDirectory $file) $featureRoot $file }
            foreach ($entry in $licenseFiles.GetEnumerator()) {
                Copy-PayloadFile (Join-Path $Dependencies $entry.Key) $featureRoot ("docs/licenses/" + $entry.Value)
            }
            if ($recipe.id -eq 'workbench') {
                Copy-PayloadFile (Join-Path $Dependencies 'qt/sbom/qtbase-6.11.3.spdx.json') $featureRoot 'docs/licenses/qtbase-6.11.3.spdx.json'
            }
        }
        if ($feature.nativeRuntime) { Copy-PayloadFile $nativeDll $featureRoot $sdk.destination }
        if ($feature.sdkArchives) {
            Copy-PayloadFile $NativeArchive $featureRoot ('sdk/' + [IO.Path]::GetFileName($NativeArchive))
            foreach ($item in $sdkArtifacts) {
                $path = Join-Path (Join-Path $root $item.repo) $item.file
                if ((Get-Digest $path) -ne $item.sha256) { throw "SDK archive SHA-256 mismatch: $($item.file)" }
                Copy-PayloadFile $path $featureRoot ('sdk/' + [IO.Path]::GetFileName($path))
            }
        }
    }
    $package = & (Join-Path $PSScriptRoot 'New-SdpMsi.ps1') -Wix $Wix -Product $recipe -PayloadRoot $productRoot `
        -OutputDirectory $output -TestFamily $TestFamily -TestPerUser:$TestPerUser
    $artifact = Get-Item -LiteralPath $package.path
    $digest = Get-Digest $artifact.FullName
    $scope = if ($TestPerUser -or $recipe.scope -eq 'perUser') { 'per-user' } else { 'per-machine' }
    foreach ($feature in $recipe.features) {
        $requiredComponents = @(foreach ($id in @($feature.depends) + $(if ($feature.id -ne 'Core') { $recipe.features[0].component })) {
            if ($id) { [ordered]@{id = $id; version = $recipe.version} }
        })
        $ownership = @(if ($feature.nativeRuntime) {
            [ordered]@{mode='app-local'; scope=$recipe.id; path=$sdk.destination; version=$sdk.version; sha256=$nativeHash}
        })
        [ordered]@{
            id = $feature.component; version = $recipe.version; platform = 'windows'; architecture = 'x64'
            kind = 'msi'; feature = $feature.id; scope = $scope; productCode = $package.productCode
            artifact = $artifact.Name; sha256 = $digest; downloadBytes = $artifact.Length
            installedBytes = $package.featureInstalledBytes[$feature.id]
            signature = [ordered]@{policy = 'unsigned-development'; thumbprint = $null}
            dependencies = $requiredComponents; conflicts = @(); restart = 'none'; dataOwner = $feature.dataOwner
            ownership = $ownership
        }
    }
})

$manifest = [ordered]@{
    schema = 1; channel = 'development'
    profiles = [ordered]@{
        'server-only' = @('server.core')
        'workbench-only' = @('workbench.core')
        'cli-only' = @('cli.core')
        developer = @('workbench.migration', 'workbench.routine-export', 'workbench.sdk', 'workbench.docs', 'cli.docs')
        complete = @($components | ForEach-Object { $_.id })
    }
    components = $components
    unavailable = @('server.gpu')
}
$manifestPath = Join-Path $output 'components.json'
$manifest | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
Get-Item -LiteralPath $manifestPath
