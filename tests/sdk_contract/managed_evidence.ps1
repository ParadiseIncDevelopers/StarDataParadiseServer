function Invoke-SdkEvidence {
    param(
        [string] $Directory,
        [string] $Name,
        [string] $Executable,
        [string[]] $Arguments
    )

    $log = Join-Path $Directory "$Name.log"
    $exitFile = Join-Path $Directory "$Name.exit"
    $command = ConvertTo-Json -Compress -InputObject (@($Executable) + $Arguments)
    Set-Content -LiteralPath $log -Value $command -Encoding utf8
    $ErrorActionPreference = 'Continue'
    $PSNativeCommandUseErrorActionPreference = $false
    & $Executable @Arguments *>> $log
    $code = $LASTEXITCODE
    Set-Content -LiteralPath $exitFile -Value $code -Encoding ascii
    Get-Content -LiteralPath $log -Tail 8
    if ($code -ne 0) { throw "$Name exited $code ($log)" }
}