[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $Wix,
    [Parameter(Mandatory)][object] $Product,
    [Parameter(Mandatory)][string] $PayloadRoot,
    [Parameter(Mandatory)][string] $OutputDirectory,
    [string] $TestFamily,
    [switch] $TestPerUser
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-AbsolutePath([string] $Path, [string] $Field) {
    if ($Path -notmatch '^(?:[A-Za-z]:[\\/]|\\\\[^\\]+\\[^\\]+\\)') {
        throw "$Field must be an absolute Windows path."
    }
    return [IO.Path]::GetFullPath($Path)
}

function Assert-PlainDirectory($Directory) {
    if (-not $Directory.PSIsContainer -or ($Directory.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "A regular directory is required: $($Directory.FullName)"
    }
}

function Assert-PathPart([string] $Name) {
    if ([string]::IsNullOrWhiteSpace($Name) -or $Name -in '.', '..' -or $Name.EndsWith('.') -or $Name.EndsWith(' ') -or
        $Name.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0 -or $Name -match '^(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)') {
        throw "Invalid Windows install path segment: $Name"
    }
}

function Get-IdentityHash([string] $Value) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Value))).Replace('-', '').ToLowerInvariant()
    }
    finally { $sha.Dispose() }
}

function New-IdentityGuid([string] $Identity) {
    # Namespace bytes are in RFC UUID order, independent of Windows GUID memory layout.
    [byte[]] $namespace = 0x20, 0xd3, 0x25, 0xc2, 0x2d, 0x30, 0x48, 0xde, 0x9a, 0xc9, 0x74, 0xd5, 0x51, 0x93, 0x31, 0x2d
    $sha = [Security.Cryptography.SHA1]::Create()
    try { $hash = $sha.ComputeHash([byte[]] ($namespace + [Text.Encoding]::UTF8.GetBytes($Identity))) }
    finally { $sha.Dispose() }
    $hash[6] = ($hash[6] -band 0x0f) -bor 0x50
    $hash[8] = ($hash[8] -band 0x3f) -bor 0x80
    $hex = [BitConverter]::ToString($hash, 0, 16).Replace('-', '')
    return [guid]::ParseExact($hex, 'N').ToString('B').ToUpperInvariant()
}

function Get-PayloadFiles($Directory, [string] $Prefix) {
    foreach ($item in (Get-ChildItem -LiteralPath $Directory.FullName -Force | Sort-Object -Property Name)) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "A payload path is a reparse point: $($item.FullName)"
        }
        Assert-PathPart $item.Name
        $relative = if ($Prefix) { "$Prefix/$($item.Name)" } else { $item.Name }
        if ($item.PSIsContainer) { Get-PayloadFiles $item $relative }
        else { [pscustomobject] @{ relative = $relative; path = $item.FullName; bytes = [long] $item.Length } }
    }
}

function Write-WixStart([string] $Element, [System.Collections.IDictionary] $Attributes = @{}) {
    $xml.WriteStartElement($Element, 'http://wixtoolset.org/schemas/v4/wxs')
    foreach ($attribute in $Attributes.GetEnumerator()) {
        # WiX uses $$ for a literal dollar in preprocessed source.
        $xml.WriteAttributeString([string] $attribute.Key, ([string] $attribute.Value).Replace('$', '$$'))
    }
}

function Write-WixLeaf([string] $Element, [System.Collections.IDictionary] $Attributes = @{}) {
    Write-WixStart $Element $Attributes
    $xml.WriteEndElement()
}

function Write-InstallDirectory([string] $Relative) {
    $directory = $directories[$Relative]
    Write-WixStart 'Directory' ([ordered] @{ Id = $directory.id; Name = $directory.name })
    foreach ($file in $directory.files) {
        Write-WixStart 'Component' ([ordered] @{ Id = $file.component; Guid = $file.guid })
        $attributes = [ordered] @{ Id = $file.id; Name = $file.name; Source = $file.path }
        if ($scope -eq 'perMachine') { $attributes.KeyPath = 'yes' }
        Write-WixLeaf 'File' $attributes
        if ($scope -eq 'perUser') {
            Write-WixLeaf 'RegistryValue' ([ordered] @{
                Root = 'HKCU'; Key = $registry; Name = $file.component; Type = 'integer'; Value = '1'; KeyPath = 'yes'
            })
            foreach ($remove in $file.removals) {
                Write-WixLeaf 'RemoveFolder' ([ordered] @{ Id = $remove.id; Directory = $remove.directory; On = 'uninstall' })
            }
        }
        $xml.WriteEndElement()
    }
    foreach ($child in $directory.children) { Write-InstallDirectory $child }
    $xml.WriteEndElement()
}

$wixPath = Get-AbsolutePath $Wix 'WiX tool'
$payloadPath = Get-AbsolutePath $PayloadRoot 'PayloadRoot'
$outputPath = Get-AbsolutePath $OutputDirectory 'OutputDirectory'
if (-not (Test-Path -LiteralPath $wixPath -PathType Leaf)) { throw "WiX tool not found: $wixPath" }
$toolVersion = (& $wixPath --version | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $toolVersion -notmatch '^5\.0\.2(?:\+[^\s]+)?$') {
    throw "WiX 5.0.2 is required. Found '$toolVersion'."
}

$productId = [string] $Product.id
$productName = [string] $Product.name
$version = [string] $Product.version
$scope = [string] $Product.scope
$directoryName = [string] $Product.directory
$features = @($Product.features)
if ($productId -cnotmatch '^[A-Za-z][A-Za-z0-9._-]{0,49}$' -or [string]::IsNullOrWhiteSpace($productName) -or
    $scope -cnotin 'perMachine', 'perUser' -or $features.Count -lt 1 -or $features.Count -gt 38) {
    throw 'A product requires an identifier, name, perMachine/perUser scope and 1–38 features. The first feature is core.'
}
if ($version -notmatch '^(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,4})$' -or
    [int] $Matches[1] -gt 255 -or [int] $Matches[2] -gt 255 -or [int] $Matches[3] -gt 65535) {
    throw 'MSI product version must be numeric major.minor.build in the range 255.255.65535.'
}
Assert-PathPart $directoryName
if ($TestPerUser -and -not $TestFamily) { throw 'TestPerUser requires a separate TestFamily.' }
if ($TestFamily -and $TestFamily -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,39}$') { throw 'Invalid TestFamily identifier.' }
if ($TestPerUser) { $scope = 'perUser' }
$namespaceName = "release/$($productId.ToLowerInvariant())/$scope/x64"
$fileStem = "$productId-$version-win-x64"
if ($TestFamily) {
    $namespaceName = "validation/$($TestFamily.ToLowerInvariant())/$($productId.ToLowerInvariant())/$scope/x64"
    $productName = "$productName Test $TestFamily"
    $directoryName = "$directoryName Test $TestFamily"
    $fileStem += "-test-$TestFamily"
}
$productName += ' (Development)'
$productCode = New-IdentityGuid "$namespaceName/product/$version"
$upgradeCode = New-IdentityGuid "$namespaceName/upgrade"
$familyKey = (Get-IdentityHash $namespaceName).Substring(0, 24)
$registry = "Software\ParadiseInc\StarDataParadise\Installer\$familyKey\Files"

$payload = Get-Item -LiteralPath $payloadPath
Assert-PlainDirectory $payload
$seenFeatures = @{}
$entriesByFeature = [ordered] @{}
$featureBytes = [ordered] @{}
foreach ($feature in $features) {
    $id = [string] $feature.id
    if ($id -cnotmatch '^[A-Za-z_][A-Za-z0-9_.]{0,36}$' -or $seenFeatures.ContainsKey($id) -or [string]::IsNullOrWhiteSpace([string] $feature.title)) {
        throw "Invalid or duplicate MSI feature: $id"
    }
    $seenFeatures[$id] = $true
    $directory = Get-Item -LiteralPath (Join-Path $payload.FullName $id)
    Assert-PlainDirectory $directory
    $entries = @(Get-PayloadFiles $directory '')
    if (-not $entries.Count) { throw "MSI feature $id has no staged payload." }
    $entriesByFeature[$id] = $entries
    [long] $bytes = 0
    foreach ($entry in $entries) { $bytes += $entry.bytes }
    $featureBytes[$id] = $bytes
}
foreach ($item in (Get-ChildItem -LiteralPath $payload.FullName -Force)) {
    if (-not $item.PSIsContainer -or -not $seenFeatures.ContainsKey($item.Name)) {
        throw "Unexpected item in the payload root: $($item.Name)"
    }
}

$targetPaths = @{}
$directories = @{ '' = [pscustomobject] @{ id = 'INSTALLFOLDER'; name = $directoryName; files = [Collections.Generic.List[object]]::new(); children = [Collections.Generic.List[string]]::new() } }
$componentsByFeature = [ordered] @{}
foreach ($feature in $features) {
    $id = [string] $feature.id
    $componentsByFeature[$id] = [Collections.Generic.List[string]]::new()
    $cleanup = @{}
    foreach ($entry in $entriesByFeature[$id]) {
        $target = $entry.relative
        if ($targetPaths.ContainsKey($target) -or $directories.ContainsKey($target)) {
            throw "Multiple payload entries resolve to the same Windows path: $target"
        }
        $targetPaths[$target] = $true
        $parts = $target.Split('/')
        $parent = ''
        $removals = [Collections.Generic.List[object]]::new()
        for ($index = 0; $index -lt $parts.Length; $index++) {
            if ($scope -eq 'perUser' -and -not $cleanup.ContainsKey($parent)) {
                $cleanup[$parent] = $true
                $cleanupKey = (Get-IdentityHash "$id/remove/$($parent.ToLowerInvariant())").Substring(0, 32)
                $removals.Add(@{ id = "R_$cleanupKey"; directory = $directories[$parent].id })
            }
            if ($index -eq $parts.Length - 1) { break }
            $part = $parts[$index]
            $child = if ($parent) { "$parent/$part" } else { $part }
            if ($targetPaths.ContainsKey($child)) { throw "A payload directory collides with a file: $child" }
            if (-not $directories.ContainsKey($child)) {
                $dirKey = (Get-IdentityHash $child.ToLowerInvariant()).Substring(0, 32)
                $directories[$child] = [pscustomobject] @{
                    id = "D_$dirKey"; name = $part; files = [Collections.Generic.List[object]]::new()
                    children = [Collections.Generic.List[string]]::new()
                }
                $directories[$parent].children.Add($child)
            }
            $parent = $child
        }
        $targetKey = $target.ToLowerInvariant()
        $fileKey = (Get-IdentityHash $targetKey).Substring(0, 32)
        $component = "C_$fileKey"
        $directories[$parent].files.Add([pscustomobject] @{
            component = $component; id = "F_$fileKey"; guid = New-IdentityGuid "$namespaceName/file/$targetKey"
            path = $entry.path; name = $parts[-1]; removals = $removals
        })
        $componentsByFeature[$id].Add($component)
    }
}

[void] [IO.Directory]::CreateDirectory($outputPath)
$sourceDirectory = Join-Path $outputPath '.msi-sources'
[void] [IO.Directory]::CreateDirectory($sourceDirectory)
$sourcePath = Join-Path $sourceDirectory "$fileStem.wxs"
$artifact = Join-Path $outputPath "$fileStem.msi"
$xmlSettings = [Xml.XmlWriterSettings]::new()
$xmlSettings.Indent = $true
$xmlSettings.Encoding = [Text.UTF8Encoding]::new($false)
$xml = [Xml.XmlWriter]::Create($sourcePath, $xmlSettings)
try {
    $xml.WriteStartDocument()
    Write-WixStart 'Wix'
    Write-WixStart 'Package' ([ordered] @{
        Name = $productName; Manufacturer = 'Paradise Inc'; Version = $version; Scope = $scope
        Language = '1033'; ProductCode = $productCode; UpgradeCode = $upgradeCode
    })
    Write-WixLeaf 'MajorUpgrade' ([ordered] @{
        Schedule = 'afterInstallInitialize'; DowngradeErrorMessage = 'A newer version is already installed.'
    })
    Write-WixLeaf 'MediaTemplate' ([ordered] @{ EmbedCab = 'yes' })
    $standardDirectory = if ($scope -eq 'perUser') { 'LocalAppDataFolder' } else { 'ProgramFiles64Folder' }
    Write-WixStart 'StandardDirectory' ([ordered] @{ Id = $standardDirectory })
    Write-InstallDirectory ''
    $xml.WriteEndElement()
    $level = 1
    foreach ($feature in $features) {
        Write-WixStart 'Feature' ([ordered] @{ Id = $feature.id; Title = $feature.title; Level = $level })
        foreach ($component in $componentsByFeature[[string] $feature.id]) {
            Write-WixLeaf 'ComponentRef' ([ordered] @{ Id = $component })
        }
        $xml.WriteEndElement()
        $level = 2
    }
    $xml.WriteEndElement()
    $xml.WriteEndElement()
    $xml.WriteEndDocument()
}
finally { $xml.Dispose() }

# Keep intermediary files in the explicit build output, including in restricted build hosts.
$intermediate = Join-Path $sourceDirectory "$fileStem-intermediate"
[void] [IO.Directory]::CreateDirectory($intermediate)
$wixResult = & $wixPath build $sourcePath -arch x64 -intermediatefolder $intermediate -pdbtype none -out $artifact
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
    throw "WiX could not build $fileStem (exit $LASTEXITCODE). $($wixResult -join [Environment]::NewLine)"
}
if ($wixResult) { Write-Verbose ($wixResult -join [Environment]::NewLine) }

[pscustomobject] @{
    productCode = $productCode
    upgradeCode = $upgradeCode
    path = $artifact
    featureInstalledBytes = $featureBytes
    scope = $scope
    installDirectory = $directoryName
    wixVersion = $toolVersion
}
