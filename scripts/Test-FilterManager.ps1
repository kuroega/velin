# SPDX-License-Identifier: GPL-3.0-or-later
# Reuse the GUI smoke harness, but run only filter-manager checks.
[CmdletBinding()]
param([string]$Executable, [string]$MediaPath)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Executable) { $Executable = Join-Path $root 'build\mingw-release\velin.exe' }
if (-not $MediaPath) {
    $fixture = Get-ChildItem -LiteralPath (Join-Path $root '.cache\playback-fixtures') -Filter '*chaptered.mkv' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($fixture) { $MediaPath = $fixture.FullName }
}
if (-not $MediaPath) { throw 'Pass -MediaPath with a playable video fixture.' }
# Evaluate only the harness declarations before Test-Gui starts its own run.
$source = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'Test-Gui.ps1') -Raw
$marker = '$previousDpi = [VelinGuiSmoke]::SetThreadDpiAwarenessContext'
$offset = $source.IndexOf($marker, [StringComparison]::Ordinal)
if ($offset -lt 0) { throw 'Cannot locate the GUI harness boundary.' }
$prefix = $source.Substring(0, $offset)
$prefix = $prefix -replace '(?s)^.*?\$ErrorActionPreference = ''Stop''', '$ErrorActionPreference = ''Stop'''
$prefix = $prefix.Replace('$projectRoot = Split-Path -Parent $PSScriptRoot', '$projectRoot = $root')
Invoke-Expression $prefix

function Find-Tool([string]$Title) {
    $window = [VelinGuiSmoke]::FindClass($script:process.Id, 'Velin.NativeTool')
    if ($window -ne [IntPtr]::Zero -and [VelinGuiSmoke]::Title($window) -eq $Title) { return $window }
    return [IntPtr]::Zero
}
function Invoke-Manager([int]$Id) {
    [void][VelinGuiSmoke]::PostMessage($script:manager, 0x111, [IntPtr]$Id, [IntPtr]::Zero)
}
function Select-First([IntPtr]$List) {
    [void][VelinGuiSmoke]::SendMessage($script:manager, 0x28, $List, [IntPtr]1)
    [void][VelinGuiSmoke]::PostMessage($List, 0x201, [IntPtr]1, [IntPtr]0x00230020)
    [void][VelinGuiSmoke]::PostMessage($List, 0x202, [IntPtr]::Zero, [IntPtr]0x00230020)
    [void][VelinGuiSmoke]::PostMessage($List, 0x100, [IntPtr]0x24, [IntPtr]1)
    Start-Sleep -Milliseconds 150
}
function Rows([IntPtr]$List) {
    return [VelinGuiSmoke]::SendMessage($List, 0x1004, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32()
}
function Respond-Prompt([string]$Title, [string]$Text) {
    Wait-Gui { (Find-Tool $Title) -ne [IntPtr]::Zero } $Title
    $prompt = Find-Tool $Title
    [void][VelinGuiSmoke]::SetText([VelinGuiSmoke]::GetDlgItem($prompt,101),0x000c,[IntPtr]::Zero,$Text)
    [void][VelinGuiSmoke]::PostMessage($prompt,0x111,[IntPtr]1,[IntPtr]::Zero)
    Wait-Gui { (Find-Tool $Title) -eq [IntPtr]::Zero } "$Title closing"
}
function Open-Manager {
    $script:window = [VelinGuiSmoke]::FindPlayer($script:process.Id)
    # VideoFilters in PlayerWindow::Action.
    [void][VelinGuiSmoke]::PostMessage($script:window,0x111,[IntPtr]149,[IntPtr]::Zero)
    Wait-Gui { (Find-Tool 'Video filters') -ne [IntPtr]::Zero } 'filter manager'
    $script:manager = Find-Tool 'Video filters'
    $script:active = [VelinGuiSmoke]::GetDlgItem($script:manager,110)
    $script:presets = [VelinGuiSmoke]::GetDlgItem($script:manager,111)
}
try {
    $process = Start-Player $Executable $dataDirectory $MediaPath
    $window = [VelinGuiSmoke]::FindPlayer($process.Id)
    Wait-Gui { [VelinGuiSmoke]::Title($window) -ne 'Velin' } 'media playback'
    Open-Manager
    $initial = Rows $active
    Invoke-Manager 101
    Respond-Prompt 'Filter definition' '{"name":"hflip"}'
    Wait-Gui { (Rows $active) -eq $initial+1 } 'active filter'
    Select-First $active
    Invoke-Manager 104
    Respond-Prompt 'Save filter preset' 'Horizontal flip'
    $presetFile = Join-Path $dataDirectory 'filter-presets.json'
    Wait-Gui { Test-Path -LiteralPath $presetFile } 'saved filter preset'
    $saved = Get-Content -LiteralPath $presetFile -Raw | ConvertFrom-Json
    if (@($saved).Count -ne 1 -or $saved[0].name -ne 'Horizontal flip' -or $saved[0].filter.name -ne 'hflip') { throw 'Unexpected saved preset.' }
    Select-First $active
    Invoke-Manager 103
    Wait-Gui { (Rows $active) -eq $initial } 'filter removal'
    Select-First $presets
    Invoke-Manager 105
    Wait-Gui { (Rows $active) -eq $initial+1 } 'preset activation'
    Select-First $presets
    Invoke-Manager 105
    Wait-Gui { (Rows $active) -eq $initial } 'preset deactivation'
    [void][VelinGuiSmoke]::PostMessage($manager,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
    Wait-Gui { (Find-Tool 'Video filters') -eq [IntPtr]::Zero } 'filter manager closing'
    Stop-Player $process
    $process = Start-Player $Executable $dataDirectory $MediaPath
    $window = [VelinGuiSmoke]::FindPlayer($process.Id)
    Wait-Gui { [VelinGuiSmoke]::Title($window) -ne 'Velin' } 'media after restart'
    Open-Manager
    if ((Rows $presets) -ne 1) { throw 'Preset did not survive restart.' }
    Select-First $presets
    Invoke-Manager 105
    Wait-Gui { (Rows $active) -ge 1 } 'preset activation after restart'
    [void][VelinGuiSmoke]::PostMessage($manager,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
    Wait-Gui { (Find-Tool 'Video filters') -eq [IntPtr]::Zero } 'filter manager closing'
    Stop-Player $process
    Write-Output 'PASS filter add, remove, save, toggle, and restart persistence'
} finally {
    foreach ($owned in $ownedProcesses) {
        $owned.Refresh()
        if (-not $owned.HasExited) { $owned.Kill(); $owned.WaitForExit() }
    }
}
