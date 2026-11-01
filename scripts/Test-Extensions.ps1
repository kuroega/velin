# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param([string]$BuildDirectory = 'build/mingw-release', [string]$Python = 'python')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = if ([IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory } else { Join-Path $projectRoot $BuildDirectory }
$testBinary = Join-Path $buildRoot 'velin_extensions_tests.exe'
if (!(Test-Path -LiteralPath $testBinary)) { $testBinary = Join-Path $buildRoot 'Release/velin_extensions_tests.exe' }
if (!(Test-Path -LiteralPath $testBinary)) { throw "Build velin_extensions_tests first: $testBinary" }
$testCache = Join-Path $projectRoot '.cache/extension-network-tests'
New-Item -ItemType Directory -Path $testCache -Force | Out-Null
$portFile = Join-Path $testCache 'port.txt'
if (Test-Path -LiteralPath $portFile) { Remove-Item -LiteralPath $portFile }
$server = $null
$previousData = $env:VELIN_DATA_DIR
try {
    $env:VELIN_DATA_DIR = Join-Path $testCache 'runtime-data'
    $pythonScript = Join-Path $projectRoot 'tests/extension_mock_server.py'
    $server = Start-Process -FilePath (Get-Command $Python).Source -ArgumentList @(('"' + $pythonScript + '"'), ('"' + $portFile + '"')) -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $testCache 'server.out.log') -RedirectStandardError (Join-Path $testCache 'server.error.log')
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while (!(Test-Path -LiteralPath $portFile) -and [DateTime]::UtcNow -lt $deadline -and !$server.HasExited) { Start-Sleep -Milliseconds 50 }
    if (!(Test-Path -LiteralPath $portFile)) { throw 'Local-only HTTP contract fixture server did not start' }
    $port = (Get-Content -LiteralPath $portFile -Raw).Trim()
    Push-Location -LiteralPath $buildRoot
    try { & $testBinary "http://127.0.0.1:$port"; if ($LASTEXITCODE -ne 0) { throw "Extension integration tests failed: $LASTEXITCODE" } }
    finally { Pop-Location }
}
finally {
    if ($server -and !$server.HasExited) { Stop-Process -Id $server.Id; $server.WaitForExit(5000) | Out-Null }
    $env:VELIN_DATA_DIR = $previousData
}
