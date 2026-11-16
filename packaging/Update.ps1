#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Apply', 'Status', 'Rollback', 'CheckOnline')][string]$Action,
    [string]$JournalPath,
    [string]$BundleRoot,
    [string]$SignaturePath,
    [string]$TrustedSignerSha256,
    [switch]$AllowDevelopment,
    [uri]$ManifestUri,
    [ValidateSet('server', 'workbench', 'cli')][string]$Component = 'server',
    [string]$TargetRoot,
    [string]$SourceConfig,
    [string]$SourceProgram,
    [switch]$StopOwnedService,
    [ValidateSet('verified','preflight','stopped','staged','verified_target','started')][string]$PauseAfter
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'UpdateTrust.psm1') -Force

function Get-LocalPath([string]$Value) {
    if (!$Value -or $Value -notmatch '^[a-zA-Z]:[\\/]' -or $Value.Substring(2).Contains(':')) {
        throw 'An absolute local Windows path is required.'
    }
    $full = [IO.Path]::GetFullPath($Value)
    for ($current = $full; $current; $current = [IO.Path]::GetDirectoryName($current)) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse points are unsupported for updates.' }
        }
    }
    $full
}

function Read-Document([string]$Path) {
    $file = Get-Item -LiteralPath (Get-LocalPath $Path)
    if ($file.PSIsContainer -or $file.Length -gt 64KB) { throw 'Invalid update document size or type.' }
    [IO.File]::ReadAllText($file.FullName) | ConvertFrom-Json
}

function Test-Within([string]$Path, [string]$Root) {
    $prefix = $Root.TrimEnd('\')
    return $Path -ieq $prefix -or $Path.StartsWith($prefix + '\', [StringComparison]::OrdinalIgnoreCase)
}

function Set-Document([string]$Path, $Document) {
    $tmp = $Path + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
    try {
        $bytes = [Text.UTF8Encoding]::new($false).GetBytes(($Document | ConvertTo-Json -Depth 20))
        if ($bytes.Length -gt 64KB) { throw 'Update document exceeds 64 KiB.' }
        $file = [IO.FileStream]::new($tmp, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try {
            $file.Write($bytes, 0, $bytes.Length)
            $file.Flush($true)
        } finally { $file.Dispose() }
        if ([IO.File]::Exists($Path)) { [IO.File]::Replace($tmp, $Path, $tmp + '.previous') }
        else { [IO.File]::Move($tmp, $Path) }
    } finally {
        foreach ($temporary in @($tmp, ($tmp + '.previous'))) {
            if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
        }
    }
}

function Set-Step([string]$Phase, [string]$Next) {
    $state.phase = $Phase
    $state.pending = if ($Next) { $Next } else { $null }
    Set-Document $journal $state
}

function Invoke-Candidate([string]$Executable, [string[]]$Arguments) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = Get-LocalPath $Executable
    $start.WorkingDirectory = [IO.Path]::GetDirectoryName($start.FileName)
    $start.Arguments = ($Arguments | ForEach-Object {
        if ($_ -match '["\r\n]' -or $_.EndsWith('\')) { throw 'Invalid executable argument.' }
        '"' + $_ + '"'
    }) -join ' '
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    if ([IO.Path]::GetFileName($Executable) -eq 'StarDataParadise.exe') {
        $start.EnvironmentVariables['QT_QPA_PLATFORM'] = 'windows'
        $start.EnvironmentVariables.Remove('QT_PLUGIN_PATH')
        $start.EnvironmentVariables.Remove('QT_QPA_PLATFORM_PLUGIN_PATH')
    }
    $process = [Diagnostics.Process]::Start($start)
    try {
        $out = $process.StandardOutput.ReadToEndAsync()
        $err = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(180000)) { $process.Kill(); throw 'Update verification process timed out.' }
        if ($process.ExitCode -ne 0) { throw ('Executable check failed. ' + $err.GetAwaiter().GetResult().Trim()) }
        $out.GetAwaiter().GetResult().Trim()
    } finally { $process.Dispose() }
}

function Get-StorageReport([string]$Config, [string]$CopyPath) {
    $argsForServer = @('--config', $Config, '--update-inspect')
    if ($CopyPath) { $argsForServer = @('--config', $Config, '--update-snapshot', $CopyPath) }
    $report = Invoke-Candidate $candidate $argsForServer | ConvertFrom-Json
    if ($report.format -ne 1 -or $report.sha256 -notmatch '^[a-fA-F0-9]{64}$' -or
        $report.lsn -notmatch '^[0-9]+$' -or $report.bytes -lt 0) { throw 'Invalid Server update report.' }
    foreach ($format in @('protocol','storage','wal','checkpoint','history','plsql_ir','model')) {
        if ($report.format_versions.$format -ne 1) { throw "Server reported an unsupported $format format." }
    }
    $report
}

function Assert-Compatibility($Manifest) {
    if ($Manifest.schema -ne 1 -or $Manifest.component -cne $Component -or
        $Manifest.from_version -notmatch '^\d+\.\d+\.\d+$' -or $Manifest.to_version -notmatch '^\d+\.\d+\.\d+$' -or
        [version]$Manifest.to_version -lt [version]$Manifest.from_version) {
        throw 'Unexpected update component or product version.'
    }
    foreach ($name in @('protocol','storage','wal','checkpoint','history','plsql_ir','model','native_abi')) {
        if ($Manifest.compatibility.$name -ne 1) { throw "Unsupported $name version." }
    }
    if ($Manifest.compatibility.qt -cne '6.11.3') { throw 'Unsupported Workbench Qt runtime version.' }
}

function Get-SourceSettings {
    if ((Get-FileHash -LiteralPath $state.source_config -Algorithm SHA256).Hash -ine $state.source_config_sha256) {
        throw 'Source configuration changed after the update was planned.'
    }
    $configuration = Read-Document $state.source_config
    $result = [ordered]@{}
    foreach ($field in $configuration.PSObject.Properties) { $result[$field.Name] = $field.Value }
    $directory = [IO.Path]::GetDirectoryName($state.source_config)
    foreach ($key in @('data_dir','tls_certificate','tls_private_key')) {
        $value = [string]$result[$key]
        if (![IO.Path]::IsPathRooted($value)) { $value = Join-Path $directory $value }
        $result[$key] = Get-LocalPath $value
    }
    $result
}

function New-TargetConfiguration([string]$Data) {
    $config = Get-SourceSettings
    $config.data_dir = $Data
    $config.log_dir = Join-Path $target 'logs'
    $config.temp_dir = Join-Path $target 'temp'
    if ($config.Contains('backup')) { $config.backup = Join-Path $target 'backups' }
    $config.instance_name = $state.instance
    $config.bind = '127.0.0.1'
    $config.port = 0
    $config.allow_remote = $false
    foreach ($path in @($config.log_dir, $config.temp_dir, $config['backup'])) {
        if ($path) { [void][IO.Directory]::CreateDirectory($path) }
    }
    $config
}

function Copy-VerifiedArtifact($Artifact, [string]$Destination) {
    $originalFile = Resolve-SdpUpdateArtifact -BundleRoot $bundle -Name $Artifact.name -Sha256 $Artifact.sha256 -Bytes $Artifact.bytes
    if (![IO.File]::Exists($Destination)) {
        $tempFile = $Destination + '.' + [Guid]::NewGuid().ToString('N') + '.part'
        try {
            [IO.File]::Copy($originalFile.FullName, $tempFile, $false)
            $stream = [IO.File]::Open($tempFile, 'Open', 'ReadWrite', 'None')
            try { $stream.Flush($true) } finally { $stream.Dispose() }
            if ((Get-FileHash -LiteralPath $tempFile -Algorithm SHA256).Hash -ine $Artifact.sha256) {
                throw 'Update artifact changed while staging.'
            }
            [IO.File]::Move($tempFile, $Destination)
        } finally { if ([IO.File]::Exists($tempFile)) { [IO.File]::Delete($tempFile) } }
    }
    [void](Resolve-SdpUpdateArtifact -BundleRoot ([IO.Path]::GetDirectoryName($Destination)) -Name ([IO.Path]::GetFileName($Destination)) -Sha256 $Artifact.sha256 -Bytes $Artifact.bytes)
}

function Assert-TargetOwner {
    $marker = Read-Document (Join-Path $target '.sdp-update-owner.json')
    if ($marker.schema -ne 1 -or $marker.id -cne $state.id) { throw 'Target update ownership could not be verified.' }
}

function Get-ArchiveSize([string]$Archive) {
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $expanded = 0L
    try {
        if ($zip.Entries.Count -gt 1024) { throw 'Update ZIP contains too many entries.' }
        foreach ($entry in $zip.Entries) {
            $name = $entry.FullName.Replace('\','/').TrimEnd('/')
            if ($name -notmatch '^[a-zA-Z0-9_. /-]+$' -or $name.StartsWith('/') -or
                ($name -split '/') -match '^(\.{1,2}|CON|AUX|NUL|PRN|COM[1-9]|LPT[1-9])(\.|$)' -or
                ($name -split '/') -match '[. ]$' -or !$seen.Add($name) -or
                ((($entry.ExternalAttributes -shr 16) -band 0xf000) -eq 0xa000)) {
                throw 'Update ZIP contains an unsafe entry.'
            }
            $expanded += $entry.Length
            if ($expanded -gt 1GB) { throw 'Update ZIP exceeds its uncompressed size budget.' }
        }
    } finally { $zip.Dispose() }
    $expanded
}

function Assert-ProductVersion([string]$Path, [string]$Version, [string]$ExpectedName) {
    $observed = Invoke-Candidate $Path @('--version')
    if ($observed -cne "$ExpectedName $Version") { throw 'The executable product version differs from the manifest.' }
}

function Expand-Client([string]$Archive, [string]$Destination) {
    $expandDir = $Destination + '.' + [Guid]::NewGuid().ToString('N') + '.part'
    if ([IO.Directory]::Exists($Destination)) { throw 'A prior client extraction needs review before retry.' }
    [IO.Compression.ZipFile]::ExtractToDirectory($Archive, $expandDir)
    [IO.Directory]::Move($expandDir, $Destination)
}

function Complete-Update($Manifest, $Artifact) {
    $expectedName = @{server='StarDataParadiseServer.exe';workbench='StarDataParadise.exe';cli='StarDataParadiseCLI.exe'}[$Component]
    if ($Component -eq 'server') {
        if ($Artifact.name -cne $expectedName) { throw 'The Server update requires the matching executable.' }
    } elseif ($Artifact.name -notmatch '\.zip$') { throw 'Client updates require a verified portable ZIP.' }

    if (!$state.last_complete) {
        $state.last_complete = 'verified'
        Set-Step 'verified' 'preflight'
        if ($PauseAfter -eq 'verified') { return }
    }
    if ($state.last_complete -eq 'verified') {
        $free = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($target)).AvailableFreeSpace
        if ($free -lt [long]$Artifact.bytes) { throw 'Insufficient disk space for update staging.' }
        if ($Component -eq 'server') {
            $config = Get-SourceSettings
            $sourceData = $config.data_dir
            if ((Test-Within $target $sourceData) -or (Test-Within $sourceData $target)) {
                throw 'Update target overlaps the source data directory.'
            }
            Assert-ProductVersion $state.source_program $state.from_version 'StarDataParadiseServer'
        }
        $state.last_complete = 'preflight'
        Set-Step 'preflight' 'stopped'
        if ($PauseAfter -eq 'preflight') { return }
    }
    if ($state.last_complete -eq 'preflight') {
        if ($Component -eq 'server' -and $state.requires_admin) {
            [void](Get-SourceSettings)
            $service = Invoke-Candidate $state.source_program @('--config',$state.source_config,'--service','status') | ConvertFrom-Json
            if ($null -eq $state.source_was_running) {
                $state.source_was_running = $service.state -eq 'running'
                Set-Document $journal $state
            }
            if ($service.state -eq 'running') {
                $stopped = Invoke-Candidate $state.source_program @('--config',$state.source_config,'--service','stop') | ConvertFrom-Json
                if ($stopped.state -ne 'stopped') { throw 'The owned instance did not stop.' }
            } elseif ($service.state -ne 'stopped') { throw 'The owned instance service state is not ready for update.' }
        }
        $state.last_complete = 'stopped'
        Set-Step 'stopped' 'staging'
        if ($PauseAfter -eq 'stopped') { return }
    }
    if ($state.last_complete -eq 'stopped') {
        Set-Step 'staging' 'staged'
        if (![IO.Directory]::Exists($target)) {
            [void][IO.Directory]::CreateDirectory($target)
            Set-Document (Join-Path $target '.sdp-update-owner.json') @{schema=1;id=$state.id}
        }
        Assert-TargetOwner
        $stagedAsset = Join-Path $target $Artifact.name
        Copy-VerifiedArtifact $Artifact $stagedAsset
        if ($Component -eq 'server') {
            $candidate = $stagedAsset
            Assert-ProductVersion $candidate $state.to_version 'StarDataParadiseServer'
            $source = Get-StorageReport $state.source_config ''
            if ($state.source_sha256 -and $state.source_sha256 -ine $source.sha256) { throw 'Source changed after upgrade staging began.' }
            if (!$state.source_sha256) {
                $state.source_sha256 = $source.sha256
                $state.source_lsn = $source.lsn
                Set-Document $journal $state
            }
            $free = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($target)).AvailableFreeSpace
            if ($free -lt [decimal]$source.bytes * 3) { throw 'Insufficient space for protected backup, target and verification copy.' }
            foreach ($leaf in @('backup','data')) {
                $location = Join-Path $target $leaf
                $configFile = Join-Path $target "$leaf.json"
                Set-Document $configFile (New-TargetConfiguration $location)
                if (![IO.Directory]::Exists($location)) {
                    $snapshot = Get-StorageReport $state.source_config $location
                    if ($snapshot.source_sha256 -ine $state.source_sha256 -or $snapshot.lsn -ne $state.source_lsn) {
                        throw 'Source changed across update snapshots.'
                    }
                    $state.snapshot_sha256[$leaf] = $snapshot.sha256
                    Set-Document $journal $state
                }
                $checked = Get-StorageReport $configFile ''
                if (!$state.snapshot_sha256[$leaf] -or $checked.sha256 -ine $state.snapshot_sha256[$leaf] -or
                    $checked.lsn -ne $state.source_lsn) { throw 'Staging copy identity could not be proven. Start with a fresh target.' }
            }
        } else {
            $expanded = Get-ArchiveSize $stagedAsset
            if ([IO.DriveInfo]::new([IO.Path]::GetPathRoot($target)).AvailableFreeSpace -lt $expanded) {
                throw 'Insufficient space for the portable client.'
            }
            Expand-Client $stagedAsset (Join-Path $target 'application')
        }
        $state.last_complete = 'staged'
        Set-Step 'staged' 'verified_target'
        if ($PauseAfter -eq 'staged') { return }
    }
    Assert-TargetOwner
    $stagedAsset = Join-Path $target $Artifact.name
    [void](Resolve-SdpUpdateArtifact -BundleRoot $target -Name $Artifact.name -Sha256 $Artifact.sha256 -Bytes $Artifact.bytes)
    if ($state.last_complete -eq 'staged') {
        if ($Component -eq 'server') {
            $candidate = $stagedAsset
            foreach ($leaf in @('backup','data')) {
                $checked = Get-StorageReport (Join-Path $target "$leaf.json") ''
                if ($checked.sha256 -ine $state.snapshot_sha256[$leaf] -or $checked.lsn -ne $state.source_lsn) {
                    throw 'A previously staged database was changed.'
                }
            }
            $test = Invoke-Candidate $candidate @('--self-test-install') | ConvertFrom-Json
            if (!$test.sql -or !$test.transaction -or !$test.history) { throw 'Candidate installation verification failed.' }
        } else {
            $appRoot = Join-Path $target 'application'
            $app = Join-Path $appRoot $expectedName
            if (![IO.File]::Exists((Join-Path $appRoot 'StarDataParadiseDLLler.dll'))) {
                throw 'The application-local Native SDK is missing.'
            }
            if ($Component -eq 'cli') { Assert-ProductVersion $app $state.to_version 'StarDataParadiseCLI' }
            else {
                $qt = (Get-Item -LiteralPath (Join-Path $appRoot 'Qt6Core.dll')).VersionInfo.ProductVersion
                if ($qt -cne ($Manifest.compatibility.qt + '.0')) { throw 'The extracted Qt runtime version differs from the manifest.' }
                [void](Invoke-Candidate $app @('--smoke-test'))
            }
        }
        $state.last_complete = 'verified_target'
        Set-Step 'verified_target' 'started'
        if ($PauseAfter -eq 'verified_target') { return }
    }
    if ($state.last_complete -eq 'verified_target') {
        if ($Component -eq 'server') {
            $candidate = $stagedAsset
            $configFile = Join-Path $target 'data.json'
            $ready = Invoke-Candidate $candidate @('--config', $configFile, '--update-readiness') | ConvertFrom-Json
            if ($ready.readiness -ne $true -or $ready.bind -cne '127.0.0.1' -or $ready.port -le 0) {
                throw 'The isolated candidate did not open its loopback TLS listener.'
            }
            $afterStartup = Get-StorageReport $configFile ''
            if ($afterStartup.sha256 -ine $state.snapshot_sha256.data -or $afterStartup.lsn -ne $state.source_lsn) {
                throw 'Target changed during the isolated startup check.'
            }
        }
        $state.last_complete = 'started'
        Set-Step 'started' 'complete'
        if ($PauseAfter -eq 'started') { return }
    }
    $state.last_complete = 'complete'
    $state.rollback_boundary = 'source_preserved'
    Set-Step 'complete' ''
}

try {
    if ($Action -eq 'CheckOnline') {
        Get-SdpOnlineUpdateManifest -ManifestUri $ManifestUri -TrustedSignerSha256 $TrustedSignerSha256 | ConvertTo-Json -Depth 15
        exit 0
    }
    $journal = Get-LocalPath $JournalPath
    if ($Action -eq 'Status') { Read-Document $journal | ConvertTo-Json -Depth 15; exit 0 }
    $target = (Get-LocalPath $TargetRoot).TrimEnd('\')
    if ($target -eq [IO.Path]::GetPathRoot($target).TrimEnd('\')) { throw 'A volume root cannot be an update target.' }
    if (Test-Within $journal $target) { throw 'Store the update journal outside the target directory.' }
    if ($Component -eq 'server') {
        $originalConfig = Get-LocalPath $SourceConfig
        $configuredData = [string](Read-Document $originalConfig).data_dir
        if (!$configuredData) { throw 'The source data directory is missing from the update configuration.' }
        if (![IO.Path]::IsPathRooted($configuredData)) {
            $configuredData = Join-Path ([IO.Path]::GetDirectoryName($originalConfig)) $configuredData
        }
        if (Test-Within $journal (Get-LocalPath $configuredData)) { throw 'Store the update journal outside the source data directory.' }
    }
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($journal))
    $lock = [IO.FileStream]::new((Get-LocalPath ($journal + '.lock')), 'OpenOrCreate', 'ReadWrite', 'None')
    try {
        $bundle = Get-LocalPath $BundleRoot
        $trustOptions = @{ManifestPath=(Join-Path $bundle 'update.json');AllowDevelopment=$AllowDevelopment}
        if ($SignaturePath) { $trustOptions.SignaturePath=Get-LocalPath $SignaturePath }
        if ($TrustedSignerSha256) { $trustOptions.TrustedSignerSha256=$TrustedSignerSha256 }
        $trusted = Read-SdpUpdateManifest @trustOptions
        $manifest = $trusted.manifest
        Assert-Compatibility $manifest
        if (@($manifest.artifacts).Count -ne 1) { throw 'Each update bundle must select one program artifact.' }
        $artifact = @($manifest.artifacts)[0]
        [void](Resolve-SdpUpdateArtifact -BundleRoot $bundle -Name $artifact.name -Sha256 $artifact.sha256 -Bytes $artifact.bytes)
        $source = if ($Component -eq 'server') { Get-LocalPath $SourceConfig } else { '' }
        $oldProgram = if ($Component -eq 'server') { Get-LocalPath $SourceProgram } else { '' }
        $configDigest = if ($source) { (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash } else { '' }
        $programDigest = if ($oldProgram) { (Get-FileHash -LiteralPath $oldProgram -Algorithm SHA256).Hash } else { '' }
        if ([IO.File]::Exists($journal)) {
            $state = Read-Document $journal
            if ($state.schema -ne 1 -or $state.component -cne $Component -or $state.manifest_sha256 -ine $trusted.manifestSha256 -or
                $state.trust -cne $trusted.trust -or $state.target_root -ine $target -or $state.source_config -ine $source -or
                $state.source_program -ine $oldProgram -or $state.requires_admin -ne [bool]$StopOwnedService -or
                $state.source_config_sha256 -ine $configDigest -or $state.source_program_sha256 -ine $programDigest) {
                throw 'The update request does not match the recorded journal.'
            }
            $state.snapshot_sha256 = @{backup=$state.snapshot_sha256.backup;data=$state.snapshot_sha256.data}
        } else {
            if ($Action -ne 'Apply' -or (Test-Path -LiteralPath $target)) { throw 'A new update needs a fresh target and journal.' }
            $id = [Guid]::NewGuid().ToString('N')
            $state = [pscustomobject]@{
                schema=1;component=$Component;from_version=$manifest.from_version;to_version=$manifest.to_version
                phase='planned';pending='verified';last_complete=$null;rollback_boundary='unknown'
                instance=('upd-' + $id.Substring(0,12));requires_admin=[bool]$StopOwnedService;id=$id
                manifest_sha256=$trusted.manifestSha256;trust=$trusted.trust;target_root=$target
                source_config=$source;source_program=$oldProgram;source_sha256=$null;source_lsn=$null
                source_config_sha256=$configDigest;source_program_sha256=$programDigest;source_was_running=$null
                snapshot_sha256=@{backup=$null;data=$null}
            }
            Set-Document $journal $state
        }
        try {
            if ($Action -eq 'Rollback') {
                if ($state.last_complete -ne 'complete') { throw 'The update has not completed.' }
                Assert-TargetOwner
                if ($Component -eq 'server') {
                    $candidate = Join-Path $target $artifact.name
                    [void](Resolve-SdpUpdateArtifact -BundleRoot $target -Name $artifact.name -Sha256 $artifact.sha256 -Bytes $artifact.bytes)
                    $now = Get-StorageReport (Join-Path $target 'data.json') ''
                    if ($now.sha256 -ine $state.snapshot_sha256.data -or $now.lsn -ne $state.source_lsn) {
                        $state.rollback_boundary = 'has_new_writes'
                        Set-Step 'safe_stop' 'rolled_back'
                        throw 'The candidate has new data. Rolling back would leave those writes behind.'
                    }
                    if ($state.source_was_running) {
                        $restored = Invoke-Candidate $state.source_program @('--config',$state.source_config,'--service','start') | ConvertFrom-Json
                        if ($restored.state -ne 'running') { throw 'The original owned service did not restart.' }
                    }
                }
                $state.rollback_boundary = 'rollback_complete'
                Set-Step 'rolled_back' ''
            } elseif ($state.phase -eq 'rolled_back') {
                throw 'This journal recorded a rollback. Choose a new update target.'
            } elseif ($state.last_complete -ne 'complete') { Complete-Update $manifest $artifact }
        } catch {
            if ($state.phase -notin @('safe_stop','rolled_back')) { Set-Step 'safe_stop' $state.pending }
            throw
        }
        $state | ConvertTo-Json -Depth 15
    } finally { $lock.Dispose() }
    exit 0
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
