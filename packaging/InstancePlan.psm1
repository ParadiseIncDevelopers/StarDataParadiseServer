Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-SdpLocalPath {
    param([string]$Path)
    if ($Path -notmatch '\A[A-Za-z]:[\\/]' -or $Path.IndexOfAny([char[]]'<>:"|?*', 2) -ge 0) {
        throw 'Choose an absolute path on a local Windows drive.'
    }
    $full = [IO.Path]::GetFullPath($Path)
    $root = [IO.Path]::GetPathRoot($full)
    if ($full -ieq $root) { throw 'Choose a dedicated folder or file below the drive root.' }
    $current = $root
    foreach ($part in $full.Substring($root.Length).TrimEnd('\').Split('\')) {
        if (!$part -or $part.StartsWith(' ') -or $part.EndsWith('.') -or $part.EndsWith(' ') -or
            $part -match '\A(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|\z)') {
            throw 'A setup path contains an unsafe name.'
        }
        $current = Join-Path $current $part
        $entry = Get-Item -LiteralPath $current -Force -ErrorAction SilentlyContinue
        if ($null -ne $entry -and ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Setup paths must not use symbolic links or junctions.'
        }
    }
    return $full.TrimEnd('\')
}

function Test-SdpPathOverlap {
    param([string]$First, [string]$Second)
    return $First -ieq $Second -or
        $First.StartsWith($Second + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $Second.StartsWith($First + '\', [StringComparison]::OrdinalIgnoreCase)
}

function Test-SdpInstancePath {
    param([string]$Path, [string]$Root)
    if (!$Path.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Data, log, temp and backup folders must be inside the dedicated instance root.'
    }
    if ((Test-Path -LiteralPath $Path) -and !(Test-Path -LiteralPath $Path -PathType Container)) {
        throw 'An instance folder is occupied by a file.'
    }
}

function New-SdpInstancePlan {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][ValidatePattern('\A[a-z0-9_-]{1,32}\z')][string]$InstanceName,
        [Parameter(Mandatory = $true)][string]$ProgramPath,
        [Parameter(Mandatory = $true)][string]$InstanceRoot,
        [string]$DataDirectory, [string]$LogDirectory, [string]$TempDirectory, [string]$BackupDirectory,
        [Parameter(Mandatory = $true)][string]$TlsCertificate,
        [Parameter(Mandatory = $true)][string]$TlsPrivateKey,
        [Parameter(Mandatory = $true)][string]$TlsCa,
        [string]$Bind = '127.0.0.1',
        [ValidateRange(0, 65535)][int]$Port = 7632,
        [switch]$AllowRemote, [switch]$InstallService, [switch]$Firewall,
        [Parameter(Mandatory = $true)][string[]]$Components
    )
    if ($InstanceName -cnotmatch '\A[a-z0-9_-]{1,32}\z' -or $Components -cnotcontains 'server.core' -or
        ($Components -ccontains 'server.deep-learning' -and $Components -cnotcontains 'server.ml-cpu') -or
        $Components -ccontains 'server.gpu') {
        throw 'The Server instance requires Core, compatible CPU modules and a valid instance name.'
    }
    $address = $null
    if (![Net.IPAddress]::TryParse($Bind, [ref]$address) -or $address.ToString() -cne $Bind) {
        throw 'Enter a canonical IPv4 or IPv6 address for the Server listener.'
    }
    $loopback = [Net.IPAddress]::IsLoopback($address)
    if ((!$loopback -and !$AllowRemote) -or ($Firewall -and ($loopback -or !$AllowRemote -or !$Port)) -or
        ($InstallService -and !$Port)) {
        throw 'Explicit remote access is required for a remote listener. Service and firewall rules require a fixed port.'
    }
    $root = Get-SdpLocalPath $InstanceRoot
    $program = Get-SdpLocalPath $ProgramPath
    if ([IO.Path]::GetFileName($program) -ine 'StarDataParadiseServer.exe' -or
        (Test-SdpPathOverlap ([IO.Path]::GetDirectoryName($program)) $root)) {
        throw 'The instance root and the Server installation are separate folders.'
    }
    $paths = @(
        $(if ($DataDirectory) { Get-SdpLocalPath $DataDirectory } else { Join-Path $root 'data' })
        $(if ($LogDirectory) { Get-SdpLocalPath $LogDirectory } else { Join-Path $root 'log' })
        $(if ($TempDirectory) { Get-SdpLocalPath $TempDirectory } else { Join-Path $root 'temp' })
        $(if ($BackupDirectory) { Get-SdpLocalPath $BackupDirectory } else { Join-Path $root 'backup' })
    )
    foreach ($path in $paths) { Test-SdpInstancePath (Get-SdpLocalPath $path) $root }
    for ($i = 0; $i -lt $paths.Count; $i++) {
        for ($j = $i + 1; $j -lt $paths.Count; $j++) {
            if (Test-SdpPathOverlap $paths[$i] $paths[$j]) { throw 'The Server instance directories must be independent.' }
        }
    }
    $certificate = Get-SdpLocalPath $TlsCertificate
    $key = Get-SdpLocalPath $TlsPrivateKey
    $ca = Get-SdpLocalPath $TlsCa
    foreach ($pem in $certificate, $key, $ca) {
        if (!(Test-Path -LiteralPath $pem -PathType Leaf) -or (Get-Item -LiteralPath $pem).Length -gt 1MB) {
            throw 'Select existing TLS certificate, private key and CA files.'
        }
    }
    if ($certificate -ieq $key -or $ca -ieq $key) { throw 'The private TLS key requires a separate file.' }
    $config = Get-SdpLocalPath (Join-Path $root 'server.json')
    if (($paths -contains $config) -or (@($certificate, $key, $ca) -contains $config)) {
        throw 'The configuration file must be separate from instance directories and TLS files.'
    }
    $configValue = [ordered]@{
        instance_name = $InstanceName; data_dir = $paths[0]; log_dir = $paths[1]; temp_dir = $paths[2]
        backup = $paths[3]; tls_certificate = $certificate; tls_private_key = $key
        bind = $Bind; allow_remote = [bool]$AllowRemote; port = $Port; max_connections = 32; timeout_ms = 20000
        decision_enabled = $Components -ccontains 'server.decision'
        ml_cpu_enabled = $Components -ccontains 'server.ml-cpu'
        mlp_cpu_enabled = $Components -ccontains 'server.deep-learning'
    }
    $service = 'StarDataParadise-' + $InstanceName
    $firewallName = $service + '-TLS-v1'
    $actions = @('Write instance configuration', 'Preserve existing storage and model files')
    if ($InstallService) { $actions += "Register $service with its own virtual account" }
    if ($Firewall) { $actions += "Allow inbound TCP $Port for $service on Private and Domain networks" }
    [pscustomobject]@{
        instanceName = $InstanceName; programPath = $program; instanceRoot = $root; configPath = $config
        dataDirectory = $paths[0]; walDirectory = $paths[0]; modelDirectory = $paths[0]
        logDirectory = $paths[1]; tempDirectory = $paths[2]; backupDirectory = $paths[3]
        tlsCertificate = $certificate; tlsPrivateKey = $key; tlsCa = $ca
        bind = $Bind; port = $Port; allowRemote = [bool]$AllowRemote
        installService = [bool]$InstallService; firewall = [bool]$Firewall
        serviceName = $service; serviceAccount = 'NT SERVICE\' + $service; firewallName = $firewallName
        components = @($Components); configuration = $configValue; actions = $actions
    }
}

function Get-SdpPortAvailable {
    param($Instance)
    if (!$Instance.port) { return $true }
    $listener = $null
    try {
        $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Parse($Instance.bind), $Instance.port)
        $listener.Server.ExclusiveAddressUse = $true
        $listener.Start()
        return $true
    }
    catch [Net.Sockets.SocketException] { return $false }
    finally { if ($null -ne $listener) { $listener.Stop() } }
}

function Get-SdpSetupPreflight {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)]$Plan, [string]$InstallRoot, $Instance)
    $issues = New-Object 'System.Collections.Generic.List[string]'
    $windows = [Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT
    if (!$windows) { $issues.Add('This bundle requires Windows.') }
    $architecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
    if ($architecture -ne 'x64' -or ![Environment]::Is64BitProcess) { $issues.Add('Run Setup on 64-bit x64 Windows.') }
    $os = [Environment]::OSVersion.Version
    if ($os.Build -lt 17763) { $issues.Add('Windows 10 1809 or Windows Server 2019 or newer is required.') }
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    try {
        $principal = New-Object Security.Principal.WindowsPrincipal($identity)
        $elevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    }
    finally { $identity.Dispose() }
    if (!$elevated -and ($Plan.requiresElevation -or
        ($null -ne $Instance -and ($Instance.installService -or $Instance.firewall)))) {
        $issues.Add('Administrator access is required for the selected machine-wide setup or instance integration.')
    }
    if (!$InstallRoot) { $InstallRoot = $env:LOCALAPPDATA }
    $volume = New-Object IO.DriveInfo([IO.Path]::GetPathRoot((Get-SdpLocalPath $InstallRoot)))
    $available = $volume.AvailableFreeSpace
    if ($available -lt $Plan.installedBytes) { $issues.Add('Insufficient disk space for the selected packages.') }
    $reboot = Test-Path -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending'
    $reboot = $reboot -or (Test-Path -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired')
    $sessionManager = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager'
    $pendingRegistry = Get-Item -LiteralPath $sessionManager
    try { $pendingFiles = $pendingRegistry.GetValue('PendingFileRenameOperations', $null) }
    finally { $pendingRegistry.Dispose() }
    $reboot = $reboot -or ($null -ne $pendingFiles -and @($pendingFiles).Count -gt 0)
    $runtime = 'Application-local MSVC runtime'
    $instanceExists = $serviceExists = $false
    $portAvailable = $true
    if ($null -ne $Instance) {
        $instanceExists = Test-Path -LiteralPath $Instance.configPath -PathType Leaf
        $serviceExists = $null -ne (Get-Service -Name $Instance.serviceName -ErrorAction SilentlyContinue)
        $portAvailable = Get-SdpPortAvailable $Instance
        if (!$instanceExists -and !$portAvailable) { $issues.Add('The configured Server address and port are in use.') }
    }
    [pscustomobject]@{
        ok = $issues.Count -eq 0; issues = $issues.ToArray(); os = $os.ToString(); architecture = $architecture
        elevated = $elevated; availableDiskBytes = $available; requiredDiskBytes = $Plan.installedBytes
        pendingReboot = $reboot; runtime = $runtime; serviceExists = $serviceExists
        instanceExists = $instanceExists; portAvailable = $portAvailable
    }
}

function Invoke-SdpInstanceServer {
    param($Plan, [string]$Config, [string[]]$Command, [Security.SecureString]$Password)
    $program = Get-SdpLocalPath $Plan.programPath
    if (!(Test-Path -LiteralPath $program -PathType Leaf)) { throw 'The selected Server executable is unavailable.' }
    $arguments = @('--config', (Get-SdpLocalPath $Config)) + $Command
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $program
    $start.Arguments = ($arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $utf8 = New-Object Text.UTF8Encoding($false, $true)
    if ($null -ne $start.PSObject.Properties['StandardInputEncoding']) {
        $start.StandardInputEncoding = $utf8
    }
    $start.WorkingDirectory = [IO.Path]::GetDirectoryName($program)
    # .NET Framework uses Console.InputEncoding and emits its preamble when opening
    # the anonymous pipe. Set a BOM-free encoding for that one launch.
    $inputLock = [Diagnostics.ProcessStartInfo]
    [Threading.Monitor]::Enter($inputLock)
    try {
        $inputEncoding = [Console]::InputEncoding
        try {
            [Console]::InputEncoding = $utf8
            $process = [Diagnostics.Process]::Start($start)
        }
        finally { [Console]::InputEncoding = $inputEncoding }
    }
    finally { [Threading.Monitor]::Exit($inputLock) }
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if ($null -ne $Password) {
            $pointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($Password)
            $characters = New-Object char[] $Password.Length
            $encoded = $null
            try {
                for ($i = 0; $i -lt $characters.Length; $i++) {
                    $characters[$i] = [char]([Runtime.InteropServices.Marshal]::ReadInt16($pointer, $i * 2) -band 0xFFFF)
                }
                $encoded = $utf8.GetBytes($characters)
                if ($encoded.Length -lt 16 -or $encoded.Length -gt 1024 -or
                    ($encoded -contains 0) -or ($encoded -contains 10) -or ($encoded -contains 13)) {
                    throw 'Enter an administrator password with 16 to 1024 UTF-8 bytes on one line.'
                }
                $stdin = $process.StandardInput.BaseStream
                $stdin.Write($encoded, 0, $encoded.Length)
                $stdin.WriteByte(10)
                $stdin.Write($encoded, 0, $encoded.Length)
                $stdin.WriteByte(10)
                $stdin.Flush()
            }
            finally {
                [Array]::Clear($characters, 0, $characters.Length)
                if ($null -ne $encoded) { [Array]::Clear($encoded, 0, $encoded.Length) }
                [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer)
            }
        }
        $process.StandardInput.Close()
        if (!$process.WaitForExit(45000)) {
            $process.Kill()
            $process.WaitForExit()
            throw "The Server operation '$($Command[0])' did not finish in time."
        }
        $output = $stdout.GetAwaiter().GetResult()
        $errors = $stderr.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) {
            if ($Command[0] -eq '--check-package-config' -and $errors -match 'Decision') {
                throw 'The proposed configuration cannot remove Decision while active rules remain.'
            }
            if ($Command[0] -eq '--service' -and $errors -match 'match|mapped|registration|instance') {
                throw 'The Server service registration does not match this instance.'
            }
            $message = $errors.Trim()
            if ($Command[0] -eq '--bootstrap-admin' -and $message -cin @('Unable to read a valid password',
                    'Passwords must match and contain 16 to 1024 UTF-8 bytes', 'Password must be valid UTF-8',
                    'Cannot publish administrator verifier', 'Invalid configuration',
                    'A configured directory or TLS file is missing', 'Administrator already configured')) {
                throw $message
            }
            throw "The Server operation '$($Command[0])' failed (exit $($process.ExitCode))."
        }
        return $output
    }
    finally {
        if (!$process.HasExited) {
            $process.StandardInput.Close()
            $process.Kill()
            $process.WaitForExit()
        }
        $process.Dispose()
    }
}

function Get-SdpExistingInstance {
    param($Plan)
    $path = Get-SdpLocalPath $Plan.configPath
    if (!(Test-Path -LiteralPath $path)) { return }
    $file = Get-Item -LiteralPath $path -Force
    if ($file.PSIsContainer -or $file.Length -gt 16KB) { throw 'The existing Server configuration is invalid.' }
    $current = Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($null -eq $current.PSObject.Properties['instance_name'] -or $current.instance_name -cne $Plan.instanceName) {
        throw 'The existing Server configuration belongs to a different instance.'
    }
    foreach ($entry in @{data_dir=$Plan.dataDirectory; log_dir=$Plan.logDirectory; temp_dir=$Plan.tempDirectory; backup=$Plan.backupDirectory}.GetEnumerator()) {
        $item = $current.PSObject.Properties[$entry.Key]
        if ($null -eq $item -and $entry.Key -eq 'backup') { continue }
        if ($null -eq $item -or $item.Value -isnot [string]) { throw 'The existing Server configuration is invalid.' }
        $value = $item.Value
        if (![IO.Path]::IsPathRooted($value)) { $value = Join-Path $Plan.instanceRoot $value }
        if ((Get-SdpLocalPath $value) -ine $entry.Value) {
            throw 'Setup must keep the existing instance data and runtime directories.'
        }
    }
    return $current
}

function Write-SdpJsonFile {
    param([string]$Path, $Value)
    $encoding = New-Object Text.UTF8Encoding($false, $true)
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 10), $encoding)
}

function New-SdpConfigCandidate {
    param($Plan, $Current, [switch]$DisableDecision)
    $path = Join-Path $Plan.instanceRoot ('.sdp-setup-' + [Guid]::NewGuid().ToString('N') + '.json')
    $output = [ordered]@{}
    if ($null -ne $Current) {
        foreach ($property in $Current.PSObject.Properties) { $output[$property.Name] = $property.Value }
    }
    foreach ($entry in $Plan.configuration.GetEnumerator()) {
        if ($null -ne $Current -and $entry.Key -in 'max_connections', 'timeout_ms' -and $output.Contains($entry.Key)) { continue }
        $output[$entry.Key] = $entry.Value
    }
    if ($DisableDecision) { $output['decision_enabled'] = $false }
    Write-SdpJsonFile $path $output
    return $path
}

function Test-SdpInstanceConfiguration {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)]$Plan, [switch]$DisableDecision)
    $current = Get-SdpExistingInstance $Plan
    if ($null -eq $current) { return }
    $candidate = New-SdpConfigCandidate $Plan $current -DisableDecision:$DisableDecision
    try { [void](Invoke-SdpInstanceServer $Plan $candidate @('--check-package-config')) }
    finally { [IO.File]::Delete($candidate) }
}

function Set-SdpServiceAccess {
    param($Plan, $Saved)
    $account = [Security.Principal.NTAccount]::new($Plan.serviceAccount)
    $sid = $account.Translate([Security.Principal.SecurityIdentifier])
    $modify = [Security.AccessControl.FileSystemRights]::Modify
    $read = [Security.AccessControl.FileSystemRights]::ReadAndExecute
    foreach ($target in @(
        @{path=$Plan.instanceRoot; rights=$read; inherit=$false}
        @{path=$Plan.configPath; rights=$read; inherit=$false}
        @{path=$Plan.tlsCertificate; rights=$read; inherit=$false}
        @{path=$Plan.tlsPrivateKey; rights=[Security.AccessControl.FileSystemRights]::Read; inherit=$false}
        @{path=(Join-Path $Plan.dataDirectory 'admin-v1.json'); rights=[Security.AccessControl.FileSystemRights]::Read; inherit=$false}
        @{path=$Plan.dataDirectory; rights=$modify; inherit=$true}
        @{path=$Plan.logDirectory; rights=$modify; inherit=$true}
        @{path=$Plan.tempDirectory; rights=$modify; inherit=$true}
        @{path=$Plan.backupDirectory; rights=$modify; inherit=$true}
    )) {
        $path = Get-SdpLocalPath $target.path
        $inheritance = [Security.AccessControl.InheritanceFlags]::None
        if ($target.inherit) {
            $inheritance = [Security.AccessControl.InheritanceFlags]::ContainerInherit -bor [Security.AccessControl.InheritanceFlags]::ObjectInherit
        }
        $access = Get-Acl -LiteralPath $path
        if (!$Saved.Contains($path)) { $Saved[$path] = $access.Sddl }
        $rule = [Security.AccessControl.FileSystemAccessRule]::new($sid, $target.rights, $inheritance,
            [Security.AccessControl.PropagationFlags]::None, [Security.AccessControl.AccessControlType]::Allow)
        $access.SetAccessRule($rule)
        Set-Acl -LiteralPath $path -AclObject $access
    }
}

function Restore-SdpServiceAccess {
    param($Saved)
    foreach ($entry in $Saved.GetEnumerator()) {
        $path = Get-SdpLocalPath $entry.Key
        if (Test-Path -LiteralPath $path) {
            $acl = Get-Acl -LiteralPath $path
            $acl.SetSecurityDescriptorSddlForm($entry.Value, [Security.AccessControl.AccessControlSections]::Access)
            Set-Acl -LiteralPath $path -AclObject $acl
        }
    }
}

function Get-SdpFirewallRule {
    param($Plan)
    Get-NetFirewallRule -Name $Plan.firewallName -PolicyStore PersistentStore -ErrorAction SilentlyContinue
}

function Get-SdpInstanceServiceState {
    param($Plan)
    $reply = Invoke-SdpInstanceServer $Plan $Plan.configPath @('--service', 'status') | ConvertFrom-Json
    if ($reply.instance -cne $Plan.instanceName -or $reply.state -cnotin
        'not_installed', 'running', 'stopped', 'start_pending', 'stop_pending', 'paused', 'pause_pending', 'continue_pending') {
        throw 'The Server returned an unexpected Windows service status.'
    }
    return $reply.state
}

function New-SdpInstanceFirewall {
    param($Plan)
    if ($null -ne (Get-SdpFirewallRule $Plan)) { throw 'A firewall rule already uses this instance integration name.' }
    $local = if ($Plan.bind -in '0.0.0.0', '::') { 'Any' } else { $Plan.bind }
    $arguments = @{
        Name = $Plan.firewallName; DisplayName = $Plan.firewallName; Description = 'StarDataParadise Setup v1'
        PolicyStore = 'PersistentStore'; Direction = 'Inbound'; Action = 'Allow'; Enabled = 'True'
        Program = $Plan.programPath; Profile = @('Domain', 'Private'); Protocol = 'TCP'
        LocalPort = $Plan.port; LocalAddress = $local
    }
    if ($Plan.installService) { $arguments.Service = $Plan.serviceName }
    New-NetFirewallRule @arguments | Out-Null
}

function Remove-SdpInstanceFirewall {
    param($Plan)
    $rule = Get-SdpFirewallRule $Plan
    if ($null -eq $rule) { return }
    Assert-SdpFirewallMatch $Plan $rule
    $rule | Remove-NetFirewallRule
}

function Assert-SdpFirewallMatch {
    param($Plan, $Rule)
    $app = $Rule | Get-NetFirewallApplicationFilter
    $port = $Rule | Get-NetFirewallPortFilter
    $address = $Rule | Get-NetFirewallAddressFilter
    $service = $Rule | Get-NetFirewallServiceFilter
    $local = if ($Plan.bind -in '0.0.0.0', '::') { 'Any' } else { $Plan.bind }
    $ownedService = if ($Plan.installService) { $Plan.serviceName } else { 'Any' }
    $profiles = ([string]$Rule.Profile -split ',\s*') | Sort-Object
    if ($Rule.Description -cne 'StarDataParadise Setup v1' -or $Rule.Direction -ne 'Inbound' -or
        $Rule.Action -ne 'Allow' -or $Rule.Enabled -ne 'True' -or ($profiles -join ',') -ne 'Domain,Private' -or
        $app.Program -ine $Plan.programPath -or $port.Protocol -ne 'TCP' -or $port.LocalPort -ne "$($Plan.port)" -or
        @($address.LocalAddress).Count -ne 1 -or $address.LocalAddress -ne $local -or $service.Service -ine $ownedService) {
        throw 'The firewall rule does not match this instance integration.'
    }
}

function Initialize-SdpInstance {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]$Plan,
        [ValidatePattern('\A[A-Za-z0-9_.-]{1,64}\z')][string]$AdminUser = 'local-admin',
        [Security.SecureString]$AdminPassword,
        [switch]$UpdateConfiguration
    )
    $current = Get-SdpExistingInstance $Plan
    if ($null -ne $current -and !$UpdateConfiguration) {
        throw 'The instance already exists. Use the configuration update workflow.'
    }
    if ($null -ne $current -and $null -ne $AdminPassword) {
        throw 'Use the existing instance account and verifier during configuration updates.'
    }
    $createdFolders = New-Object 'System.Collections.Generic.List[string]'
    $savedAccess = [ordered]@{}
    $candidate = $backup = $null
    $completed = $false
    $configWritten = $bootstrapped = $serviceCreated = $firewallCreated = $false
    $store = Join-Path $Plan.dataDirectory 'store.sdp'
    $verifier = Join-Path $Plan.dataDirectory 'admin-v1.json'
    $guarded = $null -ne $current
    $dataPresent = Test-Path -LiteralPath $store -PathType Leaf
    $currentFirewall = $null
    if ($Plan.firewall) {
        $currentFirewall = Get-SdpFirewallRule $Plan
        if ($null -ne $currentFirewall) { Assert-SdpFirewallMatch $Plan $currentFirewall }
    }
    $before = $null
    if ($guarded) {
        $before = (Get-FileHash -LiteralPath $Plan.configPath).Hash
        if (!$dataPresent -and $null -ne (Get-ChildItem -LiteralPath $Plan.dataDirectory -Force -ErrorAction SilentlyContinue |
                Where-Object Name -NotIn @('admin-v1.json', '.sdp.lock') | Select-Object -First 1)) {
            throw 'The existing instance data directory requires storage recovery.'
        }
    }
    elseif ((Test-Path -LiteralPath $Plan.instanceRoot) -and
        $null -ne (Get-ChildItem -LiteralPath $Plan.instanceRoot -Force | Select-Object -First 1)) {
        throw 'Choose an empty instance folder or an existing verified configuration.'
    }
    if (!$guarded -and $null -eq $AdminPassword) {
        throw 'A new Server instance requires an administrator password.'
    }
    if (!$guarded -and !(Get-SdpPortAvailable $Plan)) { throw 'The configured Server address and port are in use.' }
    try {
        foreach ($directory in @($Plan.instanceRoot, $Plan.dataDirectory, $Plan.logDirectory, $Plan.tempDirectory, $Plan.backupDirectory)) {
            $path = Get-SdpLocalPath $directory
            if (!(Test-Path -LiteralPath $path)) {
                [void][IO.Directory]::CreateDirectory($path)
                $createdFolders.Add($path)
            }
        }
        $candidate = New-SdpConfigCandidate $Plan $current
        if ($guarded) {
            [void](Invoke-SdpInstanceServer $Plan $candidate @('--check-package-config'))
            if ((Get-FileHash -LiteralPath $Plan.configPath).Hash -ne $before) {
                throw 'The instance configuration changed after evaluation. Open Setup again.'
            }
            $backup = Join-Path $Plan.instanceRoot ('.sdp-config-' + [Guid]::NewGuid().ToString('N') + '.bak')
            [IO.File]::Replace($candidate, $Plan.configPath, $backup)
        }
        else {
            [IO.File]::Move($candidate, $Plan.configPath)
        }
        $configWritten = $true
        if (!$guarded) {
            [void](Invoke-SdpInstanceServer $Plan $Plan.configPath @('--bootstrap-admin', $AdminUser) $AdminPassword)
            [void](Invoke-SdpInstanceServer $Plan $Plan.configPath @('--initialize-storage'))
            $bootstrapped = $true
        }
        if ($Plan.installService) {
            if ((Get-SdpInstanceServiceState $Plan) -eq 'not_installed') {
                [void](Invoke-SdpInstanceServer $Plan $Plan.configPath @('--service', 'install'))
                $serviceCreated = $true
            }
            Set-SdpServiceAccess $Plan $savedAccess
        }
        if ($Plan.firewall -and $null -eq $currentFirewall) {
            New-SdpInstanceFirewall $Plan
            $firewallCreated = $true
        }
        $completed = $true
        [pscustomobject]@{
            instanceName = $Plan.instanceName; configPath = $Plan.configPath; dataDirectory = $Plan.dataDirectory
            serviceName = $Plan.serviceName; firewallName = $Plan.firewallName
            created = !$guarded; serviceInstalled = $serviceCreated; firewallInstalled = $firewallCreated
            retainedData = $dataPresent; initializedStorage = $bootstrapped
        }
    }
    catch {
        try {
            if ($firewallCreated) { Remove-SdpInstanceFirewall $Plan }
            if ($savedAccess.Count) { Restore-SdpServiceAccess $savedAccess }
            if ($serviceCreated) { [void](Invoke-SdpInstanceServer $Plan $Plan.configPath @('--service', 'remove')) }
        }
        catch { Write-Warning 'A newly added service or firewall entry could not be restored automatically.' }
        if ($configWritten) {
            if ($guarded) {
                $rollback = Join-Path $Plan.instanceRoot ('.sdp-rollback-' + [Guid]::NewGuid().ToString('N') + '.bak')
                [IO.File]::Replace($backup, $Plan.configPath, $rollback)
                [IO.File]::Delete($rollback)
            }
            elseif (!(Test-Path -LiteralPath $store -PathType Leaf)) {
                [IO.File]::Delete($Plan.configPath)
                [IO.File]::Delete($verifier)
                [IO.File]::Delete((Join-Path $Plan.dataDirectory '.sdp.lock'))
            }
        }
        throw
    }
    finally {
        if ($candidate) { [IO.File]::Delete($candidate) }
        if ($backup) { [IO.File]::Delete($backup) }
        if (!$completed -and !($configWritten -and !$guarded -and (Test-Path -LiteralPath $store -PathType Leaf))) {
            for ($index = $createdFolders.Count - 1; $index -ge 0; $index--) {
                $directory = Get-SdpLocalPath $createdFolders[$index]
                if ((Test-Path -LiteralPath $directory -PathType Container) -and
                    $null -eq (Get-ChildItem -LiteralPath $directory -Force | Select-Object -First 1)) {
                    [IO.Directory]::Delete($directory)
                }
            }
        }
    }
}

function Remove-SdpInstanceIntegration {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]$Plan,
        [switch]$ServiceOwned,
        [switch]$FirewallOwned
    )
    if ($null -eq (Get-SdpExistingInstance $Plan)) { throw 'The Server instance configuration is unavailable.' }
    $serviceState = if ($ServiceOwned) { Get-SdpInstanceServiceState $Plan } else { 'not_installed' }
    $firewallRule = $null
    if ($FirewallOwned) {
        $firewallRule = Get-SdpFirewallRule $Plan
        if ($null -ne $firewallRule) { Assert-SdpFirewallMatch $Plan $firewallRule }
    }
    $firewallRemoved = $serviceStopped = $false
    try {
        if ($null -ne $firewallRule) {
            Remove-SdpInstanceFirewall $Plan
            $firewallRemoved = $true
        }
        if ($serviceState -ne 'not_installed') {
            [void](Invoke-SdpInstanceServer $Plan $Plan.configPath @('--service', 'stop'))
            $serviceStopped = $true
            [void](Invoke-SdpInstanceServer $Plan $Plan.configPath @('--service', 'remove'))
        }
    }
    catch {
        if ($serviceStopped -and $serviceState -eq 'running') {
            try { [void](Invoke-SdpInstanceServer $Plan $Plan.configPath @('--service', 'start')) }
            catch { Write-Warning 'The existing instance service could not be restarted automatically.' }
        }
        if ($firewallRemoved) {
            try { New-SdpInstanceFirewall $Plan }
            catch { Write-Warning 'The existing instance firewall rule could not be restored automatically.' }
        }
        throw
    }
}

Export-ModuleMember -Function Get-SdpSetupPreflight, New-SdpInstancePlan, Initialize-SdpInstance,
    Test-SdpInstanceConfiguration, Remove-SdpInstanceIntegration
