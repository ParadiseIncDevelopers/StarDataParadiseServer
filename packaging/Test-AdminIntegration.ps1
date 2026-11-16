param(
    [string]$LogPath = (Join-Path $PSScriptRoot '..\out\sdp-m9\stage45\admin-run\admin-acceptance.log'),
    [string]$ExitPath = (Join-Path $PSScriptRoot '..\out\sdp-m9\stage45\admin-run\admin-acceptance.exit')
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Evidence([string]$Step) {
    $line = '{0:o} {1}' -f [DateTimeOffset]::Now, $Step
    Add-Content -LiteralPath $LogPath -Encoding UTF8 -Value $line
    Write-Host $line
}
function Require([bool]$Condition, [string]$Problem) {
    if (!$Condition) { throw $Problem }
}
function Invoke-ServiceCommand($Plan, [string]$Action) {
    $result = & $Plan.programPath --config $Plan.configPath --service $Action
    Require ($LASTEXITCODE -eq 0) "Server service $Action failed (exit $LASTEXITCODE)"
    $status = $result | ConvertFrom-Json
    Require ($status.instance -ceq $Plan.instanceName) 'Unexpected service instance identity'
    return $status
}
function Assert-Unregistered($Plan) {
    for ($attempt = 0; $attempt -lt 30; $attempt++) {
        if (!(Get-Service -Name $Plan.serviceName -ErrorAction SilentlyContinue)) { return }
        Start-Sleep -Milliseconds 100
    }
    throw 'Fixture service remained registered'
}
function Get-FreeLoopbackPort {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    try { $listener.Start(); return $listener.LocalEndpoint.Port }
    finally { $listener.Stop() }
}
function Get-InstancePlan([string]$Name, [string]$Execution, [string]$Instance, [int]$Port, [string]$Bind = '127.0.0.1', [switch]$Firewall) {
    $tls = Join-Path $case 'tls'
    return New-SdpInstancePlan -InstanceName $Name -ProgramPath $Execution -InstanceRoot $Instance -Port $Port -Bind $Bind -Components @('server.core') -InstallService -TlsCertificate (Join-Path $tls 'server.pem') -TlsPrivateKey (Join-Path $tls 'server.key') -TlsCa (Join-Path $tls 'ca.pem') -AllowRemote:$Firewall -Firewall:$Firewall
}
function Assert-Available($Plan) {
    Require (!(Get-Service -Name $Plan.serviceName -ErrorAction SilentlyContinue)) 'Fixture service name is already in use'
    Require (!(Get-NetFirewallRule -Name $Plan.firewallName -PolicyStore PersistentStore -ErrorAction SilentlyContinue)) 'Fixture firewall rule name is already in use'
}
function Assert-Identity($Plan) {
    $registered = Get-CimInstance Win32_Service -Filter ("Name='{0}'" -f $Plan.serviceName)
    Require ($null -ne $registered -and $registered.StartName -ieq $Plan.serviceAccount) 'Expected virtual account was not registered'
    $expected = '"{0}" --service-run "{1}" {2}' -f $Plan.programPath, $Plan.configPath, $Plan.instanceName
    Require ($registered.PathName -ieq $expected) 'Registered image does not match the fixture binary and config'
}
function Invoke-SqlFixture($Plan, [string]$Operation) {
    $probe = Join-Path $serverRoot 'tests\probe_scm_tls.py'
    $verifier = Join-Path $Plan.dataDirectory 'admin-v1.json'
    $inputLine = [Text.Encoding]::ASCII.GetBytes($password + [char]10)
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $python
    $start.Arguments = '-B "{0}" {1} "{2}" {3} "{4}"' -f $probe, $Plan.port, $Plan.tlsCa, $Operation, $verifier
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $child = [Diagnostics.Process]::Start($start)
    try {
        $out = $child.StandardOutput.ReadToEndAsync()
        $err = $child.StandardError.ReadToEndAsync()
        $child.StandardInput.BaseStream.Write($inputLine, 0, $inputLine.Length)
        $child.StandardInput.BaseStream.Flush()
        $child.StandardInput.Close()
        if (!$child.WaitForExit(30000)) {
            $child.Kill()
            throw "TLS probe '$Operation' timed out"
        }
        if ($child.ExitCode -ne 0) { throw "TLS probe '$Operation' failed: $($err.GetAwaiter().GetResult().Trim())" }
        Evidence ($out.GetAwaiter().GetResult().Trim())
    } finally {
        [Array]::Clear($inputLine, 0, $inputLine.Length)
        $child.Dispose()
    }
}
function Get-DataFingerprint($Plan) {
    return @(Get-ChildItem -LiteralPath $Plan.dataDirectory -File -Force | Sort-Object Name | ForEach-Object {
        $_.Name + '=' + (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    }) -join ','
}
function Remove-OwnedIntegration($Plan, [switch]$Firewall) {
    if (Test-Path -LiteralPath $Plan.configPath -PathType Leaf) {
        Remove-SdpInstanceIntegration -Plan $Plan -ServiceOwned -FirewallOwned:$Firewall
        Assert-Unregistered $Plan
        if ($Firewall) {
            Require (!(Get-NetFirewallRule -Name $Plan.firewallName -PolicyStore PersistentStore -ErrorAction SilentlyContinue)) 'Fixture rule remained registered'
        }
    }
}
function Test-ServiceFixture {
    $plan = Get-InstancePlan "s45svc-$token" $binary (Join-Path $case 'service') (Get-FreeLoopbackPort)
    Assert-Available $plan
    try {
        $setup = Initialize-SdpInstance -Plan $plan -AdminPassword $secret
        Require ($setup.initializedStorage -and $setup.serviceInstalled) 'Setup did not initialize and register the fixture'
        Assert-Identity $plan
        $sid = [Security.Principal.NTAccount]::new($plan.serviceAccount).Translate([Security.Principal.SecurityIdentifier])
        $verifier = Join-Path $plan.dataDirectory 'admin-v1.json'
        $access = @( (Get-Acl -LiteralPath $verifier).GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier]) |
            Where-Object { $_.IdentityReference -eq $sid -and $_.AccessControlType -eq 'Allow' } )
        Evidence "Verifier service SID read ACE present $([bool]$access.Count)"
        $started = Invoke-ServiceCommand $plan 'start'
        Require ($started.state -ceq 'running' -and $started.process_id -gt 0) 'Service did not report Running'
        $process = Get-CimInstance Win32_Process -Filter "ProcessId=$($started.process_id)"
        Require ($null -ne $process) 'Registered service PID is missing'
        $owner = Invoke-CimMethod -InputObject $process -MethodName GetOwner
        Require ($owner.ReturnValue -eq 0 -and "$($owner.Domain)\$($owner.User)" -ieq $plan.serviceAccount) 'Server process is not running under its virtual account'
        Invoke-SqlFixture $plan 'seed'
        Require ((Invoke-ServiceCommand $plan 'stop').state -ceq 'stopped') 'Service did not stop'
        Require ((Invoke-ServiceCommand $plan 'start').state -ceq 'running') 'Service did not restart'
        Invoke-SqlFixture $plan 'verify'
        Require ((Invoke-ServiceCommand $plan 'stop').state -ceq 'stopped') 'Restarted service did not stop'
        $before = Get-DataFingerprint $plan
        Remove-OwnedIntegration $plan
        Require ((Get-DataFingerprint $plan) -ceq $before) 'Service removal modified fixture data'
        & $plan.programPath --config $plan.configPath --check-storage
        Require ($LASTEXITCODE -eq 0) 'Storage integrity check failed after service removal'
        Evidence 'PASS virtual account SCM start, TLS SQL, restart, stop, removal and storage preservation'
    }
    finally { Remove-OwnedIntegration $plan }
}
function Test-FirewallFixture {
    $name = "s45fw-$token"
    $fixture = Join-Path $case 'firewall'
    $port = Get-FreeLoopbackPort
    $reserved = '192.0.2.42'
    Require (!(Get-NetIPAddress -IPAddress $reserved -ErrorAction SilentlyContinue)) 'Documentation address is assigned locally'
    $local = Get-InstancePlan $name $binary $fixture $port
    $plan = Get-InstancePlan $name $binary $fixture $port $reserved -Firewall
    Assert-Available $plan
    try {
        $setup = Initialize-SdpInstance -Plan $local -AdminPassword $secret
        Require ($setup.initializedStorage -and $setup.serviceInstalled) 'Firewall fixture initialization failed'
        $before = Get-DataFingerprint $local
        $configured = Initialize-SdpInstance -Plan $plan -UpdateConfiguration
        Require ($configured.firewallInstalled -and !$configured.serviceInstalled -and $configured.retainedData) 'Setup did not create the fixture firewall rule'
        Assert-Identity $plan
        $rules = @(Get-NetFirewallRule -Name $plan.firewallName -PolicyStore PersistentStore)
        Require ($rules.Count -eq 1) 'Expected exactly one firewall rule'
        $rule = $rules[0]
        $application = $rule | Get-NetFirewallApplicationFilter
        $transport = $rule | Get-NetFirewallPortFilter
        $address = $rule | Get-NetFirewallAddressFilter
        $service = $rule | Get-NetFirewallServiceFilter
        $profiles = (([string]$rule.Profile -split ',\s*') | Sort-Object) -join ','
        Require ($rule.Direction -eq 'Inbound' -and $rule.Action -eq 'Allow' -and $rule.Enabled -eq 'True' -and $profiles -eq 'Domain,Private') 'Firewall scope or action differs from the plan'
        Require ($application.Program -ieq $binary -and $transport.Protocol -eq 'TCP' -and $transport.LocalPort -eq [string]$port -and $service.Service -ieq $plan.serviceName) 'Firewall program, service or port differs from the plan'
        Require (@($address.LocalAddress).Count -eq 1 -and $address.LocalAddress -in @($reserved, "$reserved/32")) 'Firewall bind address differs from the isolated plan'
        Require ((Invoke-ServiceCommand $plan 'status').state -ceq 'stopped') 'Firewall test service unexpectedly started'
        $again = Initialize-SdpInstance -Plan $plan -UpdateConfiguration
        Require (!$again.firewallInstalled -and !$again.serviceInstalled) 'Reconfiguration duplicated an existing integration'
        Remove-OwnedIntegration $plan -Firewall
        Require ((Get-DataFingerprint $plan) -ceq $before) 'Firewall removal modified fixture data'
        Evidence 'PASS scoped Windows Firewall create, exact filters, idempotent reconfigure and removal'
    }
    finally { Remove-OwnedIntegration $plan -Firewall }
}

$code = 1
$secret = $null
$password = $null
try {
    foreach ($path in @($LogPath, $ExitPath)) {
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($path)))
    }
    Set-Content -LiteralPath $LogPath -Encoding UTF8 -Value 'Stage45 Windows SCM and Firewall acceptance'
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    Require (([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) 'Run from an elevated PowerShell'
    $serverRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).ProviderPath
    Import-Module (Join-Path $serverRoot 'packaging\InstancePlan.psm1') -Force
    $source = (Resolve-Path -LiteralPath (Join-Path $serverRoot 'out\sdp-m9\stage45\server-build\Release\StarDataParadiseServer.exe')).ProviderPath
    $openssl = (Resolve-Path -LiteralPath (Join-Path $serverRoot 'out\sdp-m1\dependencies\openssl-install\bin\openssl.exe')).ProviderPath
    $python = (Get-Command python.exe -CommandType Application | Select-Object -First 1).Source
    $token = [guid]::NewGuid().ToString('N').Substring(0, 12)
    $programData = (Resolve-Path -LiteralPath ([Environment]::GetFolderPath('CommonApplicationData'))).ProviderPath
    $case = Join-Path $programData ('StarDataParadise-Stage45-QA-' + $token)
    Require (!(Test-Path -LiteralPath $case)) 'Fixture root already exists'
    [void](New-Item -ItemType Directory -Path $case)
    $program = Join-Path $case 'program'
    [void](New-Item -ItemType Directory -Path $program)
    $binary = Join-Path $program 'StarDataParadiseServer.exe'
    Copy-Item -LiteralPath $source -Destination $binary
    $hash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    Require ((Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash -eq $hash) 'Fixture binary hash mismatch'
    Evidence "Fixture path $case and candidate SHA256 $hash"
    $tls = Join-Path $case 'tls'
    [void](New-Item -ItemType Directory -Path $tls)
    $acl = Get-Acl -LiteralPath $tls
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($sid in @($identity.User, [Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'), [Security.Principal.SecurityIdentifier]::new('S-1-5-18'))) {
        $rule = [Security.AccessControl.FileSystemAccessRule]::new($sid, 'FullControl', 'ContainerInherit, ObjectInherit', 'None', 'Allow')
        $acl.SetAccessRule($rule)
    }
    Set-Acl -LiteralPath $tls -AclObject $acl
    & $python -B (Join-Path $serverRoot 'tests\setup_lifecycle_fixture.py') certificates --openssl $openssl --tls $tls
    Require ($LASTEXITCODE -eq 0) 'Test TLS certificate generation failed'
    $bytes = [byte[]]::new(32)
    $random = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $random.GetBytes($bytes); $password = [Convert]::ToBase64String($bytes) }
    finally { [Array]::Clear($bytes,0,$bytes.Length); $random.Dispose() }
    $secret = ConvertTo-SecureString -String $password -AsPlainText -Force
    $failed = 0
    foreach ($test in @('Test-ServiceFixture', 'Test-FirewallFixture')) {
        try { & $test } catch { $failed++; Evidence ('FAIL ' + $test + ' ' + $_.Exception.Message) }
    }
    $code = [int]($failed -ne 0)
}
catch { Evidence ('FAIL ' + $_.Exception.Message) }
finally {
    if ($secret) { $secret.Dispose() }
    $password = $null
    Set-Content -LiteralPath $ExitPath -Encoding ASCII -Value $code
}
exit $code


