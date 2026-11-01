# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$run = Join-Path $root ('.cache/shortcut-tests/' + [guid]::NewGuid().ToString('N'))
$fixture = Join-Path $run 'installation with spaces'
New-Item -ItemType Directory -Path $fixture -Force | Out-Null
$helper = Join-Path $root 'installer/Remove-OwnedShortcut.ps1'
$createHelper = Join-Path $root 'installer/Create-ShortcutIfMissing.ps1'
$target = Join-Path $fixture 'velin.exe'
$otherTarget = Join-Path $fixture 'other.exe'
$owned = Join-Path $fixture 'owned.lnk'
$replaced = Join-Path $fixture 'replaced.lnk'
$invalid = Join-Path $fixture 'invalid.lnk'
$missing = Join-Path $fixture 'missing.lnk'
$unrelated = Join-Path $fixture 'unrelated.txt'
$created = Join-Path $fixture 'created.lnk'

$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($owned)
$shortcut.TargetPath = $target
$shortcut.Save()
$shortcut = $shell.CreateShortcut($replaced)
$shortcut.TargetPath = $otherTarget
$shortcut.Save()
Set-Content -LiteralPath $invalid -Value 'This is not a shortcut.'
Set-Content -LiteralPath $unrelated -Value 'Preserve this file.'
$replacedHash = (Get-FileHash -LiteralPath $replaced).Hash
$invalidHash = (Get-FileHash -LiteralPath $invalid).Hash
$unrelatedHash = (Get-FileHash -LiteralPath $unrelated).Hash

& $createHelper -Path $created -Target $target
if (-not (Test-Path -LiteralPath $created -PathType Leaf) -or
    -not [string]::Equals($shell.CreateShortcut($created).TargetPath, $target, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'A missing shortcut was not created with the requested target.'
}
& $createHelper -Path $replaced -Target $target
& $createHelper -Path $invalid -Target $target
& $createHelper -Path $unrelated -Target $target
& $helper -Path $owned -Target $target
& $helper -Path $replaced -Target $target
& $helper -Path $invalid -Target $target
& $helper -Path $missing -Target $target

if (Test-Path -LiteralPath $owned) { throw 'The owned shortcut was not removed.' }
if (Test-Path -LiteralPath $missing) { throw 'A missing shortcut was created.' }
if (Get-ChildItem -LiteralPath $fixture -Force -Filter '.velin-shortcut-*.lnk') { throw 'Temporary shortcuts remain.' }
foreach ($check in @(
    @{ Path = $replaced; Hash = $replacedHash },
    @{ Path = $invalid; Hash = $invalidHash },
    @{ Path = $unrelated; Hash = $unrelatedHash }
)) {
    if (-not (Test-Path -LiteralPath $check.Path -PathType Leaf) -or
        (Get-FileHash -LiteralPath $check.Path).Hash -ne $check.Hash) {
        throw "An unrelated or unverified file was changed: $($check.Path)"
    }
}
Write-Output 'PASS isolated shortcut creation and removal; existing and unrelated files preserved'
