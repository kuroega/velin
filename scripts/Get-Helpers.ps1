# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param([string]$SevenZipPath)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Download-Verified.ps1')
$root = Split-Path -Parent $PSScriptRoot
$manifest = Get-Content -LiteralPath (Join-Path $root 'config\helpers-runtime.json') -Raw | ConvertFrom-Json
$runtime = Join-Path $root 'runtime'
New-Item -ItemType Directory -Force -Path $runtime | Out-Null
foreach ($helper in @($manifest.ffmpeg, $manifest.ytDlp)) {
    $target = Join-Path $runtime $helper.file
    if ((Test-Path -LiteralPath $target) -and (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -eq $helper.fileSha256) {
        Write-Host "Verified $($helper.file) already available."
        continue
    }
    $archive = Join-Path $root ('.cache\' + $helper.archive)
    Get-VerifiedDownload -Url $helper.url -Destination $archive -Sha256 $helper.sha256
    if ($helper.archive.EndsWith('.7z')) {
        $extractor = Find-VelinSevenZip $SevenZipPath
        & $extractor x $archive "-o$runtime" $helper.file -y
        if ($LASTEXITCODE -ne 0) { throw '7-Zip failed to extract the helper.' }
    } else { Copy-Item -LiteralPath $archive -Destination $target -Force }
    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $helper.fileSha256) { throw "Extracted helper hash mismatch: $target" }
    Write-Host "Verified $($helper.file) available."
}
