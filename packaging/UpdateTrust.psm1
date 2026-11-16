Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-UpdateLocalItem([string]$Path, [bool]$Directory) {
    $absolute = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
    if ($absolute -notmatch '\A[A-Za-z]:[\\/]' -or $absolute.IndexOfAny([char[]]'<>:"|?*', 2) -ge 0) {
        throw 'Use a local Windows update path.'
    }
    $item = Get-Item -LiteralPath $absolute -Force -ErrorAction Stop
    if ($item -isnot [IO.FileSystemInfo] -or $item.PSIsContainer -ne $Directory) {
        throw 'The update path does not identify the expected file or directory.'
    }
    $cursor = $item
    while ($null -ne $cursor) {
        if ($cursor.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw 'Update paths must not use symbolic links or junctions.'
        }
        if ($cursor -is [IO.FileInfo]) { $cursor = $cursor.Directory }
        else { $cursor = $cursor.Parent }
    }
    return $item
}

function Read-UpdateLocalBytes([string]$Path, [int]$Limit) {
    $item = Get-UpdateLocalItem $Path $false
    $stream = [IO.File]::Open($item.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        if ($stream.Length -le 0 -or $stream.Length -gt $Limit) { throw 'Invalid update manifest or signature length.' }
        $expected = [int]$stream.Length
        $reader = [IO.BinaryReader]::new($stream)
        try {
            $blob = $reader.ReadBytes($expected)
            if ($blob.Length -ne $expected) { throw 'The update manifest or signature was truncated.' }
            return ,$blob
        } finally { $reader.Dispose() }
    } finally { $stream.Dispose() }
}

function Get-UpdateDigest([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($Bytes)).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Test-UpdateCms([byte[]]$Manifest, [byte[]]$Signature, [string]$ExpectedSigner) {
    if ($ExpectedSigner -notmatch '\A[0-9a-f]{64}\z') {
        throw 'A separately provisioned SHA-256 signer certificate pin is required.'
    }
    if ($PSVersionTable.PSEdition -eq 'Desktop') { Add-Type -AssemblyName System.Security }
    else { Add-Type -AssemblyName System.Security.Cryptography.Pkcs }

    $content = [Security.Cryptography.Pkcs.ContentInfo]::new($Manifest)
    $signed = [Security.Cryptography.Pkcs.SignedCms]::new($content, $true)
    $decoded = [Security.Cryptography.Pkcs.SignedCms]::new()
    try {
        $decoded.Decode($Signature)
        $signed.Decode($Signature)
    }
    catch { throw 'The update manifest CMS signature could not be decoded.' }

    if ($decoded.ContentInfo.Content.Length -ne 0 -or
        !$signed.Detached -or $signed.ContentInfo.ContentType.Value -ne '1.2.840.113549.1.7.1' -or
        (Get-UpdateDigest $signed.ContentInfo.Content) -cne (Get-UpdateDigest $Manifest) -or
        $signed.SignerInfos.Count -ne 1) {
        throw 'The update manifest requires one detached CMS signer over the manifest bytes.'
    }
    $signer = $signed.SignerInfos[0]
    if ($signer.DigestAlgorithm.Value -notin @('2.16.840.1.101.3.4.2.1', '2.16.840.1.101.3.4.2.2', '2.16.840.1.101.3.4.2.3')) {
        throw 'The update manifest signer must use a SHA-2 digest.'
    }
    $certificate = $signer.Certificate
    if ($null -eq $certificate) { throw 'The update manifest CMS signer certificate is missing.' }
    $fingerprint = Get-UpdateDigest $certificate.RawData
    if ($fingerprint -ine $ExpectedSigner) { throw 'The update manifest signer certificate pin does not match.' }
    $now = [DateTime]::UtcNow
    if ($now -lt $certificate.NotBefore.ToUniversalTime() -or $now -gt $certificate.NotAfter.ToUniversalTime()) {
        throw 'The pinned update signer certificate is outside its validity period.'
    }
    # The independently provisioned certificate pin is the trust anchor for this bundle.
    try { $signed.CheckSignature($true) }
    catch { throw 'The update manifest CMS signature does not validate.' }
    return $fingerprint
}

function ConvertTo-TrustedUpdateManifest([byte[]]$Manifest, [byte[]]$Signature, [string]$TrustedSignerSha256, [bool]$AllowDevelopment) {
    $signer = $null
    $trust = 'unsigned-development'
    if ($null -ne $Signature) {
        $signer = Test-UpdateCms $Manifest $Signature $TrustedSignerSha256
        $trust = 'pinned-cms'
    } elseif ($TrustedSignerSha256) {
        throw 'The update manifest signature is required for the configured signer pin.'
    }

    try {
        $utf8 = [Text.UTF8Encoding]::new($false, $true)
        $json = $utf8.GetString($Manifest).TrimStart([char]0xfeff)
        if (!$json.TrimStart().StartsWith('{', [StringComparison]::Ordinal)) { throw 'Expected a JSON object' }
        $document = ConvertFrom-Json -InputObject $json -ErrorAction Stop
    } catch { throw 'The update manifest must contain a UTF-8 JSON object.' }
    if ($document -isnot [pscustomobject] -or !$document.PSObject.Properties['channel'] -or
        $document.channel -isnot [string] -or $document.channel -cnotin @('production', 'development')) {
        throw 'The update manifest channel must be production or development.'
    }
    if ($document.channel -ceq 'development' -and !$AllowDevelopment) {
        throw 'Development update bundles require explicit AllowDevelopment approval.'
    }
    if ($document.channel -ceq 'production' -and $trust -cne 'pinned-cms') {
        throw 'A production update manifest requires a pinned CMS signature.'
    }
    [pscustomobject]@{
        manifest = $document
        manifestSha256 = Get-UpdateDigest $Manifest
        trust = $trust
        signerSha256 = $signer
    }
}

function Read-SdpUpdateManifest {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$ManifestPath,
        [string]$SignaturePath,
        [ValidatePattern('\A[0-9a-fA-F]{64}\z')][string]$TrustedSignerSha256,
        [switch]$AllowDevelopment
    )
    $manifest = Read-UpdateLocalBytes $ManifestPath 1MB
    $signature = $null
    if ($SignaturePath) { $signature = Read-UpdateLocalBytes $SignaturePath 1MB }
    elseif (Get-Item -LiteralPath ($ManifestPath + '.p7s') -Force -ErrorAction SilentlyContinue) {
        $signature = Read-UpdateLocalBytes ($ManifestPath + '.p7s') 1MB
    }
    return ConvertTo-TrustedUpdateManifest $manifest $signature $TrustedSignerSha256 ([bool]$AllowDevelopment)
}

function Resolve-SdpUpdateArtifact {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$BundleRoot,
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][ValidatePattern('\A[0-9a-fA-F]{64}\z')][string]$Sha256,
        [ValidateRange(0, [long]::MaxValue)][long]$Bytes
    )
    if ($Name -notmatch '\A[a-z0-9][a-z0-9._-]{0,199}\z' -or $Name.EndsWith('.') -or
        $Name -match '\A(?:CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(?:\.|\z)') {
        throw 'Update artifact names must be single safe filenames inside the bundle.'
    }
    $root = Get-UpdateLocalItem $BundleRoot $true
    $artifact = Get-UpdateLocalItem (Join-Path $root.FullName $Name) $false
    $stream = [IO.File]::Open($artifact.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        if ($PSBoundParameters.ContainsKey('Bytes') -and $stream.Length -ne $Bytes) {
            throw "Update artifact '$Name' length does not match."
        }
        if ((Get-FileHash -InputStream $stream -Algorithm SHA256).Hash -ine $Sha256) {
            throw "Update artifact '$Name' SHA-256 does not match."
        }
    } finally { $stream.Dispose() }
    return $artifact
}

function Read-UpdateHttpsBytes($Client, [Uri]$Uri, [int]$Limit) {
    $deadline = [Threading.CancellationTokenSource]::new([TimeSpan]::FromSeconds(15))
    try {
        $response = $Client.GetAsync($Uri, [Net.Http.HttpCompletionOption]::ResponseHeadersRead, $deadline.Token).GetAwaiter().GetResult()
        try {
            if ($response.StatusCode -ne [Net.HttpStatusCode]::OK) {
                throw ('Update check expected HTTP 200 and received ' + [int]$response.StatusCode + '.')
            }
            $length = $response.Content.Headers.ContentLength
            if ($null -ne $length -and ($length -le 0 -or $length -gt $Limit)) {
                throw 'The remote update manifest or signature exceeds the byte limit.'
            }
            $stream = $response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
            $bytes = [IO.MemoryStream]::new()
            try {
                $buffer = [byte[]]::new(8192)
                while (($count = $stream.ReadAsync($buffer, 0, $buffer.Length, $deadline.Token).GetAwaiter().GetResult()) -gt 0) {
                    if ($bytes.Length + $count -gt $Limit) { throw 'The remote update manifest or signature exceeds the byte limit.' }
                    $bytes.Write($buffer, 0, $count)
                }
                if (!$bytes.Length) { throw 'The remote update manifest or signature is empty.' }
                return ,$bytes.ToArray()
            } finally {
                $bytes.Dispose()
                $stream.Dispose()
            }
        } finally { $response.Dispose() }
    } finally { $deadline.Dispose() }
}

function Get-SdpOnlineUpdateManifest {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$ManifestUri,
        [Parameter(Mandatory)][ValidatePattern('\A[0-9a-fA-F]{64}\z')][string]$TrustedSignerSha256
    )
    $uri = $null
    if ($ManifestUri.Contains('\') -or ![Uri]::TryCreate($ManifestUri, [UriKind]::Absolute, [ref]$uri) -or
        $uri.Scheme -cne 'https' -or !$uri.IsWellFormedOriginalString() -or !$uri.Host -or
        $uri.UserInfo -or $uri.Query -or $uri.Fragment -or $uri.AbsolutePath.EndsWith('/')) {
        throw 'Supply an absolute HTTPS manifest file URL without credentials, query or fragment.'
    }
    Add-Type -AssemblyName System.Net.Http
    $handler = [Net.Http.HttpClientHandler]::new()
    $handler.AllowAutoRedirect = $false
    $handler.UseCookies = $false
    $handler.UseDefaultCredentials = $false
    $handler.UseProxy = $false
    $client = [Net.Http.HttpClient]::new($handler)
    try {
        $manifest = Read-UpdateHttpsBytes $client $uri 1MB
        $signature = Read-UpdateHttpsBytes $client ([Uri]($uri.AbsoluteUri + '.p7s')) 1MB
        return ConvertTo-TrustedUpdateManifest $manifest $signature $TrustedSignerSha256 $false
    } finally { $client.Dispose() }
}

Export-ModuleMember -Function Read-SdpUpdateManifest, Resolve-SdpUpdateArtifact, Get-SdpOnlineUpdateManifest
