# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Download-Verified.ps1')
$root = Split-Path -Parent $PSScriptRoot
$manifest = (Get-Content -LiteralPath (Join-Path $root 'config\installer-tools.json') -Raw | ConvertFrom-Json).nsis
$archive = Join-Path $root ('.cache\' + $manifest.archive)
Get-VerifiedDownload -Url $manifest.url -Destination $archive -Sha256 $manifest.sha256
$tools = Join-Path $root '.cache\tools'
New-Item -ItemType Directory -Force -Path $tools | Out-Null
Expand-Archive -LiteralPath $archive -DestinationPath $tools -Force
$compiler = Join-Path $tools ('nsis-' + $manifest.version + '\makensis.exe')
if (-not (Test-Path -LiteralPath $compiler)) { throw 'The pinned NSIS archive is missing makensis.exe.' }
Write-Output $compiler
