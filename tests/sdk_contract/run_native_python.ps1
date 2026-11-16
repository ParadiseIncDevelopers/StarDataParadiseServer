param(
    [Parameter(Mandatory)][string]$Server,
    [Parameter(Mandatory)][string]$OpenSsl,
    [Parameter(Mandatory)][string]$Contract,
    [Parameter(Mandatory)][string]$ExpiryHelper,
    [Parameter(Mandatory)][string]$Native,
    [Parameter(Mandatory)][string]$FrozenNative,
    [Parameter(Mandatory)][string]$FoundationNative,
    [Parameter(Mandatory)][string]$Wheel,
    [Parameter(Mandatory)][string]$FrozenWheel,
    [Parameter(Mandatory)][string]$FoundationWheel,
    [Parameter(Mandatory)][string]$Scratch
)

$ErrorActionPreference = 'Stop'
function Confirm-Package([string]$Path, [string]$Expected) {
    $package = (Resolve-Path -LiteralPath $Path).Path
    if ((Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash -ne $Expected) {
        throw "Unexpected checksum for $([IO.Path]::GetFileName($package))"
    }
    return $package
}

$Native = (Resolve-Path -LiteralPath $Native).Path
$FrozenNative = Confirm-Package $FrozenNative '4ee15b8fa50c94c5ee012245d8ed8e3c16259df64fc154ca15fb286cb6a8ad25'
$FoundationNative = Confirm-Package $FoundationNative '9a9e7409c3670e38c1a54bca1bcdc2b9cf9e3f68c87286500c745bdbea1cfd4c'
$Wheel = (Resolve-Path -LiteralPath $Wheel).Path
$FrozenWheel = Confirm-Package $FrozenWheel 'b48ec6e5ed8e52e5fa2b04186f9c7229628dbffe3d6a5693f48c5a154e160940'
$FoundationWheel = Confirm-Package $FoundationWheel '4e9607dc1f9cf5ff76d2fba77a08110e4b4550f41cc71ca5b7c5f20188b7e62a'
$root = New-Item -ItemType Directory -Path $Scratch -Force
$root = New-Item -ItemType Directory -Path (Join-Path $root.FullName ('run-' + [Guid]::NewGuid().ToString('N').Substring(0,8)))
Write-Host "Evidence $($root.FullName)"
$fixture = @((Join-Path $PSScriptRoot '../cross_sdk_fixture.py'),
    '--server', (Resolve-Path -LiteralPath $Server).Path,
    '--openssl', (Resolve-Path -LiteralPath $OpenSsl).Path,
    '--contract', (Resolve-Path -LiteralPath $Contract).Path,
    '--scratch', (Join-Path $root.FullName 'f'))
$faults = @('--require-observed', '--expiry-helper', (Resolve-Path -LiteralPath $ExpiryHelper).Path,
    '--drop-op', 'tx_commit', '--drop-read')

function Invoke-Fixture([string]$Name, [string[]]$Consumer, [switch]$FullContract) {
    $options = @()
    if ($FullContract) { $options = $faults }
    $log = Join-Path $root.FullName "$Name.log"
    & python -B @fixture @options -- @Consumer *> $log
    $status = $LASTEXITCODE
    Set-Content -LiteralPath (Join-Path $root.FullName "$Name.exit") -Value $status
    Write-Host "$Name exit $status"
    Get-Content -LiteralPath $log | Write-Host
    return $status
}

function Install-Wheel([string]$Name, [string]$Package) {
    $directory = Join-Path $root.FullName $Name
    $log = "$directory-install.log"
    python -m venv $directory *> $log
    $status = $LASTEXITCODE
    $interpreter = Join-Path $directory 'Scripts/python.exe'
    if ($status -eq 0) {
        & $interpreter -m pip --disable-pip-version-check install --no-index --no-deps $Package *>> $log
        $status = $LASTEXITCODE
    }
    Set-Content -LiteralPath "$directory-install.exit" -Value $status
    if ($status -ne 0) { throw "Package installation failed. See $log" }
    return $interpreter
}

$failed = 0
foreach ($test in @(@('native', $Native, '0.4.1'), @('native-frozen', $FrozenNative, '0.4.0'),
                    @('native-foundation', $FoundationNative, '0.1.0'))) {
    $name, $package, $version = $test
    $runtime = Join-Path $root.FullName $name
    Expand-Archive -LiteralPath $package -DestinationPath $runtime
    $dll = Join-Path $runtime 'bin/StarDataParadiseDLLler.dll'
    $consumer = @('python','-I','-B',(Join-Path $PSScriptRoot 'native.py'),$dll,$version)
    if ((Invoke-Fixture $name $consumer -FullContract:($version -ne '0.1.0')) -ne 0) { $failed = 1 }
}
foreach ($test in @(@('python', $Wheel), @('python-frozen', $FrozenWheel))) {
    $name, $package = $test
    $interpreter = Install-Wheel $name $package
    $consumer = @($interpreter,'-I','-B',(Join-Path $PSScriptRoot 'python.py'))
    if ((Invoke-Fixture $name $consumer -FullContract) -ne 0) { $failed = 1 }
}

$interpreter = Install-Wheel 'python-foundation' $FoundationWheel
$log = Join-Path $root.FullName 'python-foundation.log'
& $interpreter -I -B -c "import stardataparadise_sdk as sdk; assert sdk.__version__ == '0.1.0' and not hasattr(sdk, 'connect'); print('Python 0.1.0 installed; this package only exposes version metadata')" *> $log
$status = $LASTEXITCODE
Set-Content -LiteralPath (Join-Path $root.FullName 'python-foundation.exit') -Value $status
Write-Host "python-foundation exit $status"
Get-Content -LiteralPath $log | Write-Host
if ($status -ne 0) { $failed = 1 }
exit $failed
