# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param([string]$Executable = 'build/mingw-release/velin.exe')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$Executable=[IO.Path]::GetFullPath((Join-Path $root $Executable))
$data=Join-Path $root ('.cache/plugin-windows/'+[guid]::NewGuid().ToString('N'))
$id='org.velin.window-fixture'
$plugin=Join-Path $data "plugins/$id"
New-Item -ItemType Directory -Force -Path $plugin | Out-Null
@{name='Window integration fixture';identifier=$id;version='1.0.0';author=@{name='Velin tests'};entry='main.js';globalEntry='global.js';permissions=@('network-request');allowedDomains=@('*')} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $plugin 'Info.json') -Encoding utf8
@{$id=@{enabled=$true;permissions=@('network-request')}} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $data 'plugins/state.json') -Encoding utf8
@'
iina.global.onMessage('ready',(data,source)=>iina.global.postMessage(source,'ping',{answer:42}));
iina.global.onMessage('pong',(data,source)=>{iina.file.write('@data/global.txt',String(data.answer));});
const child=iina.global.createPlayerInstance({label:'child'});
iina.file.write('@data/child-pid.txt',String(child));
'@ | Set-Content (Join-Path $plugin 'global.js') -Encoding utf8
@'
if(iina.global.getLabel()==='child'){
  iina.global.onMessage('ping',data=>{iina.file.write('@data/child.txt',String(data.answer));iina.global.postMessage('pong',data);});
  setInterval(()=>iina.global.postMessage('ready',{}),200);
}else{
  iina.input.onKeyUp('F8',()=>{iina.file.write('@data/key-up.txt','42');return true;});
  for(const kind of ['Down','Up','Drag']) iina.input['onMouse'+kind](iina.input.MOUSE,event=>{iina.file.write('@data/mouse-'+kind+'-'+event.clickCount+'.txt',JSON.stringify(event));return true;});
  iina.event.on('iina.window-loaded',()=>{
    iina.standaloneWindow.onMessage('loaded',data=>{iina.file.write('@data/webview.txt',String(data.answer));});
    iina.standaloneWindow.loadFile('view.html');iina.standaloneWindow.open();
  });
}
'@ | Set-Content (Join-Path $plugin 'main.js') -Encoding utf8
'<!doctype html><meta charset="UTF-8"><title>Velin WebView test</title><h1>Plugin HTML</h1><script>window.addEventListener("DOMContentLoaded",()=>iina.postMessage("loaded",{answer:42}));</script>' | Set-Content (Join-Path $plugin 'view.html') -Encoding utf8
Add-Type @'
using System;using System.Runtime.InteropServices;using System.Text;
public static class VelinPluginTest{
  [StructLayout(LayoutKind.Sequential)]public struct Rect{public int Left,Top,Right,Bottom;}
  delegate bool Callback(IntPtr window,IntPtr state);
  [DllImport("user32.dll")]static extern bool EnumWindows(Callback callback,IntPtr state);
  [DllImport("user32.dll")]static extern uint GetWindowThreadProcessId(IntPtr window,out uint process);
  [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern int GetClassName(IntPtr window,StringBuilder text,int length);
  [DllImport("user32.dll")]public static extern bool PostMessage(IntPtr window,uint message,IntPtr wp,IntPtr lp);
  [DllImport("user32.dll")]public static extern bool GetClientRect(IntPtr window,out Rect rect);
  [DllImport("user32.dll")]public static extern uint GetDpiForWindow(IntPtr window);
  [DllImport("user32.dll")]public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
  public static IntPtr Find(int process){IntPtr result=IntPtr.Zero;EnumWindows((window,state)=>{uint owner;GetWindowThreadProcessId(window,out owner);var name=new StringBuilder(128);GetClassName(window,name,128);if(owner==process&&name.ToString()=="Velin.Player"){result=window;return false;}return true;},IntPtr.Zero);return result;}
}
'@
$process=$null;$childProcess=$null
try{
    $start=[Diagnostics.ProcessStartInfo]::new($Executable,'--new-window');$start.UseShellExecute=$false;$start.EnvironmentVariables['VELIN_DATA_DIR']=$data
    $process=[Diagnostics.Process]::Start($start)
    $state=Join-Path $data "plugins/.data/$id"
    $deadline=[DateTime]::UtcNow.AddSeconds(25)
    while([DateTime]::UtcNow -lt $deadline){
        if($process.HasExited){throw 'Parent player exited unexpectedly.'}
        if((Test-Path "$state/child-pid.txt") -and !$childProcess){$childProcess=Get-Process -Id ([int](Get-Content "$state/child-pid.txt" -Raw))}
        if((Test-Path "$state/global.txt") -and (Test-Path "$state/child.txt") -and (Test-Path "$state/webview.txt")){break}
        Start-Sleep -Milliseconds 100
    }
    foreach($name in @('global','child','webview')){
        if(!(Test-Path "$state/$name.txt") -or (Get-Content "$state/$name.txt" -Raw).Trim() -ne '42'){throw "Missing or invalid $name round trip. Fixture: $data"}
    }
    $window=[VelinPluginTest]::Find($process.Id)
    if($window -eq [IntPtr]::Zero){throw 'Player input window is missing.'}
    [void][VelinPluginTest]::SetThreadDpiAwarenessContext([IntPtr](-4))
    [void][VelinPluginTest]::PostMessage($window,0x101,[IntPtr]0x77,[IntPtr]0)
    [void][VelinPluginTest]::PostMessage($window,0x201,[IntPtr]1,[IntPtr]0x002A0017)
    [void][VelinPluginTest]::PostMessage($window,0x200,[IntPtr]1,[IntPtr]0x002B0018)
    [void][VelinPluginTest]::PostMessage($window,0x202,[IntPtr]0,[IntPtr]0x002B0018)
    [void][VelinPluginTest]::PostMessage($window,0x203,[IntPtr]1,[IntPtr]0x002A0017)
    [void][VelinPluginTest]::PostMessage($window,0x202,[IntPtr]0,[IntPtr]0x002A0017)
    $deadline=[DateTime]::UtcNow.AddSeconds(5)
    $inputFiles=@('key-up','mouse-Down-1','mouse-Up-1','mouse-Drag-1','mouse-Down-2','mouse-Up-2')
    do {
      $missing=@($inputFiles | Where-Object { !(Test-Path "$state/$_.txt") })
      if(!$missing.Count){break}
      Start-Sleep -Milliseconds 50
    } while([DateTime]::UtcNow -lt $deadline)
    foreach($name in $inputFiles){
      if(!(Test-Path "$state/$name.txt")){throw "Missing native input callback: $name"}
    }
    if((Get-Content "$state/key-up.txt" -Raw).Trim() -ne '42'){throw 'Invalid key-up callback.'}
    $bounds=[VelinPluginTest+Rect]::new(); [void][VelinPluginTest]::GetClientRect($window,[ref]$bounds)
    $scale=[VelinPluginTest]::GetDpiForWindow($window)/96.0
    foreach($name in @('mouse-Down-1','mouse-Up-1','mouse-Drag-1','mouse-Down-2','mouse-Up-2')){
      $event=Get-Content "$state/$name.txt" -Raw | ConvertFrom-Json
      $isMoved=$name -in @('mouse-Up-1','mouse-Drag-1')
      $expectedX=if($isMoved){24}else{23}; $expectedY=if($isMoved){43}else{42}
      $expectedCount=if($name.EndsWith('-2')){2}else{1}
      $expectedPressure=if($name.StartsWith('mouse-Up')){0}else{1}
      if([Math]::Abs($event.x-$expectedX/$scale) -gt 0.01 -or [Math]::Abs($event.y-($bounds.Bottom-$expectedY)/$scale) -gt 0.01 -or $event.clickCount -ne $expectedCount -or $event.pressure -ne $expectedPressure){throw "Invalid native mouse event: $name"}
    }
    Write-Output 'PASS real parent/child plugin IPC, WebView2 HTML messaging and native key-up/mouse callbacks'
}finally{
    foreach($owned in @($childProcess,$process)){
        if($owned -and !$owned.HasExited){$window=[VelinPluginTest]::Find($owned.Id);if($window -ne [IntPtr]::Zero){[void][VelinPluginTest]::PostMessage($window,0x10,[IntPtr]::Zero,[IntPtr]::Zero)};if(!$owned.WaitForExit(5000)){$owned.Kill();$owned.WaitForExit()}}
    }
}
