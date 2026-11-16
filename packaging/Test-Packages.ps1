#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ManifestPath,
    [Parameter(Mandatory)][string]$RedistPath,
    [Parameter(Mandatory)][string]$Wix,
    [string]$Scratch = (Join-Path $PSScriptRoot '..\out\sdp-m9\stage44\validation')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'PackagePlan.psm1') -Force

function Assert-True([bool]$Condition, [string]$Detail) {
    if (!$Condition) { throw $Detail }
}

function Invoke-TestProcess([string]$Program, [string[]]$Arguments, [string]$WorkingDirectory, [int]$Seconds) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Program
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.WorkingDirectory = $WorkingDirectory
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    $start.Environment['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot"
    foreach ($key in @($start.Environment.Keys)) {
        if ($key -match '^(QT_|QML|SDP_)') { $null = $start.Environment.Remove($key) }
    }
    $start.Environment['QT_QPA_PLATFORM'] = 'windows'
    $process = [Diagnostics.Process]::Start($start)
    try {
        $output = $process.StandardOutput.ReadToEndAsync()
        $errors = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit($Seconds * 1000)) {
            $process.Kill()
            $process.WaitForExit()
            throw "$([IO.Path]::GetFileName($Program)) timed out after $Seconds seconds."
        }
        [pscustomobject]@{
            ExitCode = $process.ExitCode
            Output = $output.GetAwaiter().GetResult()
            Error = $errors.GetAwaiter().GetResult()
        }
    } finally {
        $process.Dispose()
    }
}

function Assert-FileSegment([string]$Name) {
    Assert-True ($Name -notin @('', '.', '..') -and $Name -notmatch '[/\\:<>|?*"]') "Unsafe path in the extracted MSI: $Name"
}

function Expand-MsiFeatures([string]$Msi, [string]$WixTool, [object]$Info, [string[]]$Features, [string]$ScratchDirectory) {
    $cabinet = Join-Path $ScratchDirectory 'cabinet'
    $decompiled = Join-Path $ScratchDirectory 'package.wxs'
    $intermediate = Join-Path $ScratchDirectory 'intermediate'
    $arguments = @('msi', 'decompile', $Msi, '-o', $decompiled, '-x', $cabinet, '-intermediateFolder', $intermediate)
    $result = Invoke-TestProcess $WixTool $arguments $ScratchDirectory 90
    $result.Output + $result.Error | Set-Content -LiteralPath (Join-Path $ScratchDirectory 'decompile.log')
    Assert-True ($result.ExitCode -eq 0) "WiX could not decompile $Msi. See $ScratchDirectory\decompile.log."

    $document = [xml]::new()
    $document.XmlResolver = $null
    $document.Load($decompiled)
    $selected = @{}
    foreach ($feature in $Features) {
        foreach ($entry in $Info.features[$feature].files.Values) { $selected[$entry.id] = $entry }
    }
    $target = New-Item -ItemType Directory -Path (Join-Path $ScratchDirectory 'image')
    $seen = @{}
    foreach ($file in $document.SelectNodes("//*[local-name()='File']")) {
        $id = $file.GetAttribute('Id')
        if (!$selected.ContainsKey($id)) { continue }
        Assert-FileSegment $id
        $name = $file.GetAttribute('Name')
        Assert-FileSegment $name
        Assert-True ($name -eq $selected[$id].name -and !$seen.ContainsKey($id)) "Decompiled file mapping differs for $id."
        $directories = @()
        $parent = $file.ParentNode
        while ($parent -is [Xml.XmlElement] -and $parent.GetAttribute('Id') -ne 'INSTALLFOLDER') {
            if ($parent.LocalName -eq 'Directory') {
                $directory = $parent.GetAttribute('Name')
                Assert-FileSegment $directory
                $directories = @($directory) + $directories
            }
            $parent = $parent.ParentNode
        }
        Assert-True ($parent -is [Xml.XmlElement]) "Selected file $id is outside the package's application directory."
        $parentDirectory = $target.FullName
        foreach ($directory in $directories) { $parentDirectory = Join-Path $parentDirectory $directory }
        [void][IO.Directory]::CreateDirectory($parentDirectory)
        $embedded = Get-Item -LiteralPath (Join-Path $cabinet "File\$id")
        Assert-True ($embedded.Length -eq $selected[$id].bytes) "Decompiled bytes differ from the MSI table for $id."
        [IO.File]::Copy($embedded.FullName, (Join-Path $parentDirectory $name))
        $seen[$id] = $true
    }
    Assert-True ($seen.Count -eq $selected.Count) 'WiX extraction omitted files for the selected MSI features.'
    $target
}

function Get-SinglePayloadFile([System.IO.FileInfo[]]$Files, [string]$Name) {
    $found = @($Files | Where-Object Name -EQ $Name)
    Assert-True ($found.Count -eq 1) "Expected exactly one $Name in the extracted MSI."
    $found[0]
}

function Assert-X64([System.IO.FileInfo]$File) {
    $reader = [IO.BinaryReader]::new($File.OpenRead())
    try {
        Assert-True ($reader.ReadUInt16() -eq 0x5a4d) "$($File.Name) does not have a DOS/PE header."
        $reader.BaseStream.Position = 0x3c
        $reader.BaseStream.Position = $reader.ReadUInt32()
        Assert-True ($reader.ReadUInt32() -eq 0x00004550 -and $reader.ReadUInt16() -eq 0x8664) "$($File.Name) is not a Windows x64 executable."
    } finally {
        $reader.Dispose()
    }
}

function Assert-FileMatch([string]$Actual, [string]$Expected) {
    Assert-True (Test-Path -LiteralPath $Expected -PathType Leaf) "Expected reference payload is absent: $Expected"
    $actualHash = (Get-FileHash -LiteralPath $Actual).Hash
    $expectedHash = (Get-FileHash -LiteralPath $Expected).Hash
    Assert-True ($actualHash -eq $expectedHash) "Payload hash differs: $Actual"
}

function Get-Component([object]$Document, [string]$Id) {
    @($Document.components | Where-Object id -EQ $Id)[0]
}

function Assert-Rejected(
    [string]$Path, [string]$Case, [string]$Expected, [hashtable]$Selection, [bool]$AllowDevelopment = $true
) {
    $failure = $null
    try {
        $arguments = @{ ManifestPath = $Path; Platform = 'windows'; Architecture = 'x64'; AllowDevelopment = $AllowDevelopment }
        $null = Resolve-SdpPackagePlan @arguments @Selection
    } catch {
        $failure = $_
    }
    Assert-True ($null -ne $failure) "Resolver accepted negative case '$Case'."
    Assert-True ($failure.ToString() -match $Expected) "Resolver rejected '$Case' for the wrong reason: $failure"
    Write-Output "Resolver refused $Case."
}

$ManifestPath = (Resolve-Path -LiteralPath $ManifestPath).Path
$RedistPath = (Resolve-Path -LiteralPath $RedistPath).Path
$Wix = (Resolve-Path -LiteralPath $Wix).Path
$wixVersion = (& $Wix --version | Out-String).Trim()
Assert-True ($LASTEXITCODE -eq 0 -and $wixVersion -match '^5\.0\.2(?:\+[^\s]+)?$') 'MSI acceptance requires WiX 5.0.2.'
$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json -Depth 32
$run = New-Item -ItemType Directory -Path (Join-Path $Scratch ([Guid]::NewGuid().ToString('N'))) -Force
$required = [ordered]@{
    server = @{ Core = 'server.core'; Exe = 'StarDataParadiseServer.exe' }
    workbench = @{ Core = 'workbench.core'; Exe = 'StarDataParadise.exe' }
    cli = @{ Core = 'cli.core'; Exe = 'StarDataParadiseCLI.exe' }
}
$native = @{}
$seenProducts = @()
$seenArtifacts = @()
    foreach ($name in $required.Keys) {
        $profile = "$name-only"
        $spec = $required[$name]
        $core = Get-Component $manifest $spec.Core
        $plan = Resolve-SdpPackagePlan -ManifestPath $ManifestPath -Profile $profile -Platform windows -Architecture x64 -AllowDevelopment
        Assert-True ($plan.products.Count -eq 1) "$profile must resolve to exactly one independently installable MSI."
        $product = $plan.products[0]
        $msi = (Resolve-Path -LiteralPath $product.artifactPath).Path
        Assert-True ($msi.EndsWith('.msi', [StringComparison]::OrdinalIgnoreCase)) "$profile artifact is not an MSI."
        Assert-True ($msi -notin $seenArtifacts -and $product.productCode -notin $seenProducts) "$profile shares another product's MSI or product code."
        $seenArtifacts += $msi
        $seenProducts += $product.productCode
        Assert-True ($product.features -contains $core.feature) "$profile did not resolve its Core feature."
        Assert-True ($plan.downloadBytes -eq (Get-Item -LiteralPath $msi).Length -and $plan.installedBytes -gt 0) "$profile has incorrect download or installed sizes."

        $db = Get-SdpMsiPackageInfo -Path $msi
        Assert-True ($db.architecture -eq 'x64') "$profile is not an actual x64 MSI."
        Assert-True ($db.productCode -eq $product.productCode) "$profile manifest product code does not match its MSI."
        Assert-True ($db.features.ContainsKey($core.feature)) "$profile Core feature is missing from the actual MSI."
        foreach ($feature in $product.features) {
            Assert-True ($db.features.ContainsKey($feature)) "Resolved feature $feature is absent from the actual MSI."
        }
        if ($manifest.channel -eq 'development') {
            Assert-True ((Get-AuthenticodeSignature -LiteralPath $msi).Status -eq 'NotSigned') "$profile development MSI has an unexpected signature state."
        }

        $coreEntries = $db.features[$core.feature].files.Values
        $coreFiles = @($coreEntries | Select-Object -ExpandProperty name)
        Assert-True ($coreFiles -contains $spec.Exe) "$profile Core feature does not own its required executable."
        if ($name -eq 'server') {
            Assert-True (!($coreFiles -match '^(Qt[0-9]|qwindows\.dll$|cuda|nvcuda|cudnn|nvrtc)')) 'Server Core includes a Qt or GPU runtime.'
        }

        $slot = New-Item -ItemType Directory -Path (Join-Path $run.FullName $name)
        $target = Expand-MsiFeatures $msi $Wix $db $product.features $slot.FullName
        $files = @(Get-ChildItem -LiteralPath $target.FullName -Recurse -File)
        Assert-True (($files | Measure-Object -Property Length -Sum).Sum -eq $plan.installedBytes) "$profile Core extracted size differs from the resolved install size."
        $exe = Get-SinglePayloadFile $files $spec.Exe
        Assert-X64 $exe
        foreach ($crtName in (Get-ChildItem -LiteralPath $RedistPath -Filter '*.dll' -File).Name) {
            Assert-FileMatch (Join-Path $exe.DirectoryName $crtName) (Join-Path $RedistPath $crtName)
        }
        Write-Output "S44-T01 $profile extracted its own x64 executable."

        if ($name -eq 'server') {
            $launch = Invoke-TestProcess $exe.FullName @('--version') $exe.DirectoryName 20
            Assert-True ($launch.ExitCode -eq 0 -and $launch.Output -match '^StarDataParadiseServer \d+\.\d+\.\d+') 'Extracted Server did not start and print its version.'
        } else {
            Assert-True (!($files.Name -match '^(StarDataParadiseServer\.exe|hostfxr\.dll|python3.*\.dll)$|\.(runtimeconfig\.json|jar)$')) "$profile includes a local Server or managed language runtime."
            $dllName = 'StarDataParadiseDLLler.dll'
            $dll = Get-SinglePayloadFile $files $dllName
            Assert-True ($dll.DirectoryName -eq $exe.DirectoryName) "$profile Native runtime is not app-local."
            $owned = @($core.ownership | Where-Object { $_.mode -eq 'app-local' -and $_.path -match 'StarDataParadiseDLLler\.dll$' })
            $dllHash = (Get-FileHash -LiteralPath $dll.FullName).Hash
            Assert-True ($owned.Count -eq 1 -and $owned[0].sha256 -eq $dllHash) "$profile Native ownership digest does not match its extracted DLL."
            $dllRecord = @($coreEntries | Where-Object Name -EQ $dllName)
            Assert-True ($dllRecord.Count -eq 1) "$profile has ambiguous Native DLL ownership."
            $dllOwner = $db.components[$dllRecord[0].component]
            Assert-True ($null -ne $dllOwner -and $dllOwner.id -match '^\{[0-9a-f-]{36}\}$') "$profile is missing its Native Windows Installer component ID."
            $native[$name] = @{ File = $dll.FullName; Owner = $dllOwner.id; Scope = $owned[0].scope }

            if ($name -eq 'workbench') {
                $migration = Get-Component $manifest 'workbench.migration'
                $helper = @($db.features[$migration.feature].files.Values |
                    Where-Object Name -EQ 'StarDataParadiseMigrationSource.exe')
                Assert-True ($helper.Count -eq 1 -and
                    (Test-Path -LiteralPath (Join-Path $slot.FullName "cabinet\File\$($helper[0].id)") -PathType Leaf)) 'Workbench is missing the optional Migration source helper.'
                $plugin = Join-Path $exe.DirectoryName 'platforms\qwindows.dll'
                Assert-True (Test-Path -LiteralPath $plugin -PathType Leaf) 'Workbench is missing its own Windows Qt platform plugin.'
                $launch = Invoke-TestProcess $exe.FullName @('--smoke-test') $exe.DirectoryName 25
            } else {
                $launch = Invoke-TestProcess $exe.FullName @('--version') $exe.DirectoryName 20
                Assert-True ($launch.Output -match '^StarDataParadiseCLI \d+\.\d+\.\d+') 'CLI did not print its own version.'
            }
            Assert-True ($launch.ExitCode -eq 0) "$profile did not launch using its extracted runtime: $($launch.Error)"
        }
    }

Assert-FileMatch $native.workbench.File $native.cli.File
Assert-True ($native.workbench.Owner -ne $native.cli.Owner -and $native.workbench.Scope -ne $native.cli.Scope) 'Workbench and CLI must have distinct ownership for identically versioned app-local Native DLLs.'
Write-Output 'S44-T02 Server Core has no Qt/GPU runtime, and the two separately extracted clients start without a local Server.'
Write-Output 'S44-T03 Workbench and CLI package the same Native bytes in independently owned components.'

$negative = New-Item -ItemType Directory -Path (Join-Path $run.FullName 'negative')
$sourceRoot = [IO.Path]::TrimEndingDirectorySeparator((Split-Path -Parent $ManifestPath)) + [IO.Path]::DirectorySeparatorChar
$negativeRoot = $negative.FullName + [IO.Path]::DirectorySeparatorChar
foreach ($relative in @($manifest.components.artifact | Sort-Object -Unique)) {
    $source = (Resolve-Path -LiteralPath (Join-Path $sourceRoot $relative)).Path
    $target = [IO.Path]::GetFullPath((Join-Path $negativeRoot $relative))
    Assert-True ($source.StartsWith($sourceRoot, [StringComparison]::OrdinalIgnoreCase) -and
        $target.StartsWith($negativeRoot, [StringComparison]::OrdinalIgnoreCase)) 'Test package copy must stay inside its input and scratch directories.'
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $target))
    [IO.File]::Copy($source, $target)
}

$original = Get-Content -LiteralPath $ManifestPath -Raw
$client = @{ Profile = 'cli-only' }
$bothClients = @{ ComponentId = @('workbench.core', 'cli.core') }
$negativeCases = @(
    @{
        Name = 'conflict'; Expected = 'Package conflict between'; Select = $bothClients
        Change = {
            param($item)
            (Get-Component $item 'cli.core').conflicts = @('workbench.core')
            (Get-Component $item 'workbench.core').conflicts = @('cli.core')
        }
    },
    @{
        Name = 'incompatible-architecture'; Expected = 'Incompatible platform or architecture'; Select = $client
        Change = { param($item) (Get-Component $item 'cli.core').architecture = 'x86' }
    },
    @{
        Name = 'artifact-traversal'; Expected = 'Unsafe manifest artifact'; Select = $client
        Change = { param($item) (Get-Component $item 'cli.core').artifact = '../unowned.msi' }
    },
    @{
        Name = 'duplicate-component'; Expected = 'Duplicate package component ID'; Select = $client
        Change = { param($item) $item.components = @($item.components) + (Get-Component $item 'cli.core') }
    },
    @{
        Name = 'missing-dependency'; Expected = 'Unknown dependency'; Select = $client
        Change = {
            param($item)
            (Get-Component $item 'cli.core').dependencies = @([pscustomobject]@{ id = 'absent.core'; version = '0.0.1' })
        }
    },
    @{
        Name = 'dependency-cycle'; Expected = 'Dependency cycle'; Select = $client
        Change = {
            param($item)
            $first = Get-Component $item 'cli.core'
            $second = Get-Component $item 'workbench.core'
            $first.dependencies = @([pscustomobject]@{ id = $second.id; version = $second.version })
            $second.dependencies = @([pscustomobject]@{ id = $first.id; version = $first.version })
        }
    },
    @{
        Name = 'overlapping-dll-ownership'; Expected = 'Conflicting application-local DLL ownership'; Select = $bothClients
        Change = {
            param($item)
            $cli = Get-Component $item 'cli.core'
            $wb = Get-Component $item 'workbench.core'
            $cli.ownership[0].scope = $wb.ownership[0].scope
            $cli.ownership[0].path = $wb.ownership[0].path
        }
    }
)
foreach ($case in $negativeCases) {
    $document = $original | ConvertFrom-Json -Depth 32
    & $case.Change $document
    $path = Join-Path $negativeRoot ($case.Name + '.json')
    $document | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $path -Encoding utf8
    Assert-Rejected $path $case.Name $case.Expected $case.Select
}

$path = Join-Path $negativeRoot 'selected.json'
$original | Set-Content -LiteralPath $path -Encoding utf8
Assert-Rejected $path 'insufficient-disk' 'Insufficient disk space' @{ Profile = 'cli-only'; AvailableDiskBytes = 1 }
if ($manifest.channel -eq 'development') {
    Assert-Rejected $path 'unacknowledged-development' 'Explicitly allow development packages' $client $false
}

# Alter one byte without changing the length of the disposable MSI.
$damaged = Join-Path $negativeRoot (Get-Component $manifest 'cli.core').artifact
$stream = [IO.File]::Open($damaged, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite)
try {
    $stream.Position = $stream.Length - 1
    $value = $stream.ReadByte()
    $stream.Position--
    $stream.WriteByte([byte]($value -bxor 1))
} finally {
    $stream.Dispose()
}
Assert-Rejected $path 'tampered-artifact' 'Package SHA-256 or download size mismatch' $client
Write-Output 'S44-T03 Resolver negative tests passed.'
Write-Output "Evidence directory $($run.FullName)"
