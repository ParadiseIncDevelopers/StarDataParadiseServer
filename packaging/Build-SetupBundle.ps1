#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ManifestPath,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$Wix = (Join-Path $PSScriptRoot '..\out\sdp-m9\stage44\tools\wix.exe')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'PackagePlan.psm1') -Force
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Assert-BundlePart([string]$Name) {
    if ([string]::IsNullOrWhiteSpace($Name) -or $Name -in '.', '..' -or $Name.EndsWith('.') -or $Name.EndsWith(' ') -or
        $Name.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0 -or $Name -match '^(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)') {
        throw "Unsafe file or directory name in portable payload: $Name"
    }
}

function Get-BundleHash([string]$File) { return (Get-FileHash -LiteralPath $File -Algorithm SHA256).Hash }

function New-PortableWorkbench([string]$Msi, [string]$Image, [string]$Scratch, [string[]]$Features, [string]$NativeHash) {
    $info = Get-SdpMsiPackageInfo -Path $Msi
    $selected = @{}
    foreach ($feature in $Features) {
        if (-not $info.features.ContainsKey($feature)) { throw "The verified Workbench MSI does not contain '$feature'." }
        foreach ($entry in $info.features[$feature].files.Values) { $selected[$entry.id] = $entry }
    }
    $wxs = Join-Path $Scratch 'workbench.wxs'
    $cabinet = Join-Path $Scratch 'cabinet'
    $intermediate = Join-Path $Scratch 'intermediate'
    $details = & $wixPath 'msi' 'decompile' $Msi '-o' $wxs '-x' $cabinet '-intermediateFolder' $intermediate 2>&1
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $wxs -PathType Leaf)) {
        throw "WiX could not extract the verified Workbench MSI. $($details -join [Environment]::NewLine)"
    }

    $settings = [Xml.XmlReaderSettings]::new()
    $settings.DtdProcessing = [Xml.DtdProcessing]::Prohibit
    $settings.XmlResolver = $null
    $settings.MaxCharactersInDocument = 8MB
    $reader = [Xml.XmlReader]::Create($wxs, $settings)
    $document = [xml]::new()
    $document.XmlResolver = $null
    try { $document.Load($reader) } finally { $reader.Dispose() }
    $seen = @{}
    foreach ($file in $document.SelectNodes("//*[local-name()='File']")) {
        $id = $file.GetAttribute('Id')
        if (-not $selected.ContainsKey($id)) { continue }
        Assert-BundlePart $id
        $name = $file.GetAttribute('Name')
        Assert-BundlePart $name
        if ($selected[$id].name -cne $name -or $seen.ContainsKey($id)) {
            throw "Decompiled Workbench payload differs from the MSI File table for '$id'."
        }
        $parents = @()
        $parent = $file.ParentNode
        while ($parent -is [Xml.XmlElement] -and $parent.GetAttribute('Id') -cne 'INSTALLFOLDER') {
            if ($parent.LocalName -ceq 'Directory') {
                $directory = $parent.GetAttribute('Name')
                Assert-BundlePart $directory
                $parents = @($directory) + $parents
            }
            $parent = $parent.ParentNode
        }
        if ($parent -isnot [Xml.XmlElement]) { throw "Workbench payload '$id' is outside its application directory." }
        $destination = $Image
        foreach ($part in $parents) { $destination = Join-Path $destination $part }
        [void] [IO.Directory]::CreateDirectory($destination)
        $entry = Get-Item -LiteralPath (Join-Path $cabinet "File/$id") -Force
        if ($entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $entry.Length -ne $selected[$id].bytes) {
            throw "The embedded payload differs from the verified MSI File table for '$id'."
        }
        [IO.File]::Copy($entry.FullName, (Join-Path $destination $name))
        $seen[$id] = $true
    }
    if ($seen.Count -ne $selected.Count) { throw 'WiX extraction omitted one or more selected Workbench files.' }
    $required = @('StarDataParadise.exe', 'StarDataParadiseDLLler.dll', 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Network.dll',
        'Qt6Widgets.dll', 'platforms/qwindows.dll', 'vcruntime140.dll', 'msvcp140.dll')
    foreach ($path in $required) {
        if (-not (Test-Path -LiteralPath (Join-Path $Image $path) -PathType Leaf)) { throw "Portable Workbench runtime file missing: $path" }
    }
    if ((Get-BundleHash (Join-Path $Image 'StarDataParadiseDLLler.dll')) -ine $NativeHash) {
        throw 'Portable Workbench Native DLL does not match the manifest SHA-256.'
    }
}

$wixPath = (Get-Item -LiteralPath $Wix).FullName
$wixVersion = (& $wixPath --version | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $wixVersion -notmatch '^5\.0\.2(?:\+[^\s]+)?$') { throw 'Offline bundle assembly requires the pinned WiX 5.0.2.' }
$manifest = Get-Item -LiteralPath $ManifestPath
$complete = Resolve-SdpPackagePlan -ManifestPath $manifest.FullName -Profile 'complete' -AllowDevelopment
$core = @($complete.components | Where-Object id -CEQ 'workbench.core')
$migration = @($complete.components | Where-Object id -CEQ 'workbench.migration')
$products = @($complete.products)
if ($products.Count -ne 3 -or $core.Count -ne 1 -or $core[0].kind -cne 'msi' -or $migration.Count -gt 1) {
    throw 'The offline installer requires three MSI products and one Workbench Core feature.'
}
$workbench = @($products | Where-Object productCode -EQ $core[0].productCode)[0]
$features = @($core[0].feature)
if ($migration.Count) {
    if ($migration[0].productCode -ine $workbench.productCode) { throw 'Workbench Migration must be in its application MSI.' }
    $features += $migration[0].feature
}
$owner = @($complete.ownership | Where-Object { $_.scope -ceq 'workbench' -and $_.path -ceq 'StarDataParadiseDLLler.dll' })
if ($owner.Count -ne 1) { throw 'The manifest must identify the app-local Native DLL owned by Workbench.' }

$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
$archive = "$output.zip"
if ((Test-Path -LiteralPath $output) -or (Test-Path -LiteralPath $archive)) { throw 'Choose a new offline bundle output directory.' }
$parent = [IO.Path]::GetDirectoryName($output)
if (-not $parent) { throw 'The bundle cannot be produced directly at a drive root.' }
[void] [IO.Directory]::CreateDirectory($parent)
$scratch = Join-Path $parent ('.setup-build-' + [guid]::NewGuid().ToString('N'))
$stage = Join-Path $scratch 'bundle'
$image = Join-Path $scratch 'workbench'
$stageArchive = Join-Path $scratch 'offline.zip'
[void] [IO.Directory]::CreateDirectory($stage)
[void] [IO.Directory]::CreateDirectory($image)
try {
    foreach ($artifact in $complete.artifacts) {
        $relative = $artifact.artifact.Replace('/', [IO.Path]::DirectorySeparatorChar)
        $destination = Join-Path $stage $relative
        [void] [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination))
        [IO.File]::Copy($artifact.artifactPath, $destination)
        if ((Get-BundleHash $destination) -ine $artifact.sha256) { throw "Artifact changed while staging '$($artifact.artifact)'." }
    }
    [IO.File]::Copy($manifest.FullName, (Join-Path $stage 'components.json'))
    foreach ($file in @('Setup.ps1', 'SetupWindow.psm1', 'SetupActions.psm1', 'PackagePlan.psm1', 'InstancePlan.psm1', 'THIRD-PARTY.md', 'SETUP.md')) {
        [IO.File]::Copy((Join-Path $PSScriptRoot $file), (Join-Path $stage $file))
    }
    $portableName = 'Workbench-portable-win-x64.zip'
    $portable = Join-Path $stage $portableName
    New-PortableWorkbench (Join-Path $stage $workbench.artifact) $image $scratch $features $owner[0].sha256
    [IO.Compression.ZipFile]::CreateFromDirectory($image, $portable, [IO.Compression.CompressionLevel]::Optimal, $false)
    $portableHash = Get-BundleHash $portable
    @{ schema = 1; portable = @{ file = $portableName; sha256 = $portableHash } } | ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath (Join-Path $stage 'offline-bundle.json') -Encoding utf8
    $null = Resolve-SdpPackagePlan -ManifestPath (Join-Path $stage 'components.json') -Profile 'complete' -AllowDevelopment
    [IO.Compression.ZipFile]::CreateFromDirectory($stage, $stageArchive, [IO.Compression.CompressionLevel]::Optimal, $false)
    $archiveHash = Get-BundleHash $stageArchive
    [IO.Directory]::Move($stage, $output)
    [IO.File]::Move($stageArchive, $archive)
    [pscustomobject]@{ directory = $output; archive = $archive; sha256 = $archiveHash; portableSha256 = $portableHash }
}
finally {
    $item = Get-Item -LiteralPath $scratch -Force
    if ($item.PSIsContainer -and -not ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -and
        $item.Parent.FullName -ieq $parent -and $item.Name -match '^\.setup-build-[0-9a-f]{32}$') {
        Remove-Item -LiteralPath $item.FullName -Recurse -Force
    }
}
