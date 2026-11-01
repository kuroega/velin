# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build/mingw-release',
    [string]$FFmpeg = 'ffmpeg',
    [switch]$Headless
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory = Join-Path $projectRoot $BuildDirectory
}
$smoke = Join-Path $BuildDirectory 'velin_engine_smoke.exe'
if (-not (Test-Path -LiteralPath $smoke)) {
    $smoke = Join-Path $BuildDirectory 'Release\velin_engine_smoke.exe'
}
if (-not (Test-Path -LiteralPath $smoke)) {
    throw 'Build velin_engine_smoke with BUILD_TESTING=ON before running this script.'
}
if (-not (Get-Command $FFmpeg -ErrorAction SilentlyContinue)) {
    throw 'FFmpeg is required to generate test media. Pass -FFmpeg C:\path\to\ffmpeg.exe.'
}

$fixtures = Join-Path $projectRoot '.cache\playback-fixtures'
New-Item -ItemType Directory -Force -Path $fixtures | Out-Null
$metadata = Join-Path $fixtures 'chapters.ffmetadata'
@'
;FFMETADATA1
title=Velin playback test
[CHAPTER]
TIMEBASE=1/1000
START=0
END=2000
title=Opening
[CHAPTER]
TIMEBASE=1/1000
START=2000
END=4000
title=Second act
'@ | Set-Content -LiteralPath $metadata -Encoding ascii

# Exercise non-ASCII filesystem paths without depending on this script's encoding.
$unicodeName = 'Velin ' + [char]0x6E2C + [char]0x8A66 + ' ' + [char]0x2014 + ' chaptered.mkv'
$chaptered = Join-Path $fixtures $unicodeName
$second = Join-Path $fixtures 'second.mp4'
& $FFmpeg -nostdin -hide_banner -loglevel error -y `
    -f lavfi -i 'testsrc2=size=320x180:rate=24:duration=4' `
    -f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=4' `
    -f ffmetadata -i $metadata -map 0:v -map 1:a -map_metadata 2 -map_chapters 2 `
    -c:v mpeg4 -q:v 5 -c:a aac -shortest $chaptered
if ($LASTEXITCODE -ne 0) { throw 'FFmpeg failed to generate the chaptered Unicode fixture.' }

& $FFmpeg -nostdin -hide_banner -loglevel error -y `
    -f lavfi -i 'testsrc2=size=320x180:rate=24:duration=3' `
    -f lavfi -i 'sine=frequency=660:sample_rate=48000:duration=3' `
    -map 0:v -map 1:a -c:v mpeg4 -q:v 5 -c:a aac -shortest $second
if ($LASTEXITCODE -ne 0) { throw 'FFmpeg failed to generate the second media fixture.' }

@'
1
00:00:00,100 --> 00:00:03,500
Velin external subtitle test
'@ | Set-Content -LiteralPath (Join-Path $fixtures 'external.srt') -Encoding utf8

$arguments = @('--runtime', '--fixtures', $fixtures)
if ($Headless) { $arguments += '--headless' }
$originalDataDirectory = $env:VELIN_DATA_DIR
try {
    $env:VELIN_DATA_DIR = Join-Path $fixtures 'application-data'
    & $smoke @arguments
    if ($LASTEXITCODE -ne 0) { throw "Playback smoke validation failed with exit code $LASTEXITCODE." }
} finally {
    $env:VELIN_DATA_DIR = $originalDataDirectory
}
