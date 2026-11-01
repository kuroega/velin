# SPDX-License-Identifier: GPL-3.0-or-later
# Exercises only an owned player process and a new disposable fixture.
[CmdletBinding()]
param([string]$Executable = (Join-Path $PSScriptRoot '..\build\mingw-release\velin.exe'))
$ErrorActionPreference = 'Stop'
$Executable = [IO.Path]::GetFullPath($Executable)
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$fixtureDir = Join-Path $root ('.cache\history-actions\run-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureDir | Out-Null
$fixture = Join-Path $fixtureDir 'history fixture.txt'
[IO.File]::WriteAllText($fixture, 'Disposable Velin history fixture')
$history = Join-Path $fixtureDir 'history.tsv'
[IO.File]::WriteAllText($history, "# Velin history v1`nentry`t$fixture`tFixture`t1`t0`t0`t1`t0`n", [Text.UTF8Encoding]::new($false))
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class VelinHistoryTest {
  delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc callback, IntPtr state);
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr state);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
  [DllImport("user32.dll",SetLastError=true)] static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w, IntPtr l, uint flags, uint timeout, out UIntPtr result);
  [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
  [DllImport("kernel32.dll")] static extern IntPtr VirtualAllocEx(IntPtr p, IntPtr a, UIntPtr s, uint t, uint protect);
  [DllImport("kernel32.dll")] static extern bool VirtualFreeEx(IntPtr p, IntPtr a, UIntPtr s, uint t);
  [DllImport("kernel32.dll")] static extern bool WriteProcessMemory(IntPtr p, IntPtr a, byte[] b, int n, out IntPtr written);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  public static IntPtr Find(int pid, string name, bool title) {
    IntPtr found=IntPtr.Zero;
    EnumWindows(delegate(IntPtr h, IntPtr ignored) { uint owner; GetWindowThreadProcessId(h,out owner); if(owner!=pid)return true;
      var s=new StringBuilder(256); if(title)GetWindowText(h,s,s.Capacity); else GetClassName(h,s,s.Capacity);
      if(s.ToString()==name){found=h;return false;} return true; },IntPtr.Zero); return found;
  }
  public static string ChildrenText(IntPtr parent) {
    var result=new StringBuilder();
    EnumChildWindows(parent,delegate(IntPtr h, IntPtr ignored) {var s=new StringBuilder(1024);GetWindowText(h,s,s.Capacity);if(s.Length>0)result.Append(s).Append(" | ");return true;},IntPtr.Zero);
    return result.ToString();
  }
  public static bool Responsive(IntPtr window) {
    UIntPtr result;
    return SendMessageTimeout(window,0,IntPtr.Zero,IntPtr.Zero,2,500,out result)!=IntPtr.Zero;
  }
  public static string OwnedWindows(int pid) {
    var result=new StringBuilder();
    EnumWindows(delegate(IntPtr h, IntPtr ignored) {uint owner;GetWindowThreadProcessId(h,out owner);if(owner!=pid)return true;
      var name=new StringBuilder(256);var title=new StringBuilder(256);
      GetClassName(h,name,name.Capacity);GetWindowText(h,title,title.Capacity);
      result.Append('[').Append(name).Append(": ").Append(title).Append(", enabled=").Append(IsWindowEnabled(h)).Append(']');return true;
    },IntPtr.Zero);
    return result.ToString();
  }
  public static void Select(IntPtr list, int pid, int index) {
    IntPtr p=OpenProcess(0x438,false,(uint)pid); if(p==IntPtr.Zero)throw new Exception("OpenProcess failed");
    IntPtr remote=VirtualAllocEx(p,IntPtr.Zero,(UIntPtr)128,0x3000,4); if(remote==IntPtr.Zero){CloseHandle(p);throw new Exception("VirtualAllocEx failed");}
    try { var data=new byte[128]; BitConverter.GetBytes(3).CopyTo(data,12); BitConverter.GetBytes(3).CopyTo(data,16);
      IntPtr written; if(!WriteProcessMemory(p,remote,data,data.Length,out written))throw new Exception("WriteProcessMemory failed");
      SendMessage(list,0x102b,(IntPtr)index,remote);
    } finally { VirtualFreeEx(p,remote,UIntPtr.Zero,0x8000); CloseHandle(p); }
  }
}
'@
function Wait-For([scriptblock]$Predicate, [string]$Description) {
  $end=[DateTime]::UtcNow.AddSeconds(30)
  do { if (& $Predicate) { return }; Start-Sleep -Milliseconds 50 } while ([DateTime]::UtcNow -lt $end)
  throw "Timed out waiting for $Description"
}
$start=[Diagnostics.ProcessStartInfo]::new()
$start.FileName=$Executable
$start.WorkingDirectory=Split-Path -Parent $Executable
$start.UseShellExecute=$false
$start.EnvironmentVariables['VELIN_DATA_DIR']=$fixtureDir
$player=[Diagnostics.Process]::Start($start)
try {
  Wait-For { [VelinHistoryTest]::Find($player.Id,'Velin.Player',$false) -ne [IntPtr]::Zero } 'player'
  $window=[VelinHistoryTest]::Find($player.Id,'Velin.Player',$false)
  [void][VelinHistoryTest]::PostMessage($window,0x111,[IntPtr]101,[IntPtr]::Zero)
  Wait-For { [VelinHistoryTest]::Find($player.Id,'Velin.History',$false) -ne [IntPtr]::Zero } 'history dialog'
  $dialog=[VelinHistoryTest]::Find($player.Id,'Velin.History',$false)
  $list=[VelinHistoryTest]::GetDlgItem($dialog,2002)
  Wait-For { [VelinHistoryTest]::SendMessage($list,0x1004,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() -eq 1 } 'history entry'
  [VelinHistoryTest]::Select($list,$player.Id,0)
  [void][VelinHistoryTest]::PostMessage($dialog,0x111,[IntPtr]2010,[IntPtr]::Zero)
  Wait-For { [VelinHistoryTest]::Find($player.Id,'Move to Recycle Bin',$true) -ne [IntPtr]::Zero } 'confirmation'
  $confirm=[VelinHistoryTest]::Find($player.Id,'Move to Recycle Bin',$true)
  [void][VelinHistoryTest]::PostMessage($confirm,0x111,[IntPtr]7,[IntPtr]::Zero)
  Wait-For { [VelinHistoryTest]::Find($player.Id,'Move to Recycle Bin',$true) -eq [IntPtr]::Zero } 'confirmation cancellation'
  Wait-For { [VelinHistoryTest]::IsWindowEnabled($dialog) -and [VelinHistoryTest]::Responsive($dialog) } 'history dialog to resume after cancellation'
  if (-not (Test-Path -LiteralPath $fixture) -or (Get-Content -LiteralPath $history -Raw) -notmatch 'entry') { throw 'Cancelling changed the fixture or history.' }
  [void][VelinHistoryTest]::PostMessage($dialog,0x111,[IntPtr]2010,[IntPtr]::Zero)
  Wait-For { [VelinHistoryTest]::Find($player.Id,'Move to Recycle Bin',$true) -ne [IntPtr]::Zero } 'second confirmation'
  $confirm=[VelinHistoryTest]::Find($player.Id,'Move to Recycle Bin',$true)
  [void][VelinHistoryTest]::PostMessage($confirm,0x111,[IntPtr]6,[IntPtr]::Zero)
  try {
    Wait-For {
      $errorWindow=[VelinHistoryTest]::Find($player.Id,'Playback history',$true)
      if ($errorWindow -ne [IntPtr]::Zero) { throw ('Recycle operation failed: ' + [VelinHistoryTest]::ChildrenText($errorWindow)) }
      -not (Test-Path -LiteralPath $fixture)
    } 'fixture recycling'
  } catch {
    $enabled=[VelinHistoryTest]::IsWindowEnabled($dialog)
    $responsive=[VelinHistoryTest]::Responsive($dialog)
    $owned=[VelinHistoryTest]::OwnedWindows($player.Id)
    throw "$_ History dialog enabled=$enabled, responsive=$responsive. Player windows: $owned"
  }
  Wait-For { [VelinHistoryTest]::SendMessage($list,0x1004,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() -eq 0 } 'history removal'
  if ((Get-Content -LiteralPath $history -Raw) -match 'entry') { throw 'Recycled file remains in history.' }
  Write-Host 'PASS history Recycle Bin cancellation and confirmed removal'
} finally {
  if (-not $player.HasExited) { $player.Kill(); $player.WaitForExit() }
}
