# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param([string]$SevenZipPath)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$manifestPath = Join-Path $projectRoot 'config\mpv-runtime.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$cachePath = Join-Path $projectRoot '.cache'
$runtimePath = Join-Path $projectRoot 'runtime'
$archivePath = Join-Path $cachePath $manifest.archive
$dllPath = Join-Path $runtimePath $manifest.dll

if ((Test-Path -LiteralPath $dllPath) -and
    (Get-FileHash -LiteralPath $dllPath -Algorithm SHA256).Hash -eq $manifest.dllSha256) {
    Write-Host "Verified runtime already available: $dllPath"
    exit 0
}

if (-not $SevenZipPath) {
    $sevenZipCommand = Get-Command 7z.exe -ErrorAction SilentlyContinue
    if ($sevenZipCommand) { $SevenZipPath = $sevenZipCommand.Source }
}
if (-not $SevenZipPath) {
    $candidates = @(
        (Join-Path $env:ProgramFiles '7-Zip\7z.exe'),
        (Join-Path $env:ProgramFiles 'EibolSoft\FFmpeg Batch AV Converter\7z.exe')
    )
    $SevenZipPath = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $SevenZipPath -or -not (Test-Path -LiteralPath $SevenZipPath)) {
    throw '7-Zip is required. Install 7-Zip or pass -SevenZipPath C:\path\to\7z.exe.'
}

New-Item -ItemType Directory -Force -Path $cachePath, $runtimePath | Out-Null
if (-not (Test-Path -LiteralPath $archivePath) -or
    (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash -ne $manifest.sha256) {
    $partialPath = "$archivePath.partial"
    Write-Host "Downloading pinned x64 libmpv runtime ($($manifest.release))..."
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $manifest.url -OutFile $partialPath -UseBasicParsing
    $downloadHash = (Get-FileHash -LiteralPath $partialPath -Algorithm SHA256).Hash
    if ($downloadHash -ne $manifest.sha256) {
        throw "Archive SHA256 mismatch. Expected $($manifest.sha256), got $downloadHash."
    }
    Move-Item -LiteralPath $partialPath -Destination $archivePath -Force
}

# Extract only the named runtime DLL from the hash-verified archive.
& $SevenZipPath x $archivePath "-o$runtimePath" $manifest.dll -y
if ($LASTEXITCODE -ne 0) { throw "7-Zip failed with exit code $LASTEXITCODE." }
if (-not (Test-Path -LiteralPath $dllPath) -or
    (Get-FileHash -LiteralPath $dllPath -Algorithm SHA256).Hash -ne $manifest.dllSha256) {
    throw 'Extracted runtime DLL failed SHA256 verification.'
}
Write-Host "Verified runtime available: $dllPath"
Write-Host 'Reconfigure and rebuild CMake to copy the runtime beside velin.exe.'
