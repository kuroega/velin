// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace velin {
inline constexpr char iinaCompatibilityScript[] = R"VELINJS(
(() => {
  'use strict';
  const host = (name, args = []) => __velinHost(name, args);
  const listeners = new Map(), pending = new Map(), actions = new Map();
  const inputs = [], timers = new Map(), modules = new Map(), messages = new Map();
  let serial = 0;
  const next = () => String(++serial);
  const ui = (name) => (...args) => host(name, args);
  const iina = globalThis.iina = {};
  const socketHandlers=new Map(),socketSends=new Map();
  iina.ws={createServer:options=>host('ws.create',[options]),startServer:()=>host('ws.start'),stopServer:()=>host('ws.stop'),
    onStateUpdate:handler=>socketHandlers.set('state',handler),onMessage:handler=>socketHandlers.set('message',handler),
    onNewConnection:handler=>socketHandlers.set('newConnection',handler),onConnectionStateUpdate:handler=>socketHandlers.set('connectionState',handler),
    sendText:(connection,text)=>new Promise((resolve,reject)=>{const token=next();socketSends.set(token,{resolve,reject});try{host('ws.send',[connection,text,token]);}catch(error){socketSends.delete(token);reject(error);}})};
  globalThis.__velinSocketEvent=(event,args)=>{
    if(event==='sent'){const task=socketSends.get(args[0]);if(task){socketSends.delete(args[0]);args[1]?task.resolve(args[2]):task.reject(new Error(args[2]));}return;}
    const handler=socketHandlers.get(event);if(typeof handler!=='function')return;
    if(event==='message'){const value=args[1];handler(args[0],{text:()=>value.text,data:()=>new Uint8Array(value.bytes)});}
    else handler(...args);
  };
  iina.console = Object.fromEntries(['log', 'info', 'warn', 'error', 'debug'].map(level =>
    [level, (...args) => host('console.' + level, args.map(value => typeof value === 'string' ? value : JSON.stringify(value)))]));
  globalThis.console = iina.console;
  iina.mpv = {
    getNative: property => host('mpv.get', [property]),
    getFlag: property => Boolean(host('mpv.get', [property])),
    getNumber: property => Number(host('mpv.get', [property]) ?? 0),
    getString: property => { const value = host('mpv.get', [property]); return value == null ? null : String(value); },
    set: (property, value) => host('mpv.set', [property, value]),
    command: (command, args = []) => host('mpv.command', [command, args]),
    addHook: (name, priority, callback) => {
      const key = host('mpv.addHook', [name, priority]);
      actions.set('hook:' + key, callback);
    }
  };
  iina.event = {
    on: (event, callback) => {
      if (typeof callback !== 'function') throw new TypeError('Event callback must be a function');
      host('event.observe', [event]); const id = next();
      listeners.set(id, { event, callback }); return id;
    },
    off: (event, id) => { const listener = listeners.get(id); if (listener?.event === event) listeners.delete(id); }
  };
  globalThis.__velinEmit = (event, args) => {
    for (const [id, listener] of [...listeners]) if (listener.event === event && listeners.has(id)) {
      try { listener.callback(...args); } catch (error) { iina.console.error(error.stack || String(error)); }
    }
  };
  globalThis.__velinHook = (key, continuation) => {
    const callback = actions.get('hook:' + key);
    if (!callback) return host('mpv.continueHook', [continuation]);
    let continued = false;
    const next = () => { if (!continued) { continued = true; host('mpv.continueHook', [continuation]); } };
    try { const result = callback(next); if (result?.then) result.then(next, error => { iina.console.error(String(error)); next(); });
      else if (callback.length === 0) next(); }
    catch (error) { next(); throw error; }
  };
  const proxy = name => new Proxy({}, {
    get: (_, property) => property === 'loadTrack'
      ? path => host('core.track.load', [name, path]) : host('core.get', [name, String(property)]),
    set: (_, property, value) => { host('core.set', [name, String(property), value]); return true; }
  });
  iina.core = {
    open: path => host('core.open', [path]), osd: ui('osd'),
    pause: () => iina.mpv.set('pause', true), resume: () => iina.mpv.set('pause', false),
    stop: () => iina.mpv.command('stop'), seek: (seconds, exact = true) => iina.mpv.command('seek', [String(seconds), exact ? 'relative+exact' : 'relative']),
    seekTo: seconds => iina.mpv.command('seek', [String(seconds), 'absolute+exact']),
    setSpeed: speed => iina.mpv.set('speed', speed),
    getChapters: () => host('core.chapters'), playChapter: index => iina.mpv.set('chapter', index),
    setUIVisibility: ui('core.setUIVisibility'), getHistory: ui('core.getHistory'),
    getRecentDocuments: ui('core.getRecentDocuments'), getVersion: () => host('core.version'),
    window: proxy('window'), status: proxy('status'), audio: proxy('audio'), subtitle: proxy('subtitle'), video: proxy('video')
  };
  iina.playlist = {
    list: () => host('playlist.list'), count: () => host('playlist.list').length,
    add: (url, at = -1) => host('playlist.add', [url, at]),
    remove: index => host('playlist.remove', [index]), move: (index, to) => host('playlist.move', [index, to]),
    play: index => host('playlist.play', [index]), playNext: () => iina.mpv.command('playlist-next', ['force']),
    playPrevious: () => iina.mpv.command('playlist-prev', ['force']),
    registerMenuItemBuilder: builder => { if (typeof builder !== 'function') throw new TypeError('Expected menu builder');
      globalThis.__velinPlaylistMenuBuilder = builder; }
  };
  let menuItems = [];
  const syncMenu = () => host('menu.replace', menuItems);
  iina.menu = {
    item: (title, action, options = {}) => {
      const id = next(); if (typeof action === 'function') actions.set(id, action);
      return { identifier: id, title, enabled: options.enabled ?? true, selected: options.selected ?? false,
        keyBinding: options.keyBinding ?? '', subItems: [],
        addSubMenuItem(item) { this.subItems.push(item); syncMenu(); return this; } };
    },
    separator: () => ({ identifier: next(), title: '', separator: true, enabled: false }),
    addItem: item => { menuItems.push(item); syncMenu(); }, items: () => menuItems,
    removeAt: index => { if (index < 0 || index >= menuItems.length) return false; menuItems.splice(index, 1); syncMenu(); return true; },
    removeAllItems: () => { menuItems = []; syncMenu(); }, forceUpdate: syncMenu
  };
  globalThis.__velinMenuInvoke = identifier => { const action = actions.get(identifier); if (action) action(); };
  const normalizeKey = value => String(value).replace(/(?:CMD|META|Command)\+/ig, 'Ctrl+').replace(/Control\+/ig, 'Ctrl+');
  iina.input = { PRIORITY_LOW: 0, PRIORITY_HIGH: 100, MOUSE: 'MBTN_LEFT', RIGHT_MOUSE: 'MBTN_RIGHT', OTHER_MOUSE: 'MBTN_MID',
    normalizeKeyCode: normalizeKey, getAllKeyBindings: () => host('input.bindings') };
  for (const [name, event] of Object.entries({onKeyDown:'keyDown', onKeyUp:'keyUp', onMouseDown:'mouseDown', onMouseUp:'mouseUp', onMouseDrag:'mouseDrag'}))
    iina.input[name] = (key, callback, priority = 0) => { if (typeof callback !== 'function') throw new TypeError('Expected input callback');
      inputs.push({event, key:normalizeKey(key), callback, priority:Number(priority)}); inputs.sort((a,b) => b.priority-a.priority); };
  globalThis.__velinInput = (event, key, args) => {
    for (const listener of inputs) if (listener.event === event && listener.key === normalizeKey(key)) {
      if (listener.callback(args) === true) return true;
    } return false;
  };
)VELINJS"
R"VELINJS(
  iina.preferences = { get:key => host('preferences.get', [key]), set:(key,value) => host('preferences.set',[key,value]),
    sync:() => host('preferences.sync'), open:ui('preferences.open') };
  iina.file = Object.fromEntries(['exists','list','read','write','delete','trash','showInFinder'].map(name => [name, (...args) => host('file.'+name,args)]));
  iina.file.handle = (path, mode = 'r') => {
    const identifier=host('file.handle.open',[path,mode]);let closed=false;
    const valid = () => { if (closed) throw new Error('File handle is closed'); };
    const invoke=(operation,...args)=>{valid();return host('file.handle.'+operation,[identifier,...args]);};
    const seek=position=>invoke('seek',Math.max(0,Number(position)));
    return {offset:()=>invoke('offset'),seekTo:seek,seek,seekToEnd:()=>invoke('end'),
      read:length=>new Uint8Array(invoke('read',length??null)),readToEnd:()=>new Uint8Array(invoke('read',null)),
      write:data=>{let bytes;if(typeof data==='string'){const text=unescape(encodeURIComponent(data));bytes=Array.from(text,c=>c.charCodeAt(0));}
        else bytes=Array.from(data instanceof ArrayBuffer?new Uint8Array(data):data);invoke('write',bytes);},
      close:()=>{if(!closed){invoke('close');closed=true;}}};
  };
  const asynchronous = (operation, parameters) => {
    const id = String(host('async.start', [operation, parameters]));
    return new Promise((resolve,reject) => pending.set(id,{resolve,reject}));
  };
  globalThis.__velinAsyncComplete = (id, data, error) => {
    const promise = pending.get(String(id)); if (!promise) return; pending.delete(String(id));
    if (error) promise.reject(new Error(error)); else promise.resolve(data);
  };
  iina.http = {};
  for (const method of ['get','post','put','patch','delete']) iina.http[method] = (url, options = {}) => {
    if (options.params) url += (url.includes('?')?'&':'?') + Object.entries(options.params).map(([key,value])=>encodeURIComponent(key)+'='+encodeURIComponent(value)).join('&');
    const headers = {...(options.headers ?? {})}; let body='';
    if (options.data) { body = Object.entries(options.data).map(([key,value])=>encodeURIComponent(key)+'='+encodeURIComponent(value)).join('&');
      headers['Content-Type'] ??= 'application/x-www-form-urlencoded'; }
    if (options.json) { body=JSON.stringify(options.json); headers['Content-Type'] ??= 'application/json'; }
    return asynchronous('http.request', {url,method:method.toUpperCase(),headers,body}).then(response => {
      if (!response.ok) throw response; return response;
    });
  };
  iina.http.download = (url, destination, options = {}) => asynchronous('plugin.download', {url,destination,...options});
  const xmlEscape=value=>String(value).replaceAll('&','&amp;').replaceAll('<','&lt;').replaceAll('>','&gt;').replaceAll('"','&quot;').replaceAll("'",'&apos;');
  const xmlValue=value=>'<value>'+ (value==null?'<nil/>':typeof value==='boolean'?'<boolean>'+(value?1:0)+'</boolean>':
    typeof value==='number'?'<'+(Number.isInteger(value)?'int':'double')+'>'+value+'</'+(Number.isInteger(value)?'int':'double')+'>':
    Array.isArray(value)?'<array><data>'+value.map(xmlValue).join('')+'</data></array>':
    typeof value==='object'?'<struct>'+Object.entries(value).map(([key,item])=>'<member><name>'+xmlEscape(key)+'</name>'+xmlValue(item)+'</member>').join('')+'</struct>':
    '<string>'+xmlEscape(value)+'</string>')+'</value>';
  const xmlDecode=xml=>{
    if(/<!DOCTYPE|<!ENTITY/i.test(xml)) throw new Error('XML entities are prohibited');
    const tokens=xml.replace(/<\?xml[^?]*\?>/g,'').match(/<[^>]+>|[^<]+/g)||[]; let cursor=0;
    const text=value=>value.replaceAll('&lt;','<').replaceAll('&gt;','>').replaceAll('&quot;','"').replaceAll('&apos;',"'").replaceAll('&amp;','&');
    const skip=()=>{while(cursor<tokens.length && /^\s*$/.test(tokens[cursor]))cursor++;};
    const expect=tag=>{skip();if(tokens[cursor++]!==tag)throw new Error('Invalid XML-RPC response: expected '+tag);};
    const parse=()=>{
      expect('<value>'); skip(); let value;
      if(tokens[cursor]==='<struct>') { cursor++; value={}; skip(); while(tokens[cursor]!=='</struct>') {
        expect('<member>');expect('<name>');const name=text(tokens[cursor++]);expect('</name>');value[name]=parse();expect('</member>');skip();
      } cursor++; }
      else if(tokens[cursor]==='<array>') {cursor++;expect('<data>');value=[];skip();while(tokens[cursor]!=='</data>'){value.push(parse());skip();}cursor++;expect('</array>');}
      else if(tokens[cursor]==='<nil/>') {cursor++;value=null;}
      else if(tokens[cursor].startsWith('<')) {const tag=tokens[cursor++].slice(1,-1);let raw='';while(cursor<tokens.length&&tokens[cursor]!=='</'+tag+'>')raw+=tokens[cursor++];expect('</'+tag+'>');value=/^(int|i4|i8|double)$/.test(tag)?Number(raw):tag==='boolean'?raw==='1':text(raw);}
      else {value=text(tokens[cursor++]);}expect('</value>');return value;
    };
    const valueIndex=tokens.indexOf('<value>');if(valueIndex<0)throw new Error('XML-RPC response has no value');cursor=valueIndex;const result=parse();
    if(xml.includes('<fault>'))throw result;return result;
  };
  iina.http.xmlrpc = location => ({call:(method,args=[])=>asynchronous('http.request',{url:location,method:'POST',headers:{'Content-Type':'text/xml'},
    body:'<?xml version="1.0"?><methodCall><methodName>'+xmlEscape(method)+'</methodName><params>'+args.map(value=>'<param>'+xmlValue(value)+'</param>').join('')+'</params></methodCall>'})
    .then(response=>{if(!response.ok)throw response;return xmlDecode(response.text);})});
  iina.utils = Object.fromEntries(['fileInPath','resolvePath','ask','prompt','chooseFile','keychainWrite','keychainRead','open','preferredLocalizations'].map(name=>[name,(...args)=>host('utils.'+name,args)]));
  const processHooks = new Map();
  globalThis.__velinProcessChunk=(id,stream,text)=>{
    const hooks=processHooks.get(String(id));const hook=hooks?.[stream];if(hook)hook(text);
  };
  iina.utils.exec = (file,args = [],cwd,stdoutHook,stderrHook) => {
    for(const hook of [stdoutHook,stderrHook]) if(hook!=null && typeof hook!=='function') throw new TypeError('Expected process stream callback');
    if(!Array.isArray(args) || args.some(arg=>typeof arg!=='string')) throw new TypeError('Expected string array arguments');
    const id=String(host('async.start',['process.exec',{file,args,cwd,streamStdout:typeof stdoutHook==='function',streamStderr:typeof stderrHook==='function'}]));
    processHooks.set(id,{stdout:stdoutHook,stderr:stderrHook});
    return new Promise((resolve,reject)=>pending.set(id,{resolve,reject})).finally(()=>processHooks.delete(id));
  };
  iina.utils.ERROR_BINARY_NOT_FOUND=-1; iina.utils.ERROR_RUNTIME=-2;
  for (const kind of ['overlay','sidebar','standalone']) {
    const object = {};
    for (const method of ['loadFile','simpleMode','setContent','setStyle','show','hide','open','close','isOpen','setOpacity','setClickable','postMessage','setFrame','setProperty']) object[method]=ui(kind+'.'+method);
    object.onMessage=(name,callback)=>{ if (typeof callback !== 'function') throw new TypeError('Expected message callback'); messages.set(kind+':'+name,callback); };
    iina[kind]=object;
  }
  iina.standaloneWindow = iina.standalone;
  globalThis.__velinWebMessage=(kind,name,data)=>{ const callback=messages.get(kind+':'+name); if(callback) callback(data); };
  const globalMessages=new Map();
  iina.global={ createPlayerInstance:options=>host('global.createPlayerInstance',[options]),
    getLabel:()=>host('global.getLabel'),postMessage:(...args)=>host('global.postMessage',args),
    onMessage:(name,callback)=>{if(typeof callback!=='function')throw new TypeError('Expected message callback');globalMessages.set(name,callback);} };
  globalThis.__velinGlobalMessage=(name,data,source)=>{const callback=globalMessages.get(name);if(callback)callback(data,source);};
  iina.subtitle = { CUSTOM_IMPLEMENTATION:'custom-implementation', __providers:{},
    item:(data,desc)=>({data,desc}),
    registerProvider:(id,provider)=>{ if(typeof provider?.search!=='function'||typeof provider?.download!=='function') throw new TypeError('Subtitle provider needs search and download');
      iina.subtitle.__providers[id]=provider; host('subtitle.register',[id]); } };
  globalThis.__velinSearchSubtitle = id => {
    const provider=iina.subtitle.__providers[id]; if(!provider) throw new Error('Subtitle provider not found');
    Promise.resolve(provider.search()).then(results=>host('subtitle.results',[id,results]),error=>host('subtitle.error',[id,String(error)]));
  };
  globalThis.__velinDownloadSubtitle = (id,data) => {
    const provider=iina.subtitle.__providers[id]; if(!provider) throw new Error('Subtitle provider not found');
    Promise.resolve(provider.download(data)).then(paths=>host('subtitle.downloaded',[id,paths]),error=>host('subtitle.error',[id,String(error)]));
  };
  globalThis.setTimeout=(callback,delay=0,...args)=>{ const id=next(); timers.set(id,{callback,args,at:Date.now()+Math.max(0,delay),interval:0}); return id; };
  globalThis.setInterval=(callback,delay=0,...args)=>{ const id=next(); const interval=Math.max(10,delay); timers.set(id,{callback,args,at:Date.now()+interval,interval}); return id; };
  globalThis.clearTimeout=globalThis.clearInterval=id=>timers.delete(String(id));
  globalThis.__velinTick=()=>{ const now=Date.now(); for(const [id,timer] of [...timers]) if(timer.at<=now) {
    if(timer.interval) timer.at=now+timer.interval; else timers.delete(id); try { timer.callback(...timer.args); } catch(error){ iina.console.error(String(error)); }
  } };
  const load = (specifier,parent='') => {
    if(specifier==='iina') return iina;
    const module=host('module.read',[specifier,parent]);
    if(modules.has(module.path)) return modules.get(module.path).exports;
    const record={exports:{}}; modules.set(module.path,record);
    const folder=module.path.replace(/[\\/][^\\/]*$/,'');
    new Function('require','module','exports','__filename','__dirname',module.source+'\n//# sourceURL='+module.path)
      (name=>load(name,folder),record,record.exports,module.path,folder);
    return record.exports;
  };
  globalThis.require=specifier=>load(specifier);
  globalThis.__velinLoad=entry=>load(entry);
  Object.freeze(iina.mpv);
})();
)VELINJS";
} // namespace velin
