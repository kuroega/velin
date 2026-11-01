# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$run = Join-Path $root ('.cache/update-tests/' + [guid]::NewGuid().ToString('N'))
$payload = Join-Path $run 'payload'
$target = Join-Path $run 'installation'
New-Item -ItemType Directory -Force -Path $payload, $target | Out-Null
foreach ($name in @('velin.exe','velin-cli.exe','libmpv-2.dll')) {
    Set-Content -LiteralPath (Join-Path $payload $name) -Value 'new payload'
    Set-Content -LiteralPath (Join-Path $target $name) -Value 'original payload'
}
$files = @(Get-ChildItem -LiteralPath $payload -File | ForEach-Object { @{name=$_.Name;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash} })
$manifest = Join-Path $payload 'release-file-manifest.json'
@{version='0.2.0';files=$files} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifest
Add-Type -AssemblyName System.IO.Compression.FileSystem
function Invoke-Fixture([string]$Name, [bool]$ShouldFail, [string]$PreservedPending = "", [string]$RestartArgumentsJson = '[]', [string]$ExpectedError = '') {
    $zip = Join-Path $run ($Name + '.zip')
    [IO.Compression.ZipFile]::CreateFromDirectory($payload,$zip)
    $failed = $false
    $failureMessage = ''
    try { & (Join-Path $PSScriptRoot 'Apply-Update.ps1') -Archive $zip -ExpectedSha256 (Get-FileHash -LiteralPath $zip).Hash -TargetDirectory $target -RestartArgumentsJson $RestartArgumentsJson }
    catch { $failed = $true; $failureMessage = $_.Exception.ToString() }
    if ($failed -ne $ShouldFail) { throw "Unexpected update result: $Name`n$failureMessage" }
    if ($ExpectedError -and -not $failureMessage.Contains($ExpectedError)) { throw "Unexpected update error: $Name`n$failureMessage" }
    if (Get-ChildItem -LiteralPath $run -Directory -Filter '.velin-update-*') { throw "Scratch directories remain: $Name" }
    if (Get-ChildItem -LiteralPath $target -File -Filter '*.velin-new' | Where-Object { $_.FullName -ne $PreservedPending }) { throw "Pending copies remain: $Name" }
}
Set-Content -LiteralPath $manifest -Value '{broken'
Invoke-Fixture 'corrupt-manifest' $true
@{version='0.2.0';files=$files} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifest
Invoke-Fixture 'invalid-restart-string' $true '' '"one"' 'RestartArgumentsJson must be an array of strings.'
Invoke-Fixture 'invalid-restart-null' $true '' 'null' 'RestartArgumentsJson must be an array of strings.'
Invoke-Fixture 'invalid-restart-element' $true '' '[42]' 'RestartArgumentsJson must be an array of strings.'
Invoke-Fixture 'invalid-restart-extra-property' $true '' '[],"extra":[]' 'RestartArgumentsJson must be an array of strings.'
# A preexisting pending filename forces failure after earlier files are replaced.
$collision = Join-Path $target 'velin.exe.velin-new'
Set-Content -LiteralPath $collision -Value 'existing user file'
Invoke-Fixture 'copy-failure' $true $collision
foreach ($name in @('velin.exe','velin-cli.exe','libmpv-2.dll')) {
    if ((Get-Content -LiteralPath (Join-Path $target $name) -Raw).Trim() -ne 'original payload') { throw "Rollback failed: $name" }
}
if ((Get-Content -LiteralPath $collision -Raw).Trim() -ne 'existing user file') { throw 'Existing pending file was modified' }
Remove-Item -LiteralPath $collision
Invoke-Fixture 'success' $false
Invoke-Fixture 'success-with-one-restart-argument' $false '' '["one"]'
Invoke-Fixture 'success-with-restart-arguments' $false '' '["Unicode caf\u00e9", "path with spaces"]'
foreach ($name in @('velin.exe','velin-cli.exe','libmpv-2.dll')) {
    if ((Get-Content -LiteralPath (Join-Path $target $name) -Raw).Trim() -ne 'new payload') { throw "Install failed: $name" }
}
Write-Output 'PASS isolated update, restart argument validation, corrupt manifest rejection, copy failure rollback and scratch cleanup'
