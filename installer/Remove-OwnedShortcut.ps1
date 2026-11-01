# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Path,
    [Parameter(Mandatory)][string]$Target
)

# A shortcut may have been replaced since installation. Leave it in place if
# its target cannot be read or does not match this installation's executable.
try {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { exit 0 }
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { exit 0 }
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($Path)
    if (-not $shortcut.TargetPath) { exit 0 }
    $actual = [IO.Path]::GetFullPath($shortcut.TargetPath)
    $expected = [IO.Path]::GetFullPath($Target)
    if ([string]::Equals($actual, $expected, [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $Path -Force
    }
} catch {
    # Failure to determine ownership must not delete a user's shortcut.
}
