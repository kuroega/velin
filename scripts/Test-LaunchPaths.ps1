# SPDX-License-Identifier: GPL-3.0-or-later
# Runs only when no Velin process is present and closes only the test's players.
[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build/mingw-release',
    [string]$MediaPath
)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class LaunchTestWin {
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr p,IntPtr a,string c,IntPtr t);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr w,out uint p);
 [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr w,uint m,IntPtr wp,IntPtr lp);
 [StructLayout(LayoutKind.Sequential)] struct ProcessBasicInfo { public IntPtr Reserved1, Peb, Reserved2, Reserved3, ProcessId, ParentId; }
 [DllImport("ntdll.dll")] static extern int NtQueryInformationProcess(IntPtr h,int c,out ProcessBasicInfo p,int n,out int size);
 public static long ParentId(IntPtr handle) { ProcessBasicInfo info; int size; if(NtQueryInformationProcess(handle,0,out info,Marshal.SizeOf(typeof(ProcessBasicInfo)),out size)!=0) return -1; return info.ParentId.ToInt64(); }
}
'@
$root=Split-Path -Parent $PSScriptRoot
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory=Join-Path $root $BuildDirectory }
$exe=Join-Path $BuildDirectory 'velin.exe'
$cli=Join-Path $BuildDirectory 'velin-cli.exe'
if (-not (Test-Path -LiteralPath $exe)) { $exe=Join-Path $BuildDirectory 'Release\velin.exe'; $cli=Join-Path $BuildDirectory 'Release\velin-cli.exe' }
if (-not (Test-Path -LiteralPath $exe) -or -not (Test-Path -LiteralPath $cli)) { throw 'Build velin and velin-cli before running this test.' }
if (-not $MediaPath) {
 $fixture=Get-ChildItem -LiteralPath (Join-Path $root '.cache\playback-fixtures') -Filter '*chaptered.mkv' -ErrorAction SilentlyContinue | Select-Object -First 1
 if ($fixture) { $MediaPath=$fixture.FullName }
}
if (-not $MediaPath -or -not (Test-Path -LiteralPath $MediaPath -PathType Leaf)) { throw 'Pass -MediaPath with a playable video fixture.' }
$fixture=[IO.Path]::GetFullPath($MediaPath)
$run=Join-Path $root ('.cache\launch-test-' + [guid]::NewGuid().ToString('N'))
$work1=Join-Path $run 'first working directory';$work2=Join-Path $run 'second working directory';$data1=Join-Path $run 'data one';$data2=Join-Path $run 'data two'
New-Item -ItemType Directory -Force -Path $work1,$work2,$data1,$data2 | Out-Null
$name1='--first ' + [char]0x6E2C + [char]0x8A66 + '.mkv'
$name2='forward ' + [char]0x5F71 + [char]0x7247 + '.mkv'
$name3='cli ' + [char]0x6E2C + [char]0x8A66 + '.mkv'
Copy-Item -LiteralPath $fixture -Destination (Join-Path $work1 $name1)
Copy-Item -LiteralPath $fixture -Destination (Join-Path $work2 $name2)
Copy-Item -LiteralPath $fixture -Destination (Join-Path $work2 $name3)
function PlayerWindow { [LaunchTestWin]::FindWindowExW([IntPtr]::Zero,[IntPtr]::Zero,'Velin.Player',[IntPtr]::Zero) }
function AssertSolePlayer([int]$expectedId) {
 $first=PlayerWindow
 if ($first -eq [IntPtr]::Zero) { throw 'The test player window is unavailable.' }
 [uint32]$windowOwner=0;[void][LaunchTestWin]::GetWindowThreadProcessId($first,[ref]$windowOwner)
 $other=[LaunchTestWin]::FindWindowExW([IntPtr]::Zero,$first,'Velin.Player',[IntPtr]::Zero)
 $players=@(Get-Process -Name velin -ErrorAction SilentlyContinue)
 if ($windowOwner -ne $expectedId -or $other -ne [IntPtr]::Zero -or $players.Count -ne 1 -or $players[0].Id -ne $expectedId) {
  throw 'An unexpected player is present; refusing to forward media.'
 }
}
function WaitUntil([scriptblock]$condition,[string]$description) {
 $until=[DateTime]::UtcNow.AddSeconds(20)
 do { if (& $condition) { return }; Start-Sleep -Milliseconds 100 } while ([DateTime]::UtcNow -lt $until)
 throw "Timed out waiting for $description"
}
function StartOwned([string]$program,[string]$cwd,[string]$launchArguments,[string]$data) {
 $start=[Diagnostics.ProcessStartInfo]::new();$start.FileName=$program;$start.WorkingDirectory=$cwd;$start.Arguments=$launchArguments;$start.UseShellExecute=$false;$start.EnvironmentVariables['VELIN_DATA_DIR']=$data
 return [Diagnostics.Process]::Start($start)
}
function CloseTestWindow([IntPtr]$window,[int]$expectedId) {
 if ($window -eq [IntPtr]::Zero) { return }
 [uint32]$id=0;[void][LaunchTestWin]::GetWindowThreadProcessId($window,[ref]$id)
 if ($id -ne $expectedId) { throw "Refusing to close window with unverified owner $id" }
 [void][LaunchTestWin]::PostMessageW($window,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
 WaitUntil { (Get-Process -Id $id -ErrorAction SilentlyContinue) -eq $null } 'owned player shutdown'
}
function HasHistory([string]$data,[string]$path) {
 $file=Join-Path $data 'history.tsv'
 return (Test-Path -LiteralPath $file) -and ([IO.File]::ReadAllText($file).Contains($path))
}
if ((Get-Process -Name velin -ErrorAction SilentlyContinue) -or (PlayerWindow) -ne [IntPtr]::Zero) { throw 'Another player is already running; no forwarding test was attempted.' }
$window=[IntPtr]::Zero
try {
 $controller=StartOwned $cli $work1 ('--new-window --data-dir "' + $data1 + '" -- "' + $name1 + '"') $data1
 if (-not $controller.WaitForExit(10000) -or $controller.ExitCode -ne 0) { throw 'CLI startup failed' }
 WaitUntil { (PlayerWindow) -ne [IntPtr]::Zero } 'CLI-started player'
 $window=PlayerWindow
 [uint32]$cliOwner=0;[void][LaunchTestWin]::GetWindowThreadProcessId($window,[ref]$cliOwner)
 $cliPlayer=Get-Process -Id $cliOwner
 if ([LaunchTestWin]::ParentId($cliPlayer.Handle) -ne $controller.Id) { throw 'CLI-started window is not owned by its child process' }
 WaitUntil { HasHistory $data1 (Join-Path $work1 $name1) } 'CLI-started relative Unicode media playback'
 CloseTestWindow $window $cliOwner;$window=[IntPtr]::Zero
 Write-Output 'PASS CLI starts player from another working directory with option-like Unicode media'
 $player=StartOwned $exe $work1 ('--data-dir "' + $data2 + '"') $data2
 WaitUntil { (PlayerWindow) -ne [IntPtr]::Zero } 'primary player'
 $window=PlayerWindow
 [uint32]$owner=0;[void][LaunchTestWin]::GetWindowThreadProcessId($window,[ref]$owner)
 if ($owner -ne $player.Id) { throw 'Primary window does not belong to the started player' }
 AssertSolePlayer $player.Id
 $forward=StartOwned $exe $work2 ('"' + $name2 + '"') $data2
 if (-not $forward.WaitForExit(10000) -or $forward.ExitCode -ne 0) { throw 'Direct forwarding process failed' }
 WaitUntil { HasHistory $data2 (Join-Path $work2 $name2) } 'forwarded relative Unicode media playback'
 AssertSolePlayer $player.Id
 $controller=StartOwned $cli $work2 ('"' + $name3 + '"') $data2
 if (-not $controller.WaitForExit(10000) -or $controller.ExitCode -ne 0) { throw 'CLI forwarding failed' }
 WaitUntil { HasHistory $data2 (Join-Path $work2 $name3) } 'CLI-forwarded relative Unicode media playback'
 CloseTestWindow $window $player.Id;$window=[IntPtr]::Zero
 Write-Output 'PASS direct and CLI forwarding preserve sender working directory and play Unicode media'
 Write-Output "Evidence: $run"
} finally {
 if ($window -ne [IntPtr]::Zero) {
  try { if ($player -and $player.Id -eq $owner) { CloseTestWindow $window $player.Id } elseif ($cliPlayer -and [LaunchTestWin]::ParentId($cliPlayer.Handle) -eq $controller.Id) { CloseTestWindow $window $cliOwner } } catch { Write-Warning $_ }
 }
}
