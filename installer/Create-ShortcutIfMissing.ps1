# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Path,
    [Parameter(Mandatory)][string]$Target
)

$temporary = Join-Path (Split-Path -Parent $Path) ('.velin-shortcut-' + [guid]::NewGuid().ToString('N') + '.lnk')
$reserved = $false
try {
    $reservation = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    $reserved = $true
    $reservation.Dispose()
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($temporary)
    $shortcut.TargetPath = $Target
    $shortcut.Save()
    # File.Move fails if the destination already exists, including on a race.
    [IO.File]::Move($temporary, $Path)
    $reserved = $false
} catch {
    # Existing names and errors are left untouched. The application remains usable.
} finally {
    if ($reserved) { Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue }
}
