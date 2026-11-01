# SPDX-License-Identifier: GPL-3.0-or-later
# Exercises only windows belonging to processes started by this script.
[CmdletBinding()]
param(
    [string]$Executable,
    [string]$MediaPath,
    [string]$OutputDirectory,
    [int]$ExpectedVideoWidth = 0,
    [int]$ExpectedVideoHeight = 0
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $Executable) { $Executable = Join-Path $projectRoot 'build\mingw-release\velin.exe' }
$Executable = [IO.Path]::GetFullPath($Executable)
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) { throw "Player executable not found: $Executable" }
if (-not [Environment]::Is64BitProcess) { throw 'Run this script in 64-bit PowerShell.' }
if (-not $MediaPath) {
    $fixture = Get-ChildItem -LiteralPath (Join-Path $projectRoot '.cache\fixtures') -Filter '*chaptered.mkv' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($fixture) { $MediaPath = $fixture.FullName }
}
if (-not $MediaPath -or -not (Test-Path -LiteralPath $MediaPath -PathType Leaf)) {
    throw 'Pass -MediaPath with a playable video fixture. A Unicode path with chapters is recommended.'
}
$MediaPath = [IO.Path]::GetFullPath($MediaPath)
$runName = 'run-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $projectRoot ".cache\gui-smoke\$runName" }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$dataDirectory = Join-Path $projectRoot ".cache\gui-data\$runName"
New-Item -ItemType Directory -Force -Path $OutputDirectory, $dataDirectory | Out-Null

if (-not ('VelinGuiSmoke' -as [type])) {
    Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class VelinGuiSmoke {
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }
    private delegate bool EnumProc(IntPtr window, IntPtr data);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumProc callback, IntPtr data);
    [DllImport("user32.dll")] private static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr data);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassName(IntPtr window, StringBuilder name, int count);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rect bounds);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr window);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr window, int id);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageW")] public static extern IntPtr SetText(IntPtr window, uint message, IntPtr wParam, string text);
    [DllImport("user32.dll", EntryPoint = "SendMessageW")] public static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("kernel32.dll")] private static extern IntPtr OpenProcess(uint access, bool inherit, uint processId);
    [DllImport("kernel32.dll")] private static extern IntPtr VirtualAllocEx(IntPtr process, IntPtr address, UIntPtr size, uint type, uint protect);
    [DllImport("kernel32.dll")] private static extern bool VirtualFreeEx(IntPtr process, IntPtr address, UIntPtr size, uint type);
    [DllImport("kernel32.dll")] private static extern bool WriteProcessMemory(IntPtr process, IntPtr address, byte[] buffer, int size, out IntPtr written);
    [DllImport("kernel32.dll")] private static extern bool ReadProcessMemory(IntPtr process, IntPtr address, byte[] buffer, int size, out IntPtr read);
    [DllImport("kernel32.dll")] private static extern bool CloseHandle(IntPtr handle);
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct ListItem {
        public uint mask; public int item, subItem; public uint state, stateMask;
        public IntPtr text; public int textMax, image; public IntPtr param;
        public int indent, groupId; public uint columns; public IntPtr columnPointer, columnFormat;
        public int group;
    }
    [DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtr(IntPtr window, int index);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr window, IntPtr update, IntPtr region, uint flags);
    [DllImport("user32.dll")] private static extern IntPtr SendMessageTimeout(IntPtr window, uint message, IntPtr wParam, IntPtr lParam, uint flags, uint timeout, out UIntPtr result);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("dwmapi.dll")] public static extern int DwmFlush();

    public static string ClassName(IntPtr window) {
        var text = new StringBuilder(256); GetClassName(window, text, text.Capacity); return text.ToString();
    }
    public static string Title(IntPtr window) {
        var text = new StringBuilder(32768); GetWindowText(window, text, text.Capacity); return text.ToString();
    }
    public static IntPtr FindPlayer(int processId) { return FindClass(processId,"Velin.Player"); }
    public static IntPtr FindClass(int processId, string className) {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate(IntPtr window, IntPtr data) {
            uint owner; GetWindowThreadProcessId(window, out owner);
            if (owner == processId && ClassName(window) == className) { found = window; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static IntPtr[] Children(IntPtr parent) {
        var result = new List<IntPtr>();
        EnumChildWindows(parent, delegate(IntPtr window, IntPtr data) { result.Add(window); return true; }, IntPtr.Zero);
        return result.ToArray();
    }
    public static bool Responsive(IntPtr window) {
        UIntPtr result;
        return SendMessageTimeout(window, 0, IntPtr.Zero, IntPtr.Zero, 2, 1000, out result) != IntPtr.Zero;
    }
    public static string ListText(IntPtr list, int row, int column) {
        uint pid; GetWindowThreadProcessId(list, out pid);
        IntPtr process = OpenProcess(0x438, false, pid);
        if (process == IntPtr.Zero) throw new InvalidOperationException("Cannot open player for list inspection");
        IntPtr remote = VirtualAllocEx(process, IntPtr.Zero, (UIntPtr)8192, 0x3000, 4);
        if (remote == IntPtr.Zero) { CloseHandle(process); throw new InvalidOperationException("Cannot allocate list inspection buffer"); }
        try {
            var item = new ListItem { subItem = column, text = IntPtr.Add(remote, 256), textMax = 2048 };
            int size = Marshal.SizeOf(typeof(ListItem)); IntPtr local = Marshal.AllocHGlobal(size);
            try {
                Marshal.StructureToPtr(item, local, false); var raw = new byte[size]; Marshal.Copy(local, raw, 0, size);
                IntPtr written; if (!WriteProcessMemory(process, remote, raw, size, out written)) throw new InvalidOperationException("Cannot write list request");
                SendMessage(list, 0x1073, (IntPtr)row, remote);
                var text = new byte[4096]; IntPtr read;
                if (!ReadProcessMemory(process, IntPtr.Add(remote, 256), text, text.Length, out read)) throw new InvalidOperationException("Cannot read list response");
                return Encoding.Unicode.GetString(text).Split('\0')[0];
            } finally { Marshal.FreeHGlobal(local); }
        } finally { VirtualFreeEx(process, remote, UIntPtr.Zero, 0x8000); CloseHandle(process); }
    }
}
'@
}
Add-Type -AssemblyName System.Drawing

$ownedProcesses = [Collections.Generic.List[Diagnostics.Process]]::new()
$checks = [Collections.Generic.List[string]]::new()
$captures = [ordered]@{}

function Assert-Gui([bool]$Condition, [string]$Description) {
    if (-not $Condition) { throw "GUI smoke assertion failed: $Description" }
    $checks.Add($Description)
}

function Wait-Gui([scriptblock]$Condition, [string]$Description, [int]$TimeoutMilliseconds = 10000) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMilliseconds)
    do {
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 80
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Timed out waiting for $Description"
}

function Select-GuiListRow([IntPtr]$List, [int]$Row) {
    if ($Row -lt 0) { throw 'A list row must be selected before removal.' }
    [void][VelinGuiSmoke]::SendMessage($List,0x0007,[IntPtr]::Zero,[IntPtr]::Zero)
    [void][VelinGuiSmoke]::SendMessage($List,0x0100,[IntPtr]0x24,[IntPtr]1)
    for ($index=0; $index -lt $Row; $index++) { [void][VelinGuiSmoke]::SendMessage($List,0x0100,[IntPtr]0x28,[IntPtr]1) }
    $selected=[VelinGuiSmoke]::SendMessage($List,0x100c,[IntPtr](-1),[IntPtr]2).ToInt32()
    if ($selected -ne $Row) { throw "Could not select list row $Row (selected $selected)." }
}

function Start-Player([string]$Path, [string]$Data, [string]$Media = '') {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Path
    $start.WorkingDirectory = Split-Path -Parent $Path
    $start.UseShellExecute = $false
    $start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Normal
    $start.EnvironmentVariables['VELIN_DATA_DIR'] = $Data
    if ($Media) { $start.Arguments = '"' + $Media + '"' }
    $process = [Diagnostics.Process]::Start($start)
    $ownedProcesses.Add($process)
    Wait-Gui {
        $process.Refresh()
        if ($process.HasExited) { throw "Player exited during startup with code $($process.ExitCode)." }
        return [VelinGuiSmoke]::FindPlayer($process.Id) -ne [IntPtr]::Zero
    } 'the player window'
    return $process
}

function Stop-Player([Diagnostics.Process]$Process) {
    $Process.Refresh()
    if (-not $Process.HasExited) {
        $window = [VelinGuiSmoke]::FindPlayer($Process.Id)
        if ($window -ne [IntPtr]::Zero) { [void][VelinGuiSmoke]::PostMessage($window, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) }
        if (-not $Process.WaitForExit(8000)) { throw 'Player did not close within eight seconds.' }
    }
    Assert-Gui ($Process.ExitCode -eq 0) 'Player closes with exit code zero'
}

function Send-Key([IntPtr]$Window, [int]$Key) {
    Assert-Gui ([VelinGuiSmoke]::PostMessage($Window, 0x100, [IntPtr]$Key, [IntPtr]1)) "Key message $Key is accepted"
}

function Window-Width([IntPtr]$Window) {
    $bounds = [VelinGuiSmoke+Rect]::new()
    if (-not [VelinGuiSmoke]::GetWindowRect($Window, [ref]$bounds)) { throw 'Cannot read window bounds.' }
    return $bounds.Right - $bounds.Left
}

function Save-Capture([IntPtr]$Window, [string]$Name) {
    $path = Join-Path $OutputDirectory "$Name.png"
    # The mpv VO thread completes child-window resizing asynchronously.
    Start-Sleep -Milliseconds 200
    # Bounds and PrintWindow share the same physical-pixel DPI context.
    $previous = [VelinGuiSmoke]::SetThreadDpiAwarenessContext([IntPtr](-4))
    try {
        $bounds = [VelinGuiSmoke+Rect]::new()
        if (-not [VelinGuiSmoke]::GetWindowRect($Window, [ref]$bounds)) { throw 'Cannot obtain player bounds.' }
        $width = $bounds.Right - $bounds.Left
        $height = $bounds.Bottom - $bounds.Top
        if ($width -le 0 -or $height -le 0 -or $width -gt 16384 -or $height -gt 16384) { throw 'Invalid capture dimensions.' }
        [void][VelinGuiSmoke]::RedrawWindow($Window, [IntPtr]::Zero, [IntPtr]::Zero, 0x185)
        [void][VelinGuiSmoke]::DwmFlush()
        $bitmap = [Drawing.Bitmap]::new($width, $height)
        try {
            $graphics = [Drawing.Graphics]::FromImage($bitmap)
            try {
                $dc = $graphics.GetHdc()
                try {
                    if (-not [VelinGuiSmoke]::PrintWindow($Window, $dc, 2)) { throw 'PrintWindow failed.' }
                } finally { $graphics.ReleaseHdc($dc) }
            } finally { $graphics.Dispose() }
            $bitmap.Save($path, [Drawing.Imaging.ImageFormat]::Png)
        } finally { $bitmap.Dispose() }
    } finally { if ($previous -ne [IntPtr]::Zero) { [void][VelinGuiSmoke]::SetThreadDpiAwarenessContext($previous) } }
    Assert-Gui ((Get-Item -LiteralPath $path).Length -gt 2000) "$Name screenshot contains rendered content"
    $captures[$Name] = $path
}

$previousDpi = [VelinGuiSmoke]::SetThreadDpiAwarenessContext([IntPtr](-4))
try {
    $welcome = Start-Player $Executable $dataDirectory
    $window = [VelinGuiSmoke]::FindPlayer($welcome.Id)
    Start-Sleep -Milliseconds 300
    Assert-Gui ([VelinGuiSmoke]::Title($window) -eq 'Velin') 'Welcome window has the application title'
    Assert-Gui ([VelinGuiSmoke]::Responsive($window)) 'Welcome window responds to WM_NULL'
    Save-Capture $window 'welcome'
    Stop-Player $welcome

    $playback = Start-Player $Executable $dataDirectory $MediaPath
    $window = [VelinGuiSmoke]::FindPlayer($playback.Id)
    Wait-Gui { [VelinGuiSmoke]::Title($window) -ne 'Velin' } 'the media title'
    Send-Key $window ([int][char]'R')
    Wait-Gui {
        $script:videoHost = @([VelinGuiSmoke]::Children($window) | Where-Object { [VelinGuiSmoke]::ClassName($_) -eq 'Velin.Video' } | Select-Object -First 1)
        return $script:videoHost.Count -eq 1 -and [VelinGuiSmoke]::IsWindowVisible($script:videoHost[0]) -and @([VelinGuiSmoke]::Children($script:videoHost[0])).Count -gt 0
    } 'the embedded mpv video window'
    $videoHost = $script:videoHost[0]
    $videoChildren = @([VelinGuiSmoke]::Children($videoHost))
    Assert-Gui ([VelinGuiSmoke]::IsWindowVisible($videoHost)) 'Video host becomes visible for loaded media'
    Assert-Gui (@($videoChildren | Where-Object { -not [VelinGuiSmoke]::IsWindowEnabled($_) }).Count -gt 0) 'Embedded mpv window is disabled so mouse input reaches its host'
    Assert-Gui ([VelinGuiSmoke]::Responsive($window)) 'Playback window remains responsive'
    Start-Sleep -Milliseconds 300
    Save-Capture $window 'playback'

    [void][VelinGuiSmoke]::PostMessage($videoHost, 0x201, [IntPtr]1, [IntPtr]0x00640064)
    [void][VelinGuiSmoke]::PostMessage($videoHost, 0x202, [IntPtr]::Zero, [IntPtr]0x00640064)
    Start-Sleep -Milliseconds 200
    Save-Capture $window 'paused'
    [void][VelinGuiSmoke]::PostMessage($videoHost, 0x201, [IntPtr]1, [IntPtr]0x00640064)
    [void][VelinGuiSmoke]::PostMessage($videoHost, 0x202, [IntPtr]::Zero, [IntPtr]0x00640064)

    $originalVideoWidth = Window-Width $videoHost
    Send-Key $window ([int][char]'P')
    Wait-Gui { (Window-Width $videoHost) -gt $originalVideoWidth + 100 } 'sidebar hiding to enlarge the video host'
    $checks.Add('Playlist keyboard shortcut resizes the video host')
    Save-Capture $window 'sidebar-hidden'
    Send-Key $window ([int][char]'P')
    Wait-Gui { [Math]::Abs((Window-Width $videoHost) - $originalVideoWidth) -lt 3 } 'sidebar restoration'

    if ($ExpectedVideoWidth -gt 0 -and $ExpectedVideoHeight -gt 0) {
        # These dimensions are the display dimensions of the supplied uncropped fixture.
        [void][VelinGuiSmoke]::SendMessage($window,0x111,[IntPtr]12002,[IntPtr]::Zero)
        $videoBounds=[VelinGuiSmoke+Rect]::new()
        [void][VelinGuiSmoke]::GetWindowRect($videoHost,[ref]$videoBounds)
        $videoWidth=$videoBounds.Right-$videoBounds.Left
        $videoHeight=$videoBounds.Bottom-$videoBounds.Top
        Assert-Gui ([Math]::Abs($videoWidth / $videoHeight - $ExpectedVideoWidth / $ExpectedVideoHeight) -lt 0.02) 'Double-size preset preserves the video aspect ratio'
        Assert-Gui ($videoWidth -ge 2 * $ExpectedVideoWidth - 2 -and $videoHeight -ge 2 * $ExpectedVideoHeight - 2) 'Double-size preset uses at least twice the video dimensions'
        [void][VelinGuiSmoke]::SendMessage($window,0x111,[IntPtr]12010,[IntPtr]::Zero)
        $preferencesPath=Join-Path $dataDirectory 'preferences.ini'
        Assert-Gui ((Get-Content -LiteralPath $preferencesPath -Raw) -match '(?m)^lockWindowAspect=1\r?$') 'Aspect lock is saved after the menu action'
        [void][VelinGuiSmoke]::SendMessage($window,0x111,[IntPtr]12010,[IntPtr]::Zero)
        Assert-Gui ((Get-Content -LiteralPath $preferencesPath -Raw) -match '(?m)^lockWindowAspect=0\r?$') 'Aspect lock can be turned off'
    }

    Send-Key $window ([int][char]'F')
    Wait-Gui { ([VelinGuiSmoke]::GetWindowLongPtr($window, -16).ToInt64() -band 0x00c00000) -eq 0 } 'fullscreen caption removal'
    $checks.Add('Fullscreen removes the standard window caption')
    Save-Capture $window 'fullscreen'
    Send-Key $window 0x1b
    Wait-Gui { ([VelinGuiSmoke]::GetWindowLongPtr($window, -16).ToInt64() -band 0x00c00000) -eq 0x00c00000 } 'windowed caption restoration'
    $checks.Add('Escape restores the standard window caption')

    $scale = [VelinGuiSmoke]::GetDpiForWindow($window) / 96.0
    [void][VelinGuiSmoke]::SetWindowPos($window, [IntPtr]::Zero, 0, 0, [int](1000 * $scale), [int](700 * $scale), 0x6)
    Start-Sleep -Milliseconds 150
    Save-Capture $window 'resized'
    # Command 55 is Compact in PlayerWindow::Action; WM_COMMAND invokes the same UI action.
    [void][VelinGuiSmoke]::PostMessage($window, 0x111, [IntPtr]55, [IntPtr]::Zero)
    Wait-Gui { (Window-Width $window) -lt 900 * $scale } 'compact window dimensions'
    $checks.Add('Compact action reduces the player dimensions')
    Save-Capture $window 'compact'

    [void][VelinGuiSmoke]::PostMessage($window, 0x111, [IntPtr]109, [IntPtr]::Zero)
    Wait-Gui { ([VelinGuiSmoke]::GetWindowLongPtr($window,-16).ToInt64() -band 0x00c00000) -eq 0 } 'picture-in-picture entry'
    Save-Capture $window 'pip'
    Send-Key $window 0x1b
    Wait-Gui { ([VelinGuiSmoke]::GetWindowLongPtr($window,-16).ToInt64() -band 0x00c00000) -eq 0x00c00000 } 'picture-in-picture restoration'
    foreach ($dialog in @(@(100,'Velin.Preferences','preferences'),@(101,'Velin.History','history'))) {
        [void][VelinGuiSmoke]::PostMessage($window,0x111,[IntPtr]$dialog[0],[IntPtr]::Zero)
        Wait-Gui { [VelinGuiSmoke]::FindClass($playback.Id,$dialog[1]) -ne [IntPtr]::Zero } $dialog[2]
        $dialogWindow=[VelinGuiSmoke]::FindClass($playback.Id,$dialog[1])
        Assert-Gui ([VelinGuiSmoke]::Responsive($dialogWindow)) ($dialog[2] + ' dialog responds')
        Save-Capture $dialogWindow $dialog[2]
        [void][VelinGuiSmoke]::PostMessage($dialogWindow,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
        Wait-Gui { [VelinGuiSmoke]::FindClass($playback.Id,$dialog[1]) -eq [IntPtr]::Zero } ($dialog[2]+' closing')
    }

    [void][VelinGuiSmoke]::PostMessage($window,0x111,[IntPtr]100,[IntPtr]::Zero)
    Wait-Gui { [VelinGuiSmoke]::FindClass($playback.Id,'Velin.Preferences') -ne [IntPtr]::Zero } 'preferences for shortcut editor'
    $preferences=[VelinGuiSmoke]::FindClass($playback.Id,'Velin.Preferences')
    [void][VelinGuiSmoke]::PostMessage($preferences,0x0111,[IntPtr]1101,[IntPtr]::Zero)
    Wait-Gui { [VelinGuiSmoke]::FindClass($playback.Id,'Velin.ShortcutEditor') -ne [IntPtr]::Zero } 'shortcut editor window'
    $shortcutEditor=[VelinGuiSmoke]::FindClass($playback.Id,'Velin.ShortcutEditor')
    $shortcutList=[VelinGuiSmoke]::GetDlgItem($shortcutEditor,1102)
    Assert-Gui ([VelinGuiSmoke]::Responsive($shortcutEditor)) 'Shortcut editor responds to window messages'
    Assert-Gui ($shortcutList -ne [IntPtr]::Zero -and [VelinGuiSmoke]::GetDlgItem($shortcutEditor,1103) -ne [IntPtr]::Zero -and [VelinGuiSmoke]::GetDlgItem($shortcutEditor,1104) -ne [IntPtr]::Zero) 'Shortcut editor exposes key, command and override-list controls'
    [void][VelinGuiSmoke]::SetText([VelinGuiSmoke]::GetDlgItem($shortcutEditor,1103),0x000c,[IntPtr]::Zero,'Ctrl+Shift+U')
    [void][VelinGuiSmoke]::SetText([VelinGuiSmoke]::GetDlgItem($shortcutEditor,1104),0x000c,[IntPtr]::Zero,'play-pause')
    [void][VelinGuiSmoke]::PostMessage($shortcutEditor,0x0111,[IntPtr]1105,[IntPtr]::Zero)
    Wait-Gui { [VelinGuiSmoke]::SendMessage($shortcutList,0x1004,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() -eq 1 } 'shortcut add'
    [void][VelinGuiSmoke]::SetText([VelinGuiSmoke]::GetDlgItem($shortcutEditor,1104),0x000c,[IntPtr]::Zero,'mute')
    [void][VelinGuiSmoke]::PostMessage($shortcutEditor,0x0111,[IntPtr]1105,[IntPtr]::Zero)
    Wait-Gui { [VelinGuiSmoke]::ListText($shortcutList,0,1) -eq 'mute' } 'shortcut update'
    Select-GuiListRow $shortcutList 0
    [void][VelinGuiSmoke]::PostMessage($shortcutEditor,0x0111,[IntPtr]1106,[IntPtr]::Zero)
    Wait-Gui { [VelinGuiSmoke]::SendMessage($shortcutList,0x1004,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() -eq 0 } 'shortcut removal'
    [void][VelinGuiSmoke]::PostMessage($shortcutEditor,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
    Wait-Gui { [VelinGuiSmoke]::FindClass($playback.Id,'Velin.ShortcutEditor') -eq [IntPtr]::Zero } 'shortcut editor closing'
    [void][VelinGuiSmoke]::PostMessage($preferences,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
    Wait-Gui { [VelinGuiSmoke]::FindClass($playback.Id,'Velin.Preferences') -eq [IntPtr]::Zero } 'preferences closing after shortcut editor'
    Assert-Gui ((Test-Path -LiteralPath (Join-Path $dataDirectory 'preferences.ini')) -and (Get-Content -LiteralPath (Join-Path $dataDirectory 'preferences.ini') -Raw) -notmatch 'Ctrl\+Shift\+U') 'Cancelling the shortcut editor discards its draft changes'
    [void][VelinGuiSmoke]::PostMessage($window,0x111,[IntPtr]100,[IntPtr]::Zero)
    Wait-Gui { [VelinGuiSmoke]::FindClass($playback.Id,'Velin.Preferences') -ne [IntPtr]::Zero } 'preferences for shortcut apply'
    $preferences=[VelinGuiSmoke]::FindClass($playback.Id,'Velin.Preferences')
    [void][VelinGuiSmoke]::PostMessage($preferences,0x0111,[IntPtr]1101,[IntPtr]::Zero)
    Wait-Gui { [VelinGuiSmoke]::FindClass($playback.Id,'Velin.ShortcutEditor') -ne [IntPtr]::Zero } 'shortcut editor for apply'
    $shortcutEditor=[VelinGuiSmoke]::FindClass($playback.Id,'Velin.ShortcutEditor')
    [void][VelinGuiSmoke]::SetText([VelinGuiSmoke]::GetDlgItem($shortcutEditor,1103),0x000c,[IntPtr]::Zero,'Ctrl+Shift+U')
    [void][VelinGuiSmoke]::SetText([VelinGuiSmoke]::GetDlgItem($shortcutEditor,1104),0x000c,[IntPtr]::Zero,'play-pause')
    [void][VelinGuiSmoke]::PostMessage($shortcutEditor,0x0111,[IntPtr]1105,[IntPtr]::Zero)
    Wait-Gui { [VelinGuiSmoke]::SendMessage([VelinGuiSmoke]::GetDlgItem($shortcutEditor,1102),0x1004,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() -eq 1 } 'shortcut override for apply'
    [void][VelinGuiSmoke]::PostMessage($shortcutEditor,0x0111,[IntPtr]1,[IntPtr]::Zero)
    Wait-Gui { [VelinGuiSmoke]::FindClass($playback.Id,'Velin.ShortcutEditor') -eq [IntPtr]::Zero } 'shortcut editor accepted'
    [void][VelinGuiSmoke]::PostMessage($preferences,0x0111,[IntPtr]1001,[IntPtr]::Zero)
    $preferencesPath=Join-Path $dataDirectory 'preferences.ini'
    Wait-Gui { (Get-Content -LiteralPath $preferencesPath -Raw) -match '(?m)^keyBindings=Ctrl\+Shift\+U=play-pause\r?$' } 'shortcut preference save'
    [void][VelinGuiSmoke]::PostMessage($preferences,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
    Wait-Gui { [VelinGuiSmoke]::FindClass($playback.Id,'Velin.Preferences') -eq [IntPtr]::Zero } 'preferences closing after shortcut apply'
    $checks.Add('Dedicated shortcut editor adds, updates, removes, cancels and applies overrides')

    # ShowInfo is action 35. The inspector owns its native controls in the player process.
    [void][VelinGuiSmoke]::PostMessage($window,0x111,[IntPtr]35,[IntPtr]::Zero)
    Wait-Gui { [VelinGuiSmoke]::FindClass($playback.Id,'Velin.NativeTool') -ne [IntPtr]::Zero } 'media inspector'
    $inspector=[VelinGuiSmoke]::FindClass($playback.Id,'Velin.NativeTool')
    Wait-Gui { [VelinGuiSmoke]::GetDlgItem($inspector,103) -ne [IntPtr]::Zero -and [VelinGuiSmoke]::GetDlgItem($inspector,101) -ne [IntPtr]::Zero } 'inspector controls'
    $watchInput=[VelinGuiSmoke]::GetDlgItem($inspector,103)
    $watchList=[VelinGuiSmoke]::GetDlgItem($inspector,101)
    $initialRows=[VelinGuiSmoke]::SendMessage($watchList,0x1004,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32()
    Assert-Gui ($watchInput -ne [IntPtr]::Zero -and $watchList -ne [IntPtr]::Zero) 'Inspector has watch controls'
    Assert-Gui ([VelinGuiSmoke]::SetText($watchInput,0x000c,[IntPtr]::Zero,'time-pos') -ne [IntPtr]::Zero) 'Inspector accepts a property name'
    [void][VelinGuiSmoke]::PostMessage($inspector,0x111,[IntPtr]104,[IntPtr]::Zero)
    $watchFile=Join-Path $dataDirectory 'inspector-watches.txt'
    Wait-Gui { (Test-Path -LiteralPath $watchFile) -and ((Get-Content -LiteralPath $watchFile -Raw) -eq "time-pos`n") } 'watch persistence'
    Wait-Gui { [VelinGuiSmoke]::SendMessage($watchList,0x1004,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() -eq $initialRows+1 } 'watch row'
    Wait-Gui { [VelinGuiSmoke]::ListText($watchList,$initialRows,1) -match '^\d' } 'live watched property value'
    $checks.Add('Inspector adds and persists a watch')
    [void][VelinGuiSmoke]::SetText($watchInput,0x000c,[IntPtr]::Zero,'video-crop')
    [void][VelinGuiSmoke]::PostMessage($inspector,0x111,[IntPtr]104,[IntPtr]::Zero)
    Wait-Gui { [VelinGuiSmoke]::SendMessage($watchList,0x1004,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() -eq $initialRows+2 } 'crop watch'
    $originalCrop = [VelinGuiSmoke]::ListText($watchList,$initialRows+1,1)
    # CropAspectBase + 1 selects the centered 4:3 crop through the menu command path.
    [void][VelinGuiSmoke]::PostMessage($window,0x111,[IntPtr]11001,[IntPtr]::Zero)
    try { Wait-Gui { [VelinGuiSmoke]::ListText($watchList,$initialRows+1,1) -match '^"?\d+x\d+\+\d+\+\d+"?$' } 'crop preset property' }
    catch { throw "Crop property after preset: $([VelinGuiSmoke]::ListText($watchList,$initialRows+1,1))" }
    $checks.Add('Crop preset changes the mpv crop property')
    # ClearCrop is action 117.
    [void][VelinGuiSmoke]::PostMessage($window,0x111,[IntPtr]117,[IntPtr]::Zero)
    Wait-Gui { [VelinGuiSmoke]::ListText($watchList,$initialRows+1,1) -eq $originalCrop } 'crop reset'
    Select-GuiListRow $watchList ($initialRows+1)
    [void][VelinGuiSmoke]::SendMessage($watchList,0x100,[IntPtr]0x2e,[IntPtr]1)
    Start-Sleep -Milliseconds 100
    [void][VelinGuiSmoke]::PostMessage($inspector,0x111,[IntPtr]105,[IntPtr]::Zero)
    Wait-Gui { [VelinGuiSmoke]::SendMessage($watchList,0x1004,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() -eq $initialRows+1 } 'crop watch removal'
    foreach ($property in @('sub-visibility','secondary-sub-visibility')) {
        [void][VelinGuiSmoke]::SetText($watchInput,0x000c,[IntPtr]::Zero,$property)
        [void][VelinGuiSmoke]::PostMessage($inspector,0x111,[IntPtr]104,[IntPtr]::Zero)
        $expectedRows = if ($property -eq 'sub-visibility') { $initialRows+2 } else { $initialRows+3 }
        Wait-Gui { [VelinGuiSmoke]::SendMessage($watchList,0x1004,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() -eq $expectedRows } "$property watch"
    }
    Wait-Gui { [VelinGuiSmoke]::ListText($watchList,$initialRows+1,1) -match '^(true|false)$' -and [VelinGuiSmoke]::ListText($watchList,$initialRows+2,1) -match '^(true|false)$' } 'subtitle visibility values'
    $primaryVisibility = [VelinGuiSmoke]::ListText($watchList,$initialRows+1,1)
    $secondaryVisibility = [VelinGuiSmoke]::ListText($watchList,$initialRows+2,1)
    # ToggleSecondarySubtitles is action 90, invoked through the same command path as the menu.
    [void][VelinGuiSmoke]::PostMessage($window,0x111,[IntPtr]90,[IntPtr]::Zero)
    Wait-Gui { [VelinGuiSmoke]::ListText($watchList,$initialRows+2,1) -ne $secondaryVisibility } 'secondary subtitle visibility toggle'
    Assert-Gui ([VelinGuiSmoke]::ListText($watchList,$initialRows+1,1) -eq $primaryVisibility) 'Secondary subtitle visibility leaves primary visibility unchanged'
    [void][VelinGuiSmoke]::PostMessage($window,0x111,[IntPtr]90,[IntPtr]::Zero)
    Wait-Gui { [VelinGuiSmoke]::ListText($watchList,$initialRows+2,1) -eq $secondaryVisibility } 'secondary subtitle visibility restoration'
    $checks.Add('Secondary subtitle visibility toggles and restores independently')
    foreach ($unused in 1..2) {
        Select-GuiListRow $watchList ($expectedRows-1)
        [void][VelinGuiSmoke]::SendMessage($watchList,0x100,[IntPtr]0x2e,[IntPtr]1)
        [void][VelinGuiSmoke]::PostMessage($inspector,0x111,[IntPtr]105,[IntPtr]::Zero)
        $expectedRows--
        Wait-Gui { [VelinGuiSmoke]::SendMessage($watchList,0x1004,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() -eq $expectedRows } 'subtitle visibility watch removal'
    }
    [void][VelinGuiSmoke]::SetText($watchInput,0x000c,[IntPtr]::Zero,'velin-property-does-not-exist')
    [void][VelinGuiSmoke]::PostMessage($inspector,0x111,[IntPtr]104,[IntPtr]::Zero)
    Wait-Gui { [VelinGuiSmoke]::SendMessage($watchList,0x1004,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() -eq $initialRows+2 } 'unavailable watch row'
    Wait-Gui { [VelinGuiSmoke]::ListText($watchList,$initialRows+1,1) -eq 'Unavailable or null' } 'unavailable watch value'
    $checks.Add('Inspector reports an unavailable property')
    Select-GuiListRow $watchList ($initialRows+1)
    [void][VelinGuiSmoke]::SendMessage($watchList,0x100,[IntPtr]0x2e,[IntPtr]1)
    Start-Sleep -Milliseconds 100
    [void][VelinGuiSmoke]::PostMessage($inspector,0x111,[IntPtr]105,[IntPtr]::Zero)
    Wait-Gui { (Get-Content -LiteralPath $watchFile -Raw) -eq "time-pos`n" } 'unavailable watch removal'
    [void][VelinGuiSmoke]::PostMessage($inspector,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
    Wait-Gui { [VelinGuiSmoke]::FindClass($playback.Id,'Velin.NativeTool') -eq [IntPtr]::Zero } 'inspector closing'
    [void][VelinGuiSmoke]::PostMessage($window,0x111,[IntPtr]35,[IntPtr]::Zero)
    Wait-Gui { [VelinGuiSmoke]::FindClass($playback.Id,'Velin.NativeTool') -ne [IntPtr]::Zero } 'inspector reopening'
    $inspector=[VelinGuiSmoke]::FindClass($playback.Id,'Velin.NativeTool')
    Wait-Gui { [VelinGuiSmoke]::GetDlgItem($inspector,101) -ne [IntPtr]::Zero -and [VelinGuiSmoke]::SendMessage([VelinGuiSmoke]::GetDlgItem($inspector,101),0x1004,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() -eq $initialRows+1 } 'restored inspector controls'
    $watchList=[VelinGuiSmoke]::GetDlgItem($inspector,101)
    Assert-Gui ([VelinGuiSmoke]::SendMessage($watchList,0x1004,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() -eq $initialRows+1) 'Inspector restores the saved watch on reopen'
    [void][VelinGuiSmoke]::PostMessage($inspector,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
    Wait-Gui { [VelinGuiSmoke]::FindClass($playback.Id,'Velin.NativeTool') -eq [IntPtr]::Zero } 'inspector closing after reopen'

    Send-Key $window 0x26
    Send-Key $window ([int][char]'M')
    Send-Key $window 0xdd
    Start-Sleep -Milliseconds 500
    Stop-Player $playback
    $saved = Get-Content -LiteralPath (Join-Path $dataDirectory 'preferences.ini') -Raw
    Assert-Gui ($saved -match '(?m)^volume=75\r?$') 'Volume shortcut persists the updated volume'
    Assert-Gui ($saved -match '(?m)^muted=1\r?$') 'Mute shortcut persists the updated mute state'
    Assert-Gui ($saved -match '(?m)^speed=1\.25\r?$') 'Speed shortcut persists the updated speed'
    Assert-Gui ($saved -match '(?m)^recent=') 'Successful playback populates recent files'

    $restarted=Start-Player $Executable $dataDirectory $MediaPath
    $window=[VelinGuiSmoke]::FindPlayer($restarted.Id)
    Wait-Gui { [VelinGuiSmoke]::Title($window) -ne 'Velin' } 'media after restart'
    [void][VelinGuiSmoke]::PostMessage($window,0x111,[IntPtr]35,[IntPtr]::Zero)
    Wait-Gui { [VelinGuiSmoke]::FindClass($restarted.Id,'Velin.NativeTool') -ne [IntPtr]::Zero } 'inspector after restart'
    $inspector=[VelinGuiSmoke]::FindClass($restarted.Id,'Velin.NativeTool')
    Wait-Gui { [VelinGuiSmoke]::GetDlgItem($inspector,101) -ne [IntPtr]::Zero -and [VelinGuiSmoke]::SendMessage([VelinGuiSmoke]::GetDlgItem($inspector,101),0x1004,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() -eq $initialRows+1 } 'restored watch after restart'
    $watchList=[VelinGuiSmoke]::GetDlgItem($inspector,101)
    Assert-Gui ([VelinGuiSmoke]::ListText($watchList,$initialRows,0) -eq 'time-pos') 'Inspector restores the watched property after restart'
    Select-GuiListRow $watchList $initialRows
    [void][VelinGuiSmoke]::SendMessage($watchList,0x100,[IntPtr]0x2e,[IntPtr]1)
    Start-Sleep -Milliseconds 100
    [void][VelinGuiSmoke]::PostMessage($inspector,0x111,[IntPtr]105,[IntPtr]::Zero)
    Wait-Gui { (Get-Item -LiteralPath $watchFile).Length -eq 0 } 'watch removal'
    Assert-Gui ([VelinGuiSmoke]::SendMessage($watchList,0x1004,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() -eq $initialRows) 'Inspector removes the selected watch'
    [void][VelinGuiSmoke]::PostMessage($inspector,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
    Wait-Gui { [VelinGuiSmoke]::FindClass($restarted.Id,'Velin.NativeTool') -eq [IntPtr]::Zero } 'inspector closing after removal'
    Stop-Player $restarted

    $missingDirectory = Join-Path $OutputDirectory 'missing-runtime'
    New-Item -ItemType Directory -Force -Path $missingDirectory | Out-Null
    $missingExecutable = Join-Path $missingDirectory 'velin.exe'
    Copy-Item -LiteralPath $Executable -Destination $missingExecutable
    $missing = Start-Player $missingExecutable (Join-Path $dataDirectory 'missing-runtime')
    $window = [VelinGuiSmoke]::FindPlayer($missing.Id)
    Start-Sleep -Milliseconds 250
    Assert-Gui ([VelinGuiSmoke]::Responsive($window)) 'Missing-runtime welcome window remains responsive'
    $hosts = @([VelinGuiSmoke]::Children($window) | Where-Object { [VelinGuiSmoke]::ClassName($_) -eq 'Velin.Video' })
    Assert-Gui ($hosts.Count -eq 1 -and @([VelinGuiSmoke]::Children($hosts[0])).Count -eq 0) 'Missing runtime does not create an mpv child window'
    Save-Capture $window 'missing-runtime'
    Stop-Player $missing

    $summary = [ordered]@{
        executable = $Executable
        media = $MediaPath
        dataDirectory = $dataDirectory
        checks = $checks.ToArray()
        screenshots = $captures
        note = 'PrintWindow captures only the tested application window. GPU video visibility and pause artwork require visual inspection of the saved screenshots.'
    }
    $summaryPath = Join-Path $OutputDirectory 'summary.json'
    $summary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $summaryPath -Encoding UTF8
    Write-Host "GUI smoke passed ($($checks.Count) assertions)."
    Write-Output $summaryPath
} finally {
    foreach ($process in $ownedProcesses) {
        try {
            $process.Refresh()
            if (-not $process.HasExited) {
                $window = [VelinGuiSmoke]::FindPlayer($process.Id)
                if ($window -ne [IntPtr]::Zero) { [void][VelinGuiSmoke]::PostMessage($window, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) }
                if (-not $process.WaitForExit(3000)) { $process.Kill(); [void]$process.WaitForExit(3000) }
            }
        } catch { Write-Warning "Could not clean up owned player process $($process.Id): $_" }
        $process.Dispose()
    }
    if ($previousDpi -ne [IntPtr]::Zero) { [void][VelinGuiSmoke]::SetThreadDpiAwarenessContext($previousDpi) }
}
