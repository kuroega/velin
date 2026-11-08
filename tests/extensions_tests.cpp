// SPDX-License-Identifier: GPL-3.0-or-later
#include "velin/extensions.hpp"
#include "velin/plugin_url_policy.hpp"
#include "miniz/miniz.h"
#include <chrono>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void put(const std::filesystem::path& path,const std::string& text){std::filesystem::create_directories(path.parent_path());std::ofstream out(path,std::ios::binary);out<<text;}
template<typename Action>void rejects(Action action,const char* message){bool rejected=false;try{action();}catch(const std::exception&){rejected=true;}check(rejected,message);}
void makeZip(const std::filesystem::path& path,const std::vector<std::pair<std::string,std::string>>& files){
    mz_zip_archive zip{};check(mz_zip_writer_init_heap(&zip,0,0)!=0,"ZIP initialization");
    for(const auto&[name,text]:files)check(mz_zip_writer_add_mem(&zip,name.c_str(),text.data(),text.size(),0)!=0,"ZIP entry");
    void* data=nullptr;size_t size=0;check(mz_zip_writer_finalize_heap_archive(&zip,&data,&size)!=0,"ZIP finish");
    std::ofstream output(path,std::ios::binary);output.write(static_cast<const char*>(data),static_cast<std::streamsize>(size));
    mz_free(data);mz_zip_writer_end(&zip);
}
velin::Json manifest(const std::string& id,const velin::Json& permissions=velin::Json::array()){
    return {{"name","Integration Plugin"},{"identifier",id},{"version","1.0.0"},{"author",{{"name","Velin tests"}}},
        {"entry","main.js"},{"permissions",permissions},{"allowedDomains",velin::Json::array({"127.0.0.1"})},{"preferenceDefaults",{{"defaultKey",17}}}};
}
}
int main(int argc,char** argv){
    if(argc==2 && std::string(argv[1])=="--fixture-cwd"){std::cout<<std::filesystem::current_path().string();return 0;}
    if(argc>=2 && std::string(argv[1])=="--fixture-stream") {
        
#ifdef _WIN32
        _setmode(_fileno(stdout),_O_BINARY);_setmode(_fileno(stderr),_O_BINARY);
#endif

        std::cout<<"first:\xe6"<<std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        std::cout<<"\xb8\xac\xe8\xa9\xa6\n"<<std::flush;
        std::cerr<<"separate stderr\n"<<std::flush;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(argc>2 && !std::filesystem::exists(argv[2]) && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return argc>2 && std::filesystem::exists(argv[2])?7:9;
    }
    if(argc==2 && std::string(argv[1])=="--fixture-flood"){const std::string chunk(65536,'x');for(int i=0;i<160;++i)std::cout<<chunk;return 0;}
    if(argc==2 && std::string(argv[1])=="--fixture-invalid"){std::cout<<"\xff";return 0;}
    if(argc==2 && std::string(argv[1])=="--fixture-wait"){std::this_thread::sleep_for(std::chrono::seconds(60));return 0;}
    const auto root=std::filesystem::current_path()/"extensions-test-data";
    try{
        std::filesystem::remove_all(root);std::filesystem::create_directories(root);
        velin::MediaEngine engine;velin::OnlineServices services;
        std::vector<velin::Json> nativeCalls;std::vector<std::string> log;
        velin::PluginInfo lastUiPlugin;
        velin::PluginHost* activeHost=nullptr;
        bool reloadRequested=false;
        velin::ExtensionCallbacks callbacks;
        callbacks.log=[&](const auto&,const auto& text){log.push_back(text);std::cerr<<text<<"\n";};
        callbacks.ui=[&](const auto& plugin,const auto& operation,const auto& arguments)->velin::Json{
            lastUiPlugin=plugin;
            nativeCalls.push_back({{"plugin",plugin.identifier},{"operation",operation},{"arguments",arguments}});
            if(operation=="osd" && arguments.at(0)=="reload-during-callback") {
                check(activeHost && activeHost->busy(),"Reload requested while JavaScript is active");
                activeHost->reload();reloadRequested=true;
                activeHost->emit("iina.fixture-deferred",velin::Json::array({7}));
            }
            if(operation=="core.window.get")return arguments.at(0)=="fullscreen"?velin::Json(false):velin::Json(true);
            if(operation=="global.createPlayerInstance")return 42;
            if(operation=="global.getLabel")return "test-player";
            return true;
        };
        velin::PluginHost host(root/"data",engine,services,callbacks);
        activeHost=&host;
        const std::string id="org.velin.integration";
        const auto source=root/"source";
        const std::set<std::string> permissions{"show-osd","show-alert","network-request","video-overlay","file-system"};
        put(source/"Info.json",manifest(id,permissions).dump());
        put(source/"helper.js","module.exports = require('./value.json').answer;");
        put(source/"value.json","{\"answer\":42}");
        put(source/"view.html","<!doctype html><title>Plugin view</title>");
        put(source/"main.js",R"JS(
globalThis.fixture={module:require('./helper'),menu:0,input:0,events:0,timer:0};
iina.console.log('plugin loaded');
iina.event.on('mpv.pause.changed',value=>{fixture.pause=value;fixture.events++});
iina.menu.addItem(iina.menu.item('Test command',()=>fixture.menu++,{keyBinding:'Ctrl+K'}));
iina.input.onKeyDown('Ctrl+K',()=>{fixture.input++;return true},iina.input.PRIORITY_HIGH);
iina.input.onKeyUp('Ctrl+K',args=>{fixture.keyUp=args;return true});
for(const event of ['Down','Up','Drag']) iina.input['onMouse'+event](iina.input.MOUSE,args=>{fixture['mouse'+event]=args;return true});
iina.playlist.registerMenuItemBuilder(files=>[
  iina.menu.item('Selected '+files.length,()=>fixture.selected=files),
  iina.menu.item('Disabled',()=>fixture.disabled=true,{enabled:false}),iina.menu.separator()
]);
iina.event.on('iina.fixture-deferred',value=>fixture.deferred=value);
iina.preferences.set('stored','persisted');iina.preferences.sync();
iina.file.write('@data/state.txt','Unicode: \u6e2c\u8a66');fixture.file=iina.file.read('@data/state.txt');
iina.core.osd('Hello');fixture.fullscreen=iina.core.window.fullscreen;
iina.overlay.simpleMode();iina.overlay.setContent('<p>Overlay</p>');iina.overlay.show();
iina.sidebar.loadFile('view.html');iina.sidebar.onMessage('reply',value=>fixture.web=value);
iina.standaloneWindow.setFrame(300,200,10,20);
iina.global.onMessage('reply',(value,source)=>{fixture.global=value;fixture.globalSource=source;});
fixture.child=iina.global.createPlayerInstance({label:'child'});fixture.label=iina.global.getLabel();
iina.global.postMessage(42,'hello',{answer:42});
iina.subtitle.registerProvider('test',{search:async()=>[iina.subtitle.item({id:1},{name:'Test subtitle'})],download:async()=>['@data/state.txt']});
setTimeout(()=>fixture.timer++,1);
)JS");
        rejects([&]{host.install(source,{});},"Installation must require explicit permissions");
        auto info=host.install(source,permissions);check(info.identifier==id,"Manifest identifier");
        auto discovered=host.discover();check(discovered.size()==1&&discovered[0].running,"Installed plugin must run");
        const auto editedManifest=manifest(id,permissions);
        auto changedManifest=editedManifest;changedManifest["allowedDomains"]=velin::Json::array({"*"});
        put(info.directory/"Info.json",changedManifest.dump());
        host.evaluate(id,"iina.standaloneWindow.open()");
        check(lastUiPlugin.domains==std::vector<std::string>{"127.0.0.1"},"UI callback uses the approved running-instance manifest snapshot");
        put(info.directory/"Info.json",editedManifest.dump());
        check(!velin::pluginWebViewAllowsUrl("https://velin.plugin:password@evil.test/",{"*"}),"WebView rejects userinfo even with wildcard domains");
        check(!velin::pluginWebViewAllowsUrl("https:///empty",{"*"}),"WebView rejects empty hosts even with wildcard domains");
        check(velin::pluginWebViewAllowsUrl("https://velin.plugin/view.html",{}),"WebView permits local package pages without network domains");
        check(!velin::pluginWebViewHasEnforceableNetworkPolicy({}),"WebView rejects missing network permission");
        check(!velin::pluginWebViewHasEnforceableNetworkPolicy({"127.0.0.1","*.example.org"}),"WebView rejects restricted network grants");
        check(velin::pluginWebViewHasEnforceableNetworkPolicy({"*"}),"WebView accepts an explicit unrestricted network grant");
        check(host.evaluate(id,"fixture.module")==42,"CommonJS relative and JSON modules");
        check(host.evaluate(id,R"JS((()=>{const f=iina.file.handle('@data/binary.bin','write');f.write(new Uint8Array([0,255,42,10]));if(f.offset()!==4)throw Error('offset');f.seekTo(2);f.write(new Uint8Array([7]));f.seekTo(0);const bytes=Array.from(f.readToEnd());f.close();return bytes;})())JS")==velin::Json::array({0,255,7,10}),"Binary file handle offsets, writes and reads");
        rejects([&]{host.evaluate(id,"(()=>{const f=iina.file.handle('@data/binary.bin','read');try{f.write([1]);}finally{f.close();}})()");},"Read-only handles reject writes");
        check(host.evaluate(id,"iina.preferences.get('defaultKey')")==17,"Preference defaults");
        check(host.evaluate(id,"iina.file.write('@data/nested/leaf.txt','nested');iina.file.list('@data',{includeSubDir:true}).some(f=>f.filename==='leaf.txt')")==true,"Recursive directory listing");
        put(root/"data"/"plugins"/".data"/id/"utf16.txt",std::string("\xff\xfeH\0i\0",6));
        check(host.evaluate(id,"iina.file.read('@data/utf16.txt',{encoding:'utf-16'})")=="Hi","UTF-16 BOM decoding");
        rejects([&]{host.evaluate(id,"iina.file.read('@data/utf16.txt',{encoding:'unknown'})");},"Unsupported text encodings reject explicitly");
        check(host.evaluate(id,"iina.file.list('@data/nested')[0].path")=="/leaf.txt","Directory paths are relative to the requested root like IINA");
        check(host.evaluate(id,"iina.file.list('@data',{includeSubDir:true}).find(f=>f.filename==='leaf.txt').path")=="/nested/leaf.txt","Recursive paths preserve relative subdirectories");
        put(root/"data"/"plugins"/".data"/id/"latin.txt",std::string("caf\xe9",4));
        check(host.evaluate(id,"iina.file.read('@data/latin.txt',{encoding:'isoLatin1'})")=="caf\xc3\xa9","ISO Latin-1 preserves accented characters");
        put(root/"data"/"plugins"/".data"/id/"cp1252.txt",std::string("\x80",1));
        check(host.evaluate(id,"iina.file.read('@data/cp1252.txt',{encoding:'windowsCP1252'})")=="\xe2\x82\xac","Windows-1252 is distinct from Latin-1");
        put(root/"data"/"plugins"/".data"/id/"big5.txt",std::string("\xb4\xfa\xb8\xd5",4));
        check(host.evaluate(id,"iina.file.read('@data/big5.txt',{encoding:'big5'})")=="\xe6\xb8\xac\xe8\xa9\xa6","Big5 text decodes to Unicode");
        put(root/"data"/"plugins"/".data"/id/"utf32.txt",std::string("\xff\xfe\0\0\0\xf6\1\0",8));
        check(host.evaluate(id,"iina.file.read('@data/utf32.txt',{encoding:'utf32LittleEndian'})")=="\xf0\x9f\x98\x80","UTF-32 preserves supplementary characters");
        rejects([&]{host.evaluate(id,"iina.file.read('@data/latin.txt',{encoding:'ascii'})");},"ASCII rejects non-ASCII bytes");


        rejects([&]{host.evaluate(id,"iina.http.get('http://localhost:1/')");},"Allowed domains reject a different hostname before networking");
        host.evaluate(id,"globalThis.processDone=false;iina.utils.exec("+velin::Json(std::filesystem::absolute(argv[0]).string()).dump()+",['--fixture-cwd'],'@data').then(result=>{globalThis.processResult=result;processDone=true},error=>{globalThis.processError=String(error);processDone=true});");
        const auto processDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(host.evaluate(id,"processDone")!=true&&std::chrono::steady_clock::now()<processDeadline){host.poll();std::this_thread::sleep_for(std::chrono::milliseconds(10));}
        const auto expectedDirectory=std::filesystem::weakly_canonical(root/"data"/"plugins"/".data"/id).string();
        check(host.evaluate(id,"processResult.stdout.trim()")==expectedDirectory,"Process executes in requested authorized working directory");
        const auto executable=std::filesystem::absolute(argv[0]).string();
        const auto gate=(root/"data"/"plugins"/".data"/id/"stream-gate.txt").string();
        host.evaluate(id,"globalThis.streamDone=false;globalThis.stdoutChunks='';globalThis.stderrChunks='';iina.utils.exec("+velin::Json(executable).dump()+",['--fixture-stream',"+velin::Json(gate).dump()+"],null,text=>{stdoutChunks+=text;iina.file.write('@data/stream-gate.txt','ready');},text=>stderrChunks+=text).then(result=>{globalThis.streamResult=result;streamDone=true},error=>{globalThis.streamError=String(error);streamDone=true});");
        const auto streamDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
        while(host.evaluate(id,"streamDone")!=true&&std::chrono::steady_clock::now()<streamDeadline){host.poll();std::this_thread::sleep_for(std::chrono::milliseconds(10));}
        check(host.evaluate(id,"streamResult.status")==7,"Live stdout callback unblocks child before its nonzero exit");
        check(host.evaluate(id,"streamResult.stdout")=="first:\xe6\xb8\xac\xe8\xa9\xa6\n"&&host.evaluate(id,"stdoutChunks===streamResult.stdout")==true,"Split UTF-8 sequences reach live stdout callback intact");
        check(host.evaluate(id,"streamResult.stderr")=="separate stderr\n"&&host.evaluate(id,"stderrChunks===streamResult.stderr")==true,"stderr has an independent live callback and final result");
        host.evaluate(id,"globalThis.invalidDone=false;iina.utils.exec("+velin::Json(executable).dump()+",['--fixture-invalid']).then(()=>invalidDone=true,error=>{globalThis.invalidError=String(error);invalidDone=true});");
        const auto invalidDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(host.evaluate(id,"invalidDone")!=true&&std::chrono::steady_clock::now()<invalidDeadline){host.poll();std::this_thread::sleep_for(std::chrono::milliseconds(10));}
        check(host.evaluate(id,"invalidError.includes('UTF-8')")==true,"Invalid process text rejects without losing Promise completion");
        host.evaluate(id,"globalThis.floodDone=false;iina.utils.exec("+velin::Json(executable).dump()+",['--fixture-flood']).then(()=>floodDone=true,error=>{globalThis.floodError=String(error);floodDone=true});");
        const auto floodDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        while(host.evaluate(id,"floodDone")!=true&&std::chrono::steady_clock::now()<floodDeadline){host.poll();std::this_thread::sleep_for(std::chrono::milliseconds(10));}
        check(host.evaluate(id,"floodError.includes('8 MiB')")==true,"Output limit terminates a helper that exceeds the combined stream budget");

        const auto cancelledProcess=services.submit("process.exec",{{"executable",executable},{"args",{"--fixture-wait"}}},true);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));services.cancel(cancelledProcess);
        bool processCancelled=false;
        const auto cancelDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!processCancelled && std::chrono::steady_clock::now()<cancelDeadline){for(const auto& result:services.pollFor({cancelledProcess}))if(result.operation=="process.exec")processCancelled=result.cancelled;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
        check(processCancelled,"Cancelling a running helper terminates it and completes the task");



        check(host.evaluate(id,"fixture.file")=="Unicode: \\u6e2c\\u8a66"||host.evaluate(id,"fixture.file")=="Unicode: \u6e2c\u8a66","Plugin local file round trip");
        host.emit("mpv.pause.changed",velin::Json::array({true}));
        check(host.evaluate(id,"fixture.pause")==true&&host.evaluate(id,"fixture.events")==1,"Event callback");
        auto menu=host.menuItems();check(menu.size()==1&&menu[0].title=="Test command","Native menu registration");
        check(host.invokeMenu(id,menu[0].identifier)&&host.evaluate(id,"fixture.menu")==1,"Menu callback");
        host.evaluate(id,"const parent=iina.menu.item('Parent',null,{enabled:false});parent.addSubMenuItem(iina.menu.item('Child',()=>fixture.childInvoked=true));iina.menu.addItem(parent);");
        menu=host.menuItems();
        check(menu.size()==3&&menu[1].depth==0&&menu[2].depth==1&&!menu[2].enabled,"Menu hierarchy and inherited disabled state");
        check(!host.invokeMenu(id,menu[2].identifier),"Disabled parent prevents child invocation");

        check(host.input("keyDown","Ctrl+K")&&host.evaluate(id,"fixture.input")==1,"Input callback");
        check(host.input("keyUp","Ctrl+K",{{"key",75}})&&host.evaluate(id,"fixture.keyUp.key")==75,"Key-up callback arguments");
        for(const auto& event:{"Down","Up","Drag"}) {
            check(host.input(std::string("mouse")+event,"MBTN_LEFT",{{"x",23},{"y",42}}),"Mouse callback consumes event");
            check(host.evaluate(id,std::string("fixture.mouse")+event+".x")==23,"Mouse coordinates preserved");
        }
        auto contextMenu=host.playlistMenuItems({"one.mkv","two.mkv"});
        check(contextMenu.size()==3 && contextMenu[0].title=="Selected 2","Playlist menu builder receives selection");
        check(host.invokeMenu(id,contextMenu[0].identifier)&&host.evaluate(id,"fixture.selected")==velin::Json::array({"one.mkv","two.mkv"}),"Playlist context action closure");
        check(!host.invokeMenu(id,contextMenu[1].identifier)&&!host.invokeMenu(id,contextMenu[2].identifier),"Disabled context items and separators cannot run");
        const auto oldContextId=contextMenu[0].identifier;
        host.playlistMenuItems({"three.mkv"});
        check(!host.invokeMenu(id,oldContextId),"Previous context menu actions expire");
        host.evaluate(id,"iina.playlist.registerMenuItemBuilder(()=>{const p=iina.menu.item('Group',null,{enabled:false});p.addSubMenuItem(iina.menu.item('Nested',()=>fixture.childInvoked=true));return [p];});");
        contextMenu=host.playlistMenuItems({"one.mkv"});
        check(contextMenu.size()==2&&contextMenu[1].depth==1&&!contextMenu[1].enabled&&!host.invokeMenu(id,contextMenu[1].identifier),"Playlist hierarchy preserves parent disabled state");

        check(host.evaluate(id,"iina.core.osd('reload-during-callback');fixture.afterReload=42;fixture.afterReload")==42&&reloadRequested,"Reentrant reload preserves active JavaScript stack");
        check(host.evaluate(id,"fixture.afterReload")==42,"Reload is deferred until poll");
        host.poll();
        check(host.evaluate(id,"typeof fixture.afterReload")=="undefined","Poll replaces runtime after callback returns");
        host.emit("iina.fixture-deferred",velin::Json::array({9}));
        check(host.evaluate(id,"fixture.deferred")==9,"Reloaded runtime receives new events");
        host.receiveWebMessage(id,"sidebar","reply",{{"answer",7}});check(host.evaluate(id,"fixture.web.answer")==7,"WebView message callback");
        host.receiveGlobalMessage(id,"reply",{{"answer",9}},"child-player");check(host.evaluate(id,"fixture.global.answer")==9,"Global message callback");
        check(host.evaluate(id,"fixture.child")==42&&host.evaluate(id,"fixture.label")=="test-player","Global native dispatch");
        host.searchSubtitleProvider(id,"test");host.poll();
        check(std::any_of(nativeCalls.begin(),nativeCalls.end(),[](const auto& call){return call["operation"]=="subtitle.results";}),"Subtitle provider search Promise");
        host.downloadSubtitleProvider(id,"test",{{"id",1}});host.poll();
        check(std::any_of(nativeCalls.begin(),nativeCalls.end(),[](const auto& call){return call["operation"]=="subtitle.downloaded";}),"Subtitle provider download Promise");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));host.poll();check(host.evaluate(id,"fixture.timer")==1,"JavaScript timer");
        rejects([&]{host.evaluate(id,"iina.file.write('@data/../../escape.txt','bad')");},"Path traversal must be rejected");
        rejects([&]{host.evaluate(id,"while(true){}");},"Infinite script must be interrupted");
        check(host.evaluate(id,"1+1")==2,"Runtime must recover after interruption");
        host.setEnabled(id,false);check(!host.discover()[0].running,"Disable must stop runtime");
        host.setEnabled(id,true);check(host.evaluate(id,"iina.preferences.get('stored')")=="persisted","Plugin preferences must persist");
        const auto zip=root/"plugin.iinaplgz";
        makeZip(zip,{{"package/Info.json",manifest("org.velin.zip").dump()},{"package/main.js","globalThis.installedFromZip=true;"}});
        host.install(zip,{});check(host.evaluate("org.velin.zip","installedFromZip")==true,"ZIP plugin installation");
        rejects([&]{host.evaluate("org.velin.zip","iina.core.osd('denied')");},"OSD permission must be enforced");
        rejects([&]{host.evaluate("org.velin.zip","iina.overlay.show()");},"Overlay permission must be enforced");
        rejects([&]{host.evaluate("org.velin.zip","__velinHost('async.start',['http.request',{url:'http://127.0.0.1/'}])");},"Native bridge must enforce network permissions");
        rejects([&]{host.evaluate("org.velin.zip","iina.file.read('C:/Windows/win.ini')");},"Absolute file access must require permission");
        const auto malicious=root/"malicious.zip";makeZip(malicious,{{"../escape.js","bad"}});
        rejects([&]{host.inspect(malicious);},"ZIP traversal must be rejected");
        check(!std::filesystem::exists(root/"data"/"escape.js"),"ZIP must not escape staging");
        host.uninstall("org.velin.zip");check(host.discover().size()==1,"Uninstall");
        auto globalManifest=manifest("org.velin.global-only");globalManifest.erase("entry");globalManifest["globalEntry"]="global.js";
        const auto globalSource=root/"global-source";
        put(globalSource/"Info.json",globalManifest.dump());put(globalSource/"global.js","globalThis.onlyGlobal=42;");
        host.install(globalSource,{});
        check(host.evaluate("org.velin.global-only","onlyGlobal")==42,"Global-only manifest starts without a player entry");
        host.setGlobalEntriesEnabled(false);host.reload();
        check(host.evaluate("org.velin.global-only","typeof onlyGlobal")=="undefined","Child players suppress global-only entry execution");
        host.setGlobalEntriesEnabled(true);host.uninstall("org.velin.global-only");

        std::cout<<"PASS JavaScript execution, modules, events, menus, input, preferences, files, UI namespaces, global, subtitles, timers, interruption, packages, permissions\n";
        if(argc>1){
            const std::string base=argv[1];
            host.evaluate(id,"globalThis.httpDone=false;iina.http.get("+velin::Json(base+"/json").dump()+").then(response=>{globalThis.httpResult=response.data;httpDone=true},error=>{globalThis.httpError=String(error);httpDone=true});");
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(host.evaluate(id,"httpDone")!=true&&std::chrono::steady_clock::now()<deadline){host.poll();std::this_thread::sleep_for(std::chrono::milliseconds(10));}
            check(host.evaluate(id,"httpResult.answer")==42,"Async plugin HTTP Promise");
            host.evaluate(id,"globalThis.redirectDone=false;iina.http.get("+velin::Json(base+"/redirect-blocked").dump()+").then(()=>{globalThis.redirectAllowed=true;redirectDone=true},error=>{globalThis.redirectError=String(error);redirectDone=true});");
            const auto redirectDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(host.evaluate(id,"redirectDone")!=true&&std::chrono::steady_clock::now()<redirectDeadline){host.poll();std::this_thread::sleep_for(std::chrono::milliseconds(10));}
            check(host.evaluate(id,"typeof redirectAllowed")=="undefined"&&host.evaluate(id,"redirectError.includes('not allowed')")==true,"Redirect target must obey manifest domain permissions");

            host.evaluate(id,"globalThis.rpcDone=false;iina.http.xmlrpc("+velin::Json(base+"/rpc").dump()+").call('test',[{hello:'world'},42]).then(value=>{globalThis.rpcResult=value;rpcDone=true},error=>{globalThis.rpcError=String(error);rpcDone=true});");
            const auto rpcDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(host.evaluate(id,"rpcDone")!=true&&std::chrono::steady_clock::now()<rpcDeadline){host.poll();std::this_thread::sleep_for(std::chrono::milliseconds(10));}
            check(host.evaluate(id,"rpcResult.answer")==42,"XML-RPC real HTTP request and typed response parsing");
            velin::OnlineConfiguration config;config.openSubtitlesApiKey="test-key";config.assrtToken="test-token";config.openSubtitlesBase=base;config.assrtBase=base;config.pluginCatalog=base+"/catalog";services.configure(config);
            std::set<std::uint64_t> ids;
            ids.insert(services.submit("opensubtitles.search",{{"query","test"},{"languages","en"}}));
            ids.insert(services.submit("assrt.search",{{"query","test"}}));
            ids.insert(services.submit("plugins.catalog"));
            ids.insert(services.submit("opensubtitles.login",{{"username","test"},{"password","test"}}));
            ids.insert(services.submit("opensubtitles.download",{{"file_id",7},{"destination",(root/"download.srt").string()}}));
            ids.insert(services.submit("assrt.download",{{"id",9},{"destination",(root/"assrt.srt").string()}}));
            ids.insert(services.submit("download.verified",{{"url",base+"/package"},{"sha256","5b365b709602bee45e8db24117a4f631efd738927cd9e85e95f765d6d58d909d"},{"destination",(root/"verified.bin").string()}}));
            ids.insert(services.submit("updates.check",{{"url",base+"/update"},{"currentVersion","0.1.0"}}));
            const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(!ids.empty()&&std::chrono::steady_clock::now()<end){for(const auto& result:services.pollFor(ids)){check(result.error.empty(),result.error.c_str());ids.erase(result.id);}std::this_thread::sleep_for(std::chrono::milliseconds(10));}
            check(ids.empty(),"Online service mock responses must complete");
            check(std::filesystem::is_regular_file(root/"download.srt")&&std::filesystem::is_regular_file(root/"assrt.srt"),"Subtitle downloads must be written");
            const auto cancelled=services.submit("http.request",{{"url",base+"/slow"}});services.cancel(cancelled);
            bool cancellation=false;for(int i=0;i<100&&!cancellation;++i){for(const auto& result:services.pollFor({cancelled}))cancellation=result.cancelled;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
            check(cancellation,"Network cancellation");
            std::cout<<"PASS asynchronous HTTP, OpenSubtitles, Assrt, catalog mock service contracts, cancellation\n";
        }
        host.uninstall(id);
        std::filesystem::remove_all(root);
        std::cout<<"All extension checks passed.\n";return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
