# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param([string]$Executable = 'build/mingw-release/velin.exe', [string]$Python = 'python', [switch]$IncludeWebSockets)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = if ([IO.Path]::IsPathRooted($Executable)) { $Executable } else { Join-Path $root $Executable }
$data = Join-Path $root ('.cache/webview-permissions/' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $data | Out-Null
$portFile = Join-Path $data 'port.txt'
$requestFile = Join-Path $data 'requests.txt'
$server = $null
$player = $null
try {
    $serverScript = Join-Path $root 'tests/webview_mock_server.py'
    $server = Start-Process -FilePath (Get-Command $Python).Source -ArgumentList @(('"' + $serverScript + '"'), ('"' + $portFile + '"'), ('"' + $requestFile + '"')) -WorkingDirectory $root -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $data 'server.out.log') -RedirectStandardError (Join-Path $data 'server.error.log')
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while (!(Test-Path $portFile) -and [DateTime]::UtcNow -lt $deadline -and !$server.HasExited) { Start-Sleep -Milliseconds 50 }
    if (!(Test-Path $portFile)) { throw "Fixture server did not start: $data" }
    $port = (Get-Content $portFile -Raw).Trim()
    $state = @{}
    foreach ($test in @(
        @{name='allowed'; permissions=@('network-request'); domains=@('*')},
        @{name='no-permission'; permissions=@(); domains=@('*')},
        @{name='restricted'; permissions=@('network-request'); domains=@('127.0.0.1')},
        @{name='wrong-domain'; permissions=@('network-request'); domains=@('localhost')}
    )) {
        $id = 'org.velin.webview-' + $test.name
        $plugin = Join-Path $data "plugins/$id"
        New-Item -ItemType Directory -Force -Path $plugin | Out-Null
        @{name=$id; identifier=$id; version='1.0.0'; author=@{name='Velin tests'}; entry='main.js'; permissions=$test.permissions; allowedDomains=$test.domains} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $plugin 'Info.json') -Encoding utf8
        $state[$id] = @{enabled=$true; permissions=$test.permissions}
        @'
iina.event.on('iina.window-loaded',()=>{
  iina.standaloneWindow.onMessage('done',data=>iina.file.write('@data/done.txt',JSON.stringify(data)));
  try { iina.standaloneWindow.loadFile('view.html');iina.standaloneWindow.open(); }
  catch(e) { iina.file.write('@data/done.txt',JSON.stringify({denied:String(e)})); }
});
'@ | Set-Content (Join-Path $plugin 'main.js') -Encoding utf8
        $html = @'
<!doctype html><script>
window.addEventListener('DOMContentLoaded',async()=>{
  const url='http://127.0.0.1:PORT/NAME';
  const testSockets=SOCKET_TEST;
  const request=u=>fetch(u,{mode:'no-cors'}).then(r=>r.type,e=>String(e));
  const direct=await request(url);
  const frame=new Promise(resolve=>{
    window.addEventListener('message',e=>{if(e.data&&e.data.source==='frame')resolve(e.data.socket);},{once:true});
    const child=document.createElement('iframe');
    const socketCode=testSockets?"const done=new Promise(r=>{try{const sock=new WebSocket('"+url.replace('http:','ws:')+"-frame-socket');sock.onerror=()=>r('error');sock.onopen=()=>{sock.close();r('open');};}catch(e){r('error');}});":"const done=Promise.resolve('not-tested');";
    child.srcdoc="<script>"+socketCode+"Promise.all([fetch('"+url+"-frame',{mode:'no-cors'}).catch(()=>{}),done]).then(([,socket])=>parent.postMessage({source:'frame',socket},'*'))<\/script>";
    document.body.appendChild(child);
  });
  const worker=new Promise(resolve=>{
    const socketCode=testSockets?"const done=new Promise(r=>{try{const sock=new WebSocket('"+url.replace('http:','ws:')+"-worker-socket');sock.onerror=()=>r('error');sock.onopen=()=>{sock.close();r('open');};}catch(e){r('error');}});":"const done=Promise.resolve('not-tested');";
    const code=socketCode+"Promise.all([fetch('"+url+"-worker',{mode:'no-cors'}).catch(()=>{}),done]).then(([,socket])=>postMessage(socket))";
    const instance=new Worker(URL.createObjectURL(new Blob([code],{type:'text/javascript'})));
    instance.onmessage=e=>resolve(e.data);instance.onerror=()=>resolve('worker-error');
  });
  const socket=testSockets?new Promise(resolve=>{
    try { const ws=new WebSocket(url.replace('http:','ws:')+'-socket');ws.onerror=()=>resolve('error');ws.onopen=()=>{ws.close();resolve('open');}; }
    catch(e) { resolve('error'); }
  }):Promise.resolve('not-tested');
  iina.postMessage('done',{direct,frame:await frame,worker:await worker,socket:await socket});
});</script><body></body>
'@
        $html.Replace('PORT',$port).Replace('NAME',$test.name).Replace('SOCKET_TEST', $IncludeWebSockets.ToString().ToLowerInvariant()) | Set-Content (Join-Path $plugin 'view.html') -Encoding utf8
    }
    $state | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $data 'plugins/state.json') -Encoding utf8
    $start = [Diagnostics.ProcessStartInfo]::new($exe, '--new-window')
    $start.UseShellExecute = $false
    $start.EnvironmentVariables['VELIN_DATA_DIR'] = $data
    $player = [Diagnostics.Process]::Start($start)
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        if ($player.HasExited) { throw "Player exited unexpectedly: $data" }
        $missing = @('allowed','no-permission','restricted','wrong-domain' | Where-Object { !(Test-Path (Join-Path $data "plugins/.data/org.velin.webview-$_/done.txt")) })
        if (!$missing.Count) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($missing.Count) { throw "WebView fixture did not finish ($($missing -join ', ')): $data" }
    $requests = @(if (Test-Path $requestFile) { Get-Content $requestFile })
    $expected = @('/allowed','/allowed-frame','/allowed-worker')
    if ($IncludeWebSockets) { $expected += @('/allowed-socket','/allowed-frame-socket','/allowed-worker-socket') }
    if ($requests.Count -ne $expected.Count -or @(Compare-Object $requests $expected).Count) { throw "Unexpected network requests ($($requests -join ', ')): $data" }
    foreach ($name in @('allowed','no-permission','restricted','wrong-domain')) {
        $result = Get-Content (Join-Path $data "plugins/.data/org.velin.webview-$name/done.txt") -Raw | ConvertFrom-Json
        if ($name -ne 'allowed') {
            if ($result.denied -notmatch 'WebSocket network permissions cannot be enforced') { throw "Missing WebView policy denial for $name ($($result | ConvertTo-Json -Compress)): $data" }
            continue
        }
        $expectedSocket = if (!$IncludeWebSockets) { 'not-tested' } else { 'open' }
        if ($result.frame -ne $expectedSocket -or $result.worker -ne $expectedSocket -or $result.socket -ne $expectedSocket) { throw "Unexpected frame, worker, or socket result for $name ($($result | ConvertTo-Json -Compress)): $data" }
    }
    Write-Output 'PASS unrestricted WebView requests and fail-closed restricted WebView policy'
} finally {
    if ($player -and !$player.HasExited) { $player.CloseMainWindow() | Out-Null; if (!$player.WaitForExit(5000)) { $player.Kill(); $player.WaitForExit() } }
    if ($server -and !$server.HasExited) { $server.Kill(); $server.WaitForExit() }
}
