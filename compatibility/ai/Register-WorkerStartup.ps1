[CmdletBinding()]
param([Parameter(Mandatory = $true)][string] $DeploymentDirectory)
$ErrorActionPreference = 'Stop'
$sdpDirectory = (Resolve-Path -LiteralPath $DeploymentDirectory).Path
$sdpLauncher = Join-Path $sdpDirectory 'StarDataParadiseAiWorker.exe'
if (!(Test-Path -LiteralPath $sdpLauncher -PathType Leaf)) { throw 'Installed worker launcher is missing.' }
$sdpStartup = [Environment]::GetFolderPath('Startup')
$sdpLink = Join-Path $sdpStartup 'StarDataParadise AI Worker.lnk'
$sdpArguments = '--deployment "' + $sdpDirectory + '"'
$sdpShell = New-Object -ComObject WScript.Shell
if (Test-Path -LiteralPath $sdpLink) {
    $sdpExisting = $sdpShell.CreateShortcut($sdpLink)
    if ($sdpExisting.TargetPath -ne $sdpLauncher -or $sdpExisting.Arguments -ne $sdpArguments) {
        throw 'A different startup entry already uses this name; it was preserved.'
    }
}
$sdpShortcut = $sdpShell.CreateShortcut($sdpLink)
$sdpShortcut.TargetPath = $sdpLauncher
$sdpShortcut.Arguments = $sdpArguments
$sdpShortcut.WorkingDirectory = $sdpDirectory
$sdpShortcut.WindowStyle = 7
$sdpShortcut.Description = 'StarDataParadise AI worker - current user, verified TLS and encrypted credentials'
$sdpShortcut.Save()
$sdpVerified = $sdpShell.CreateShortcut($sdpLink)
if ($sdpVerified.TargetPath -ne $sdpLauncher -or $sdpVerified.Arguments -ne $sdpArguments) {
    throw 'Startup shortcut verification failed.'
}
Write-Output "Verified per-user startup entry: $sdpLink"
