[CmdletBinding()]
param([Parameter(Mandatory = $true)][string] $Output)
$ErrorActionPreference = 'Stop'
$sdpCompiler = Join-Path $env:SystemRoot 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
$sdpSource = Join-Path $PSScriptRoot 'WorkerLauncher.cs'
$sdpOutput = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Path (Split-Path -Parent $sdpOutput) -Force | Out-Null
& $sdpCompiler /nologo /target:winexe /optimize+ /reference:System.Web.Extensions.dll "/out:$sdpOutput" $sdpSource
if ($LASTEXITCODE -ne 0) { throw 'AI worker launcher compilation failed.' }
Get-FileHash -LiteralPath $sdpOutput -Algorithm SHA256
