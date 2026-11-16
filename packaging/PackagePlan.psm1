Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Release-SdpComObject {
    param($Object)
    if ($null -ne $Object -and [Runtime.InteropServices.Marshal]::IsComObject($Object)) {
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($Object)
    }
}

function Invoke-SdpComMember {
    param($Object, [string]$Name, [string]$Kind, [object[]]$Arguments)
    return $Object.GetType().InvokeMember($Name, $Kind, $null, $Object, $Arguments)
}

function Read-SdpMsiTable {
    param($Database, [string]$Query, [string[]]$Columns, [int[]]$Integers = @())
    $view = $null
    try {
        $view = Invoke-SdpComMember $Database 'OpenView' 'InvokeMethod' @($Query)
        [void](Invoke-SdpComMember $view 'Execute' 'InvokeMethod' @())
        $count = 0
        while ($true) {
            $record = Invoke-SdpComMember $view 'Fetch' 'InvokeMethod' @()
            if ($null -eq $record) { break }
            try {
                if (++$count -gt 50000) { throw 'MSI table exceeds the supported size.' }
                $row = [ordered]@{}
                for ($index = 0; $index -lt $Columns.Count; $index++) {
                    $position = $index + 1
                    $member = if ($Integers -contains $position) { 'IntegerData' } else { 'StringData' }
                    $row[$Columns[$index]] = Invoke-SdpComMember $record $member 'GetProperty' @($position)
                }
                [pscustomobject]$row
            }
            finally { Release-SdpComObject $record }
        }
    }
    finally {
        if ($null -ne $view) {
            try { [void](Invoke-SdpComMember $view 'Close' 'InvokeMethod' @()) }
            finally { Release-SdpComObject $view }
        }
    }
}

function Get-SdpMsiPackageInfo {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Path)
    $file = Get-Item -LiteralPath $Path
    if ($file.PSIsContainer -or $file.Extension -ine '.msi') { throw 'Expected an MSI package.' }
    $installer = $database = $summary = $null
    try {
        $installer = New-Object -ComObject WindowsInstaller.Installer
        $database = Invoke-SdpComMember $installer 'OpenDatabase' 'InvokeMethod' @($file.FullName, 0)
        $summary = Invoke-SdpComMember $installer 'SummaryInformation' 'GetProperty' @($file.FullName, 0)
        $template = [string](Invoke-SdpComMember $summary 'Property' 'GetProperty' @(7))
        $architecture = ($template -split ';', 2)[0]
        if ($architecture -ceq 'Intel' -or $architecture -eq '') { $architecture = 'x86' }
        $properties = @{}
        $query = 'SELECT `Property`, `Value` FROM `Property`'
        foreach ($row in (Read-SdpMsiTable $database $query @('name', 'value'))) {
            $properties[$row.name] = $row.value
        }
        if (!$properties.ContainsKey('ProductCode') -or !$properties.ContainsKey('ProductVersion')) {
            throw 'MSI does not define a product code and version.'
        }
        $features = @{}
        $query = 'SELECT `Feature`, `Feature_Parent` FROM `Feature`'
        foreach ($row in (Read-SdpMsiTable $database $query @('id', 'parent'))) {
            $features[$row.id] = [pscustomobject]@{ id = $row.id; parent = $row.parent; installedBytes = [long]0; files = @{} }
        }
        $components = @{}
        $query = 'SELECT `Component`, `ComponentId`, `Directory_` FROM `Component`'
        foreach ($row in (Read-SdpMsiTable $database $query @('name', 'id', 'directory'))) {
            $components[$row.name] = $row
        }
        $filesByComponent = @{}
        $query = 'SELECT `File`, `Component_`, `FileName`, `FileSize` FROM `File`'
        foreach ($row in (Read-SdpMsiTable $database $query @('id', 'component', 'name', 'bytes') @(4))) {
            if ($row.bytes -lt 0) { throw 'MSI file table contains an invalid size.' }
            if (!$filesByComponent.ContainsKey($row.component)) {
                $filesByComponent[$row.component] = New-Object 'System.Collections.Generic.List[object]'
            }
            $row.name = ($row.name -split '\|', 2)[-1]
            $filesByComponent[$row.component].Add($row)
        }
        $query = 'SELECT `Feature_`, `Component_` FROM `FeatureComponents`'
        foreach ($row in (Read-SdpMsiTable $database $query @('feature', 'component'))) {
            if (!$features.ContainsKey($row.feature)) { throw 'MSI map refers to an absent feature.' }
            if (!$filesByComponent.ContainsKey($row.component)) { continue }
            $feature = $features[$row.feature]
            foreach ($payload in $filesByComponent[$row.component]) {
                if ($feature.files.ContainsKey($payload.id)) { continue }
                $feature.files[$payload.id] = $payload
                $feature.installedBytes += $payload.bytes
            }
        }
        [pscustomobject]@{
            architecture = $architecture
            productCode = $properties.ProductCode
            upgradeCode = $properties['UpgradeCode']
            version = $properties.ProductVersion
            scope = if ($properties['ALLUSERS'] -eq '1') { 'per-machine' } else { 'per-user' }
            features = $features
            components = $components
        }
    }
    finally {
        Release-SdpComObject $summary
        Release-SdpComObject $database
        Release-SdpComObject $installer
    }
}

function Assert-SdpFields {
    param($Value, [string[]]$Required, [string[]]$Optional = @())
    if ($Value -isnot [pscustomobject]) { throw 'Expected a manifest object.' }
    $names = @($Value.PSObject.Properties.Name)
    foreach ($key in $Required) {
        if ($names -cnotcontains $key) { throw "Missing manifest field '$key'." }
    }
    foreach ($key in $names) {
        if ($Required -cnotcontains $key -and $Optional -cnotcontains $key) {
            throw "Unexpected manifest field '$key'."
        }
    }
}

function Assert-SdpText {
    param($Value, [string]$Pattern, [string]$Label)
    if ($Value -isnot [string] -or $Value -cnotmatch $Pattern) {
        throw "Invalid manifest $Label."
    }
}

function Assert-SdpArray {
    param($Value, [int]$Max, [string]$Label)
    if ($Value -isnot [array] -or $Value.Count -gt $Max) { throw "Invalid manifest $Label." }
}

function Assert-SdpSize {
    param($Value, [string]$Label)
    if (($Value -isnot [long] -and $Value -isnot [int]) -or $Value -lt 0 -or $Value -gt 16TB) {
        throw "Invalid manifest $Label."
    }
}

function Assert-SdpRelativePath {
    param($Value, [string]$Label)
    Assert-SdpText $Value '\A[A-Za-z0-9_. -]+(?:/[A-Za-z0-9_. -]+)*\z' $Label
    if ($Value.Length -gt 240) { throw "Invalid manifest $Label." }
    foreach ($part in $Value.Split('/')) {
        if ($part -in '.', '..' -or $part.StartsWith(' ') -or $part.EndsWith(' ') -or $part.EndsWith('.') -or
            $part -match '\A(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|\z)') {
            throw "Unsafe manifest $Label."
        }
    }
}

function Get-SdpArtifactPath {
    param([string]$Root, [string]$Relative)
    $cursor = $Root
    foreach ($part in $Relative.Split('/')) {
        $item = Get-Item -LiteralPath (Join-Path $cursor $part)
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw 'A package artifact cannot use a symbolic link or junction.'
        }
        $cursor = $item.FullName
    }
    if ($item.PSIsContainer -or !$cursor.StartsWith($Root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'A package artifact must be a file within the bundle.'
    }
    return $item
}

function Read-SdpZipPayload {
    param([string]$Path, [object[]]$Payload)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if ($Payload.Count -eq 0) { throw 'ZIP packages need a payload inventory.' }
    $expected = @{}
    foreach ($entry in $Payload) {
        Assert-SdpFields $entry @('path', 'bytes', 'sha256')
        Assert-SdpRelativePath $entry.path 'payload path'
        Assert-SdpText $entry.sha256 '(?i)\A[0-9a-f]{64}\z' 'payload SHA-256'
        Assert-SdpSize $entry.bytes 'payload size'
        if ($expected.ContainsKey($entry.path)) { throw 'ZIP payload inventory repeats a path.' }
        $expected[$entry.path] = $entry
    }
    $zip = [IO.Compression.ZipFile]::OpenRead($Path)
    $hasher = [Security.Cryptography.SHA256]::Create()
    try {
        $seen = @{}
        $size = [long]0
        foreach ($entry in $zip.Entries) {
            $name = $entry.FullName
            Assert-SdpRelativePath $name.TrimEnd('/') 'ZIP entry path'
            $entryType = ($entry.ExternalAttributes -shr 16) -band 0xf000
            if ($entryType -eq 0xa000 -or ($entry.ExternalAttributes -band 0x400)) {
                throw 'ZIP payload must not contain links.'
            }
            if ($name.EndsWith('/')) { continue }
            if (!$expected.ContainsKey($name) -or $seen.ContainsKey($name) -or
                $expected[$name].path -cne $name -or $expected[$name].bytes -ne $entry.Length) {
                throw 'ZIP payload does not match the manifest inventory.'
            }
            $stream = $entry.Open()
            try { $hash = [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-', '') }
            finally { $stream.Dispose() }
            if ($hash -ine $expected[$name].sha256) { throw "ZIP payload SHA-256 mismatch for '$name'." }
            $seen[$name] = $true
            $size += $entry.Length
        }
        if ($seen.Count -ne $expected.Count) { throw 'ZIP payload inventory contains missing files.' }
        return $size
    }
    finally {
        $hasher.Dispose()
        $zip.Dispose()
    }
}

function Assert-SdpComponent {
    param($Component, [string]$Channel)
    $required = @('id', 'version', 'platform', 'architecture', 'kind', 'artifact', 'sha256',
        'downloadBytes', 'installedBytes', 'signature', 'restart', 'dataOwner', 'dependencies', 'conflicts', 'scope')
    Assert-SdpFields $Component $required @('productCode', 'feature', 'ownership', 'payload')
    Assert-SdpText $Component.id '\A[a-z][a-z0-9.-]{0,63}\z' 'component ID'
    Assert-SdpText $Component.version '\A[0-9]+\.[0-9]+\.[0-9]+\z' 'version'
    Assert-SdpText $Component.platform '\A(?:windows|linux)\z' 'platform'
    Assert-SdpText $Component.architecture '\A(?:x64|x86|arm64)\z' 'architecture'
    Assert-SdpText $Component.kind '\A(?:msi|zip|tar)\z' 'artifact kind'
    Assert-SdpRelativePath $Component.artifact 'artifact path'
    Assert-SdpText $Component.sha256 '(?i)\A[0-9a-f]{64}\z' 'artifact SHA-256'
    Assert-SdpSize $Component.downloadBytes 'download size'
    Assert-SdpSize $Component.installedBytes 'installed size'
    Assert-SdpText $Component.scope '\A(?:per-user|per-machine)\z' 'installation scope'
    Assert-SdpText $Component.restart '\A(?:none|possible|required)\z' 'restart policy'
    Assert-SdpText $Component.dataOwner '\A(?:none|instance|user)\z' 'data owner'
    Assert-SdpFields $Component.signature @('policy', 'thumbprint')
    if ($Component.signature.policy -ceq 'authenticode') {
        Assert-SdpText $Component.signature.thumbprint '(?i)\A[0-9a-f]{40}\z' 'signer thumbprint'
    }
    elseif ($Channel -cne 'development' -or $Component.signature.policy -cne 'unsigned-development' -or
        $null -ne $Component.signature.thumbprint) {
        throw 'Invalid package signature policy.'
    }
    if ($Component.kind -ceq 'msi') {
        if ($null -eq $Component.PSObject.Properties['productCode'] -or $null -eq $Component.PSObject.Properties['feature'] -or
            $null -ne $Component.PSObject.Properties['payload'] -or $Component.platform -cne 'windows') {
            throw 'MSI packages require a Windows product code and feature.'
        }
        Assert-SdpText $Component.productCode '(?i)\A\{[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}\}\z' 'MSI product code'
        Assert-SdpText $Component.feature '\A[A-Za-z_][A-Za-z0-9_.]{0,37}\z' 'MSI feature'
    }
    elseif ($null -ne $Component.PSObject.Properties['productCode'] -or $null -ne $Component.PSObject.Properties['feature'] -or
        $null -eq $Component.PSObject.Properties['payload']) {
        throw 'Archive packages require a payload inventory.'
    }
    Assert-SdpArray $Component.dependencies 256 'dependencies'
    Assert-SdpArray $Component.conflicts 256 'conflicts'
    $seen = @{}
    foreach ($dependency in $Component.dependencies) {
        Assert-SdpFields $dependency @('id', 'version')
        Assert-SdpText $dependency.id '\A[a-z][a-z0-9.-]{0,63}\z' 'dependency ID'
        Assert-SdpText $dependency.version '\A[0-9]+\.[0-9]+\.[0-9]+\z' 'dependency version'
        if ($seen.ContainsKey($dependency.id)) { throw "Duplicate dependency '$($dependency.id)'." }
        $seen[$dependency.id] = $true
    }
    foreach ($id in $Component.conflicts) { Assert-SdpText $id '\A[a-z][a-z0-9.-]{0,63}\z' 'conflict ID' }
    if ($null -ne $Component.PSObject.Properties['ownership']) {
        Assert-SdpArray $Component.ownership 32 'file ownership'
        foreach ($entry in $Component.ownership) {
            Assert-SdpFields $entry @('mode', 'scope', 'path', 'version', 'sha256')
            Assert-SdpText $entry.mode '\Aapp-local\z' 'ownership mode'
            Assert-SdpText $entry.scope '\A(?:workbench|cli)\z' 'ownership scope'
            Assert-SdpText $entry.path '\AStarDataParadiseDLLler\.dll\z' 'app-local SDK filename'
            Assert-SdpText $entry.version '\A[0-9]+\.[0-9]+\.[0-9]+\z' 'owned SDK version'
            Assert-SdpText $entry.sha256 '(?i)\A[0-9a-f]{64}\z' 'owned SDK SHA-256'
        }
    }
}

function Add-SdpDependency {
    param([string]$Id, [hashtable]$Components, [hashtable]$State, $Ordered, [string]$Platform, [string]$Architecture)
    if (!$Components.ContainsKey($Id)) { throw "Unknown package component or dependency '$Id'." }
    if ($State[$Id] -eq 'pending') { throw "Dependency cycle at '$Id'." }
    if ($State[$Id] -eq 'complete') { return }
    $item = $Components[$Id]
    if ($item.platform -cne $Platform -or $item.architecture -cne $Architecture) {
        throw "Incompatible platform or architecture for component '$Id'."
    }
    $State[$Id] = 'pending'
    foreach ($dependency in $item.dependencies) {
        if (!$Components.ContainsKey($dependency.id)) { throw "Unknown dependency '$($dependency.id)'." }
        if ($Components[$dependency.id].version -cne $dependency.version) {
            throw "Dependency version mismatch for '$($dependency.id)'."
        }
        Add-SdpDependency $dependency.id $Components $State $Ordered $Platform $Architecture
    }
    $State[$Id] = 'complete'
    $Ordered.Add($item)
}

function Get-SdpVerifiedArtifact {
    param($Component, [string]$Root)
    $file = Get-SdpArtifactPath $Root $Component.artifact
    if ($file.Extension -ine ".$($Component.kind)") {
        throw "Package extension does not match the declared kind for '$($Component.id)'."
    }
    if ($file.Length -ne $Component.downloadBytes -or
        (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash -ine $Component.sha256) {
        throw "Package SHA-256 or download size mismatch for '$($Component.artifact)'."
    }
    if ($Component.signature.policy -ceq 'authenticode') {
        $signature = Get-AuthenticodeSignature -LiteralPath $file.FullName
        if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ine $Component.signature.thumbprint) {
            throw "Package signature or signer mismatch for '$($Component.artifact)'."
        }
    }
    return $file
}

function Add-SdpMsiFeature {
    param($Component, $Verified, [hashtable]$RestartRank)
    $msi = $Verified.msi
    if ($msi.architecture -ine $Component.architecture -or $msi.productCode -ine $Component.productCode -or
        $msi.version -cne $Component.version -or $msi.scope -cne $Component.scope) {
        throw "MSI architecture, product, version or installation scope mismatch for '$($Component.artifact)'."
    }
    $feature = $msi.features[$Component.feature]
    if ($null -eq $feature -or $feature.id -cne $Component.feature -or $feature.installedBytes -ne $Component.installedBytes) {
        throw "MSI feature or installed size mismatch for '$($Component.id)'."
    }
    $product = $Verified.product
    if ($product.features -contains $Component.feature) { throw "Duplicate MSI feature '$($Component.feature)'." }
    if ($feature.parent -and $product.features -cnotcontains $feature.parent) {
        throw "MSI parent feature '$($feature.parent)' is missing from declared dependencies."
    }
    $product.features += $feature.id
    foreach ($payload in $feature.files.Values) {
        if (!$Verified.files.ContainsKey($payload.id)) {
            $Verified.files[$payload.id] = $true
            $product.installedBytes += $payload.bytes
        }
    }
    if ($RestartRank[$Component.restart] -gt $RestartRank[$product.restart]) { $product.restart = $Component.restart }
    if ($Component.dataOwner -cne 'none') {
        if ($product.dataOwner -cne 'none' -and $product.dataOwner -cne $Component.dataOwner) {
            throw "Conflicting data ownership in MSI '$($Component.artifact)'."
        }
        $product.dataOwner = $Component.dataOwner
    }
    $Verified.artifact.installedBytes = $product.installedBytes
    return $feature
}

function Add-SdpOwnership {
    param($Component, $Feature, [hashtable]$InstalledOwners, $Ownership)
    if ($null -eq $Component.PSObject.Properties['ownership']) { return }
    foreach ($entry in $Component.ownership) {
        $target = "$($entry.scope)/$($entry.path)"
        if ($Component.id.Split('.')[0] -cne $entry.scope -or $InstalledOwners.ContainsKey($target)) {
            throw "Conflicting application-local DLL ownership for '$target'."
        }
        if ($null -ne $Feature) {
            $copies = @($Feature.files.Values | Where-Object { $_.name -ceq $entry.path })
            if ($copies.Count -ne 1) { throw "MSI feature does not own exactly one DLL for '$target'." }
        }
        else {
            $copies = @($Component.payload | Where-Object { $_.path -ceq $entry.path -and $_.sha256 -ieq $entry.sha256 })
            if ($copies.Count -ne 1) { throw "Archive does not own the specified DLL for '$target'." }
        }
        if ($Ownership.Count -and ($Ownership[0].version -cne $entry.version -or $Ownership[0].sha256 -ine $entry.sha256)) {
            throw 'Application-local DLL copies disagree on the Native SDK version or SHA-256.'
        }
        $InstalledOwners[$target] = $true
        $Ownership.Add($entry)
    }
}

function Resolve-SdpPackagePlan {
    [CmdletBinding(DefaultParameterSetName = 'Profile')]
    param(
        [Parameter(Mandatory = $true)][string]$ManifestPath,
        [Parameter(Mandatory = $true, ParameterSetName = 'Profile')][string]$Profile,
        [Parameter(Mandatory = $true, ParameterSetName = 'Components')][string[]]$ComponentId,
        [ValidateSet('windows')][string]$Platform = 'windows',
        [ValidateSet('x64')][string]$Architecture = 'x64',
        [ValidateRange(0, [long]::MaxValue)][long]$AvailableDiskBytes = [long]::MaxValue,
        [switch]$AllowDevelopment
    )

    $manifest = Get-Item -LiteralPath $ManifestPath
    if ($manifest.PSIsContainer -or $manifest.Length -gt 2MB -or
        ($manifest.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'Expected an offline package manifest file of at most 2 MiB.'
    }
    $document = Get-Content -LiteralPath $manifest.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert-SdpFields $document @('schema', 'channel', 'profiles', 'components') @('unavailable')
    if (($document.schema -isnot [int] -and $document.schema -isnot [long]) -or $document.schema -ne 1) {
        throw 'Unsupported package manifest schema.'
    }
    Assert-SdpText $document.channel '\A(?:development|release)\z' 'channel'
    if ($document.channel -ceq 'development' -and !$AllowDevelopment) {
        throw 'This bundle contains development packages. Explicitly allow development packages to proceed.'
    }
    Assert-SdpArray $document.components 256 'components'
    $components = @{}
    foreach ($item in $document.components) {
        Assert-SdpComponent $item $document.channel
        if ($components.ContainsKey($item.id)) { throw "Duplicate package component ID '$($item.id)'." }
        $components[$item.id] = $item
    }
    if ($document.profiles -isnot [pscustomobject]) { throw 'Expected a manifest profiles object.' }
    if ($PSCmdlet.ParameterSetName -eq 'Profile') {
        Assert-SdpText $Profile '\A[a-z][a-z0-9.-]{0,63}\z' 'profile'
        $entry = $document.profiles.PSObject.Properties[$Profile]
        if ($null -eq $entry -or $entry.Name -cne $Profile) { throw "Unknown setup profile '$Profile'." }
        Assert-SdpArray $entry.Value 256 'profile components'
        $ComponentId = $entry.Value
    }
    if (!$ComponentId.Count -or $ComponentId.Count -gt 256) { throw 'Select at least one package component.' }

    $ordered = New-Object 'System.Collections.Generic.List[object]'
    $state = @{}
    foreach ($id in $ComponentId) {
        Assert-SdpText $id '\A[a-z][a-z0-9.-]{0,63}\z' 'selected component ID'
        Add-SdpDependency $id $components $state $ordered $Platform $Architecture
    }
    foreach ($item in $ordered) {
        foreach ($conflict in $item.conflicts) {
            if (!$components.ContainsKey($conflict) -or $item.id -ceq $conflict) {
                throw "Invalid package conflict '$conflict' for '$($item.id)'."
            }
            if ($state[$conflict] -eq 'complete') { throw "Package conflict between '$($item.id)' and '$conflict'." }
        }
    }

    $artifactIndex = @{}
    $productIndex = @{}
    $installedOwners = @{}
    $ownership = New-Object 'System.Collections.Generic.List[object]'
    $artifacts = New-Object 'System.Collections.Generic.List[object]'
    $products = New-Object 'System.Collections.Generic.List[object]'
    $restartRank = @{ none = 0; possible = 1; required = 2 }
    $restart = 'none'
    $requiresElevation = $false
    foreach ($item in $ordered) {
        if ($restartRank[$item.restart] -gt $restartRank[$restart]) { $restart = $item.restart }
        if ($item.scope -ceq 'per-machine') { $requiresElevation = $true }
        $path = $item.artifact
        $verified = $artifactIndex[$path]
        if ($null -eq $verified) {
            $file = Get-SdpVerifiedArtifact $item $manifest.Directory.FullName
            $artifact = [pscustomobject]@{ artifact = $path; artifactPath = $file.FullName; kind = $item.kind;
                sha256 = $item.sha256; downloadBytes = $file.Length; installedBytes = [long]0 }
            $verified = [pscustomobject]@{ artifact = $artifact; component = $item; msi = $null; product = $null; files = @{} }
            $artifactIndex[$path] = $verified
            $artifacts.Add($artifact)
            if ($item.kind -ceq 'msi') {
                $verified.msi = Get-SdpMsiPackageInfo -Path $file.FullName
                if ($productIndex.ContainsKey($item.productCode)) {
                    throw 'An MSI product code identifies multiple bundle artifacts.'
                }
                $product = [pscustomobject]@{ artifact = $path; artifactPath = $file.FullName;
                    productCode = $item.productCode; features = @(); scope = $item.scope;
                    downloadBytes = $file.Length; installedBytes = [long]0; requiresElevation = $item.scope -ceq 'per-machine';
                    restart = $item.restart; dataOwner = $item.dataOwner }
                $productIndex[$item.productCode] = $product
                $verified.product = $product
                $products.Add($product)
            }
            elseif ($item.kind -ceq 'zip') {
                Assert-SdpArray $item.payload 50000 'ZIP payload'
                $artifact.installedBytes = Read-SdpZipPayload $file.FullName $item.payload
                if ($item.installedBytes -ne $artifact.installedBytes) { throw 'ZIP installed size mismatch.' }
            }
            else { throw 'The Windows package resolver cannot select TAR payloads.' }
        }
        else {
            $prior = $verified.component
            if ($path -cne $prior.artifact -or $item.kind -cne $prior.kind -or $item.sha256 -ine $prior.sha256 -or
                $item.downloadBytes -ne $prior.downloadBytes -or $item.signature.policy -cne $prior.signature.policy -or
                $item.signature.thumbprint -ine $prior.signature.thumbprint -or $item.kind -cne 'msi' -or
                $item.productCode -ine $prior.productCode) {
                throw "Conflicting declarations for shared package artifact '$path'."
            }
        }

        $feature = $null
        if ($item.kind -ceq 'msi') { $feature = Add-SdpMsiFeature $item $verified $restartRank }
        Add-SdpOwnership $item $feature $installedOwners $ownership
    }

    $download = [long]0
    $installed = [long]0
    foreach ($artifact in $artifacts) {
        $download += $artifact.downloadBytes
        $installed += $artifact.installedBytes
    }
    if ($installed -gt $AvailableDiskBytes) { throw 'Insufficient disk space for the verified package payloads.' }
    [pscustomobject]@{
        platform = $Platform; architecture = $Architecture; channel = $document.channel
        components = $ordered.ToArray(); products = $products.ToArray(); artifacts = $artifacts.ToArray()
        downloadBytes = $download; installedBytes = $installed; requiresElevation = $requiresElevation
        restart = $restart; ownership = $ownership.ToArray()
    }
}

Export-ModuleMember -Function Get-SdpMsiPackageInfo, Resolve-SdpPackagePlan
