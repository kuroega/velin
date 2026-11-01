# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Download-Verified.ps1')
$root = Split-Path -Parent $PSScriptRoot
$manifest = Get-Content -LiteralPath (Join-Path $root 'config\webview2-sdk.json') -Raw | ConvertFrom-Json
$expectedLoader = 'd3fce90bdeecb08596e377dea6a316354dd8949e32ff09b89d51be00c14e21b6'
$target = Join-Path $root 'runtime\WebView2Loader.dll'
if ((Test-Path -LiteralPath $target) -and (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -eq $expectedLoader) {
    Write-Host 'Verified WebView2Loader.dll already available.'
    exit 0
}
$archive = Join-Path $root ('.cache\webview2-' + $manifest.version + '.nupkg')
Get-VerifiedDownload -Url $manifest.url -Destination $archive -Sha256 $manifest.sha256
Add-Type -AssemblyName System.IO.Compression.FileSystem
$package = [IO.Compression.ZipFile]::OpenRead($archive)
try {
    $entry = $package.GetEntry($manifest.loader)
    if (-not $entry) { throw 'The SDK package does not contain the pinned x64 loader.' }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
} finally { $package.Dispose() }
if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $expectedLoader) { throw 'WebView2 loader hash mismatch.' }
Write-Host 'Verified WebView2Loader.dll available. Plugin web content also requires Microsoft Edge WebView2 Runtime.'
