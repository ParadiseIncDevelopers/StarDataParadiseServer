[CmdletBinding()]
param([string]$Scratch)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (!$Scratch) { $Scratch = Join-Path $PSScriptRoot '../out/sdp-m10/stage46/update-trust' }
Import-Module (Join-Path $PSScriptRoot 'UpdateTrust.psm1') -Force
if ($PSVersionTable.PSEdition -eq 'Desktop') { Add-Type -AssemblyName System.Security }
else { Add-Type -AssemblyName System.Security.Cryptography.Pkcs }

function Assert-Trust([bool]$Condition, [string]$Problem) {
    if (!$Condition) { throw $Problem }
}

function Assert-TrustRejection([string]$Name, [string]$Expected, [scriptblock]$Action) {
    try { $null = & $Action }
    catch {
        if ($_.Exception.Message -notmatch $Expected) { throw }
        Write-Output "PASS rejects $Name"
        return
    }
    throw "Accepted $Name."
}

function New-TestCms([byte[]]$Content, $Certificate, [bool]$Detached) {
    $info = [Security.Cryptography.Pkcs.ContentInfo]::new($Content)
    $cms = [Security.Cryptography.Pkcs.SignedCms]::new($info, $Detached)
    $signer = [Security.Cryptography.Pkcs.CmsSigner]::new($Certificate)
    $signer.IncludeOption = [Security.Cryptography.X509Certificates.X509IncludeOption]::EndCertOnly
    $signer.DigestAlgorithm = [Security.Cryptography.Oid]::new('2.16.840.1.101.3.4.2.1')
    $cms.ComputeSignature($signer, $true)
    return ,$cms.Encode()
}

$case = Join-Path $Scratch ([Guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $case -Force)
$case = (Resolve-Path -LiteralPath $case).ProviderPath
Write-Output "Trust fixture $case"
$encoding = [Text.UTF8Encoding]::new($false)
$manifest = Join-Path $case 'update.json'
$signature = $manifest + '.p7s'
$development = Join-Path $case 'development.json'
$artifact = Join-Path $case 'update.zip'
$artifactBytes = $encoding.GetBytes('Isolated test artifact for update signature verification.')
[IO.File]::WriteAllBytes($artifact, $artifactBytes)
$digest = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
$document = @{channel='production'; artifacts=@(@{name='update.zip'; bytes=$artifactBytes.Length; sha256=$digest})}
$manifestBytes = $encoding.GetBytes(($document | ConvertTo-Json -Depth 4 -Compress))
[IO.File]::WriteAllBytes($manifest, $manifestBytes)
[IO.File]::WriteAllText($development, '{"channel":"development"}', $encoding)

$key = [Security.Cryptography.RSACng]::new(2048)
$cert = $null
$expired = $null
try {
    $request = [Security.Cryptography.X509Certificates.CertificateRequest]::new(
        'CN=StarDataParadise update test fixture', $key, [Security.Cryptography.HashAlgorithmName]::SHA256,
        [Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $now = [DateTimeOffset]::UtcNow
    $cert = $request.CreateSelfSigned($now.AddMinutes(-5), $now.AddHours(4))
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $pin = [BitConverter]::ToString($sha.ComputeHash($cert.RawData)).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
    [IO.File]::WriteAllBytes($signature, (New-TestCms $manifestBytes $cert $true))
    $verified = Read-SdpUpdateManifest -ManifestPath $manifest -TrustedSignerSha256 $pin
    Assert-Trust ($verified.trust -ceq 'pinned-cms' -and $verified.signerSha256 -ceq $pin -and
        $verified.manifest.channel -ceq 'production' -and
        $verified.manifestSha256 -ieq (Get-FileHash -LiteralPath $manifest -Algorithm SHA256).Hash) 'Pinned production signature did not verify.'
    $file = Resolve-SdpUpdateArtifact -BundleRoot $case -Name 'update.zip' -Sha256 $digest -Bytes $artifactBytes.Length
    Assert-Trust ($file.FullName -ieq $artifact) 'Verified artifact returned a different path.'
    Write-Output 'PASS verifies detached CMS, independently pinned signer, manifest bytes and artifact bytes'

    Assert-TrustRejection 'wrong signer pin' 'pin does not match' {
        Read-SdpUpdateManifest -ManifestPath $manifest -TrustedSignerSha256 ('0' * 64)
    }
    Assert-TrustRejection 'missing signer pin' 'separately provisioned' {
        Read-SdpUpdateManifest -ManifestPath $manifest
    }
    $tampered = Join-Path $case 'tampered.json'
    [IO.File]::WriteAllText($tampered, '{"channel":"production","changed":true}', $encoding)
    Assert-TrustRejection 'tampered signed manifest' 'signature does not validate' {
        Read-SdpUpdateManifest -ManifestPath $tampered -SignaturePath $signature -TrustedSignerSha256 $pin -AllowDevelopment
    }
    Assert-TrustRejection 'unsigned production, even under development approval' 'requires a pinned CMS signature' {
        Read-SdpUpdateManifest -ManifestPath $tampered -AllowDevelopment
    }
    $otherSignature = Join-Path $case 'attached.p7s'
    [IO.File]::WriteAllBytes($otherSignature, (New-TestCms $manifestBytes $cert $false))
    Assert-TrustRejection 'attached CMS over matching bytes' 'detached CMS signer' {
        Read-SdpUpdateManifest -ManifestPath $manifest -SignaturePath $otherSignature -TrustedSignerSha256 $pin
    }
    Assert-TrustRejection 'attached CMS over a different payload' 'detached CMS signer' {
        Read-SdpUpdateManifest -ManifestPath $tampered -SignaturePath $otherSignature -TrustedSignerSha256 $pin
    }
    $badSignature = Join-Path $case 'bad.p7s'
    [IO.File]::WriteAllBytes($badSignature, [byte[]](1, 2, 3))
    Assert-TrustRejection 'malformed CMS while development approval is present' 'could not be decoded' {
        Read-SdpUpdateManifest -ManifestPath $development -SignaturePath $badSignature -TrustedSignerSha256 $pin -AllowDevelopment
    }
    Assert-TrustRejection 'missing development approval' 'explicit AllowDevelopment' {
        Read-SdpUpdateManifest -ManifestPath $development
    }
    $explicitDevelopment = Read-SdpUpdateManifest -ManifestPath $development -AllowDevelopment
    Assert-Trust ($explicitDevelopment.trust -ceq 'unsigned-development' -and
        $null -eq $explicitDevelopment.signerSha256) 'Unsigned development bundle acquired signed trust.'
    Write-Output 'PASS recognizes explicitly approved unsigned development separately'

    $expired = $request.CreateSelfSigned($now.AddDays(-2), $now.AddDays(-1))
    $expiredSignature = Join-Path $case 'expired.p7s'
    [IO.File]::WriteAllBytes($expiredSignature, (New-TestCms $manifestBytes $expired $true))
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $expiredPin = [BitConverter]::ToString($sha.ComputeHash($expired.RawData)).Replace('-', '') }
    finally { $sha.Dispose() }
    Assert-TrustRejection 'expired signer' 'outside its validity period' {
        Read-SdpUpdateManifest -ManifestPath $manifest -SignaturePath $expiredSignature -TrustedSignerSha256 $expiredPin
    }
    Assert-TrustRejection 'wrong artifact size' 'length does not match' {
        Resolve-SdpUpdateArtifact -BundleRoot $case -Name 'update.zip' -Sha256 $digest -Bytes 1
    }
    Assert-TrustRejection 'artifact outside the bundle' 'single safe filenames' {
        Resolve-SdpUpdateArtifact -BundleRoot $case -Name '..\update.zip' -Sha256 $digest
    }
    Assert-TrustRejection 'artifact alternate data stream' 'single safe filenames' {
        Resolve-SdpUpdateArtifact -BundleRoot $case -Name 'update.zip:payload' -Sha256 $digest
    }
    $artifactBytes[0] = 65
    [IO.File]::WriteAllBytes((Join-Path $case 'corrupt.zip'), $artifactBytes)
    Assert-TrustRejection 'tampered artifact of the original size' 'SHA-256 does not match' {
        Resolve-SdpUpdateArtifact -BundleRoot $case -Name 'corrupt.zip' -Sha256 $digest -Bytes $artifactBytes.Length
    }
    foreach ($uri in @('http://example.invalid/update.json', 'https://user@example.invalid/update.json',
            'https://example.invalid/update.json?token=value', 'https://example.invalid/update.json#other')) {
        Assert-TrustRejection "unsafe online endpoint $uri" 'absolute HTTPS manifest' {
            Get-SdpOnlineUpdateManifest -ManifestUri $uri -TrustedSignerSha256 $pin
        }
    }
}
finally {
    if ($expired) { $expired.Dispose() }
    if ($cert) { $cert.Dispose() }
    $key.Dispose()
}
