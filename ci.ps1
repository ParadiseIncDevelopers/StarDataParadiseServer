param([string]$OpenSslRoot = $env:OPENSSL_ROOT_DIR, [string]$JsonArchive, [string]$AsioArchive)

$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    $configure = @('--preset', 'windows-x64')
    if ($OpenSslRoot) { $configure += "-DOPENSSL_ROOT_DIR=$OpenSslRoot" }
    if ($JsonArchive) { $configure += "-DSDP_JSON_ARCHIVE=$JsonArchive" }
    if ($AsioArchive) { $configure += "-DSDP_ASIO_ARCHIVE=$AsioArchive" }
    & cmake @configure
    if ($LASTEXITCODE) { exit $LASTEXITCODE }

    & cmake --build --preset windows-x64
    if ($LASTEXITCODE) { exit $LASTEXITCODE }

    & ctest --preset windows-x64
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
