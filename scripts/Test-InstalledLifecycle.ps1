# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Installer,
    [string]$PreviousInstaller,
    [switch]$DisposableEnvironment
)

$ErrorActionPreference = 'Stop'
if (-not $DisposableEnvironment) {
    throw 'Run only in a disposable Windows account or VM, and pass -DisposableEnvironment.'
}
foreach ($candidate in @($Installer, $PreviousInstaller) | Where-Object { $_ }) {
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) { throw "Installer not found: $candidate" }
}
$Installer = (Resolve-Path -LiteralPath $Installer).Path
if ($PreviousInstaller) { $PreviousInstaller = (Resolve-Path -LiteralPath $PreviousInstaller).Path }
else { $PreviousInstaller = $Installer }

# HKCU and shell paths are shared by all per-user installations. Refuse to
# alter a profile that already has Velin state or conflicting shortcut names.
$regPaths = @('HKCU:\Software\Velin', 'HKCU:\Software\Classes\Velin.Media',
    'HKCU:\Software\Classes\velin', 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Velin')
foreach ($path in $regPaths) {
    if (Test-Path -LiteralPath $path) { throw "Profile is not clean: $path" }
}
$registered = Get-ItemProperty -LiteralPath 'HKCU:\Software\RegisteredApplications' -Name Velin -ErrorAction SilentlyContinue
if ($null -ne $registered) { throw 'Profile already has a Velin registered application.' }
$programs = [Environment]::GetFolderPath('Programs')
$desktop = [Environment]::GetFolderPath('DesktopDirectory')
$shortcuts = @((Join-Path $programs 'Velin\Velin.lnk'), (Join-Path $programs 'Velin\Uninstall Velin.lnk'), (Join-Path $desktop 'Velin.lnk'))
foreach ($shortcut in $shortcuts) {
    if (Test-Path -LiteralPath $shortcut) { throw "Profile already has a conflicting shortcut: $shortcut" }
}
$userData = Join-Path $env:LOCALAPPDATA 'Velin'
if (Test-Path -LiteralPath $userData) { throw "Profile already has Velin user data: $userData" }
if (Get-Process -Name velin -ErrorAction SilentlyContinue) { throw 'Stop all Velin processes before the lifecycle test.' }

$run = Join-Path $env:TEMP ('velin-lifecycle-' + [guid]::NewGuid().ToString('N'))
# Use non-ASCII and spaces without requiring a particular PowerShell file encoding.
$first = Join-Path $run ('installation ' + [char]0x6E2C + [char]0x8A66 + ' A')
$second = Join-Path $run 'installation B'
New-Item -ItemType Directory -Path $run, $userData -Force | Out-Null
$dataMarker = Join-Path $userData 'lifecycle-user-data.txt'
Set-Content -LiteralPath $dataMarker -Value 'Preserve user data.'

function Invoke-Setup([string]$Setup, [string]$Destination) {
    # NSIS requires /D to be the last argument and its value must not be quoted.
    $process = Start-Process -FilePath $Setup -ArgumentList @('/S', "/D=$Destination") -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "Installer failed with exit code $($process.ExitCode)." }
    foreach ($name in @('velin.exe', 'velin-cli.exe', 'uninstall.exe')) {
        if (-not (Test-Path -LiteralPath (Join-Path $Destination $name) -PathType Leaf)) { throw "Missing installed file: $name" }
    }
}
function Invoke-Uninstall([string]$Destination) {
    $uninstaller = Join-Path $Destination 'uninstall.exe'
    # _?= prevents the NSIS uninstaller from spawning an untracked copy.
    $process = Start-Process -FilePath $uninstaller -ArgumentList @('/S', "_?=$Destination") -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "Uninstaller failed with exit code $($process.ExitCode)." }
    if (Test-Path -LiteralPath (Join-Path $Destination 'velin.exe')) { throw 'Installed executable remains after uninstall.' }
}
function Assert-Registration([string]$Destination) {
    $location = (Get-ItemProperty -LiteralPath 'HKCU:\Software\Velin' -Name InstallLocation).InstallLocation
    $uninstall = (Get-ItemProperty -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Velin' -Name InstallLocation).InstallLocation
    if ($location -ne $Destination -or $uninstall -ne $Destination) { throw 'Installation registration points to the wrong directory.' }
}
function Assert-ShortcutTarget([string]$Path, [string]$Target) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Shortcut is missing: $Path" }
    $actual = (New-Object -ComObject WScript.Shell).CreateShortcut($Path).TargetPath
    if (-not [string]::Equals($actual, $Target, [StringComparison]::OrdinalIgnoreCase)) { throw "Unexpected shortcut target: $Path" }
}

Write-Output "Lifecycle fixture: $run"
Invoke-Setup $PreviousInstaller $first
Assert-Registration $first
Assert-ShortcutTarget $shortcuts[0] (Join-Path $first 'velin.exe')
Assert-ShortcutTarget $shortcuts[1] (Join-Path $first 'uninstall.exe')
$firstVersion = (& (Join-Path $first 'velin-cli.exe') --version | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or -not $firstVersion) { throw 'Installed CLI failed to start.' }
Set-Content -LiteralPath (Join-Path $first 'unrelated.txt') -Value 'Preserve this file.'
Invoke-Setup $Installer $first
Assert-Registration $first
$currentVersion = (& (Join-Path $first 'velin-cli.exe') --version | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or -not $currentVersion) { throw 'Upgraded CLI failed to start.' }
if (-not (Test-Path -LiteralPath (Join-Path $first 'unrelated.txt')) -or -not (Test-Path -LiteralPath $dataMarker)) { throw 'Upgrade removed unrelated or user data.' }

Invoke-Setup $Installer $second
Assert-Registration $second
Invoke-Uninstall $first
Assert-Registration $second
Assert-ShortcutTarget $shortcuts[0] (Join-Path $first 'velin.exe')
Assert-ShortcutTarget $shortcuts[1] (Join-Path $first 'uninstall.exe')
if (-not (Test-Path -LiteralPath (Join-Path $second 'velin.exe'))) { throw 'Older uninstall removed the newer installation.' }
if (-not (Test-Path -LiteralPath (Join-Path $first 'unrelated.txt')) -or -not (Test-Path -LiteralPath $dataMarker)) { throw 'Uninstall removed unrelated or user data.' }
Invoke-Uninstall $second
if (Test-Path -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Velin') { throw 'Uninstall registration remains.' }
if ((Get-Content -LiteralPath $dataMarker -Raw).Trim() -ne 'Preserve user data.') { throw 'Uninstall changed user data.' }
Write-Output "PASS installed lifecycle: $firstVersion -> $currentVersion; user data and unrelated files preserved."
Write-Output 'Fixtures and any preserved shortcuts are intentionally left for inspection in the disposable profile.'
