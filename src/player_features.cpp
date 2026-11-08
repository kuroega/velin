// SPDX-License-Identifier: GPL-3.0-or-later
#include "velin/player_window.hpp"
#include "velin/player_options.hpp"
#include "velin/preferences_dialog.hpp"
#include "velin/plugin_url_policy.hpp"
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincred.h>
#include <bcrypt.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace velin {
namespace {
std::string utf8Path(const std::filesystem::path& path) { return toUtf8(path.wstring()); }
std::vector<std::string> tokenize(std::string_view command) {
    std::vector<std::string> tokens; std::string token; char quote=0; bool escape=false,started=false;
    for (char c:command) {
        if (escape) { token+=c; escape=false; started=true; }
        else if (c=='\\' && quote) escape=true;
        else if (quote) { if (c==quote) quote=0; else token+=c; started=true; }
        else if (c=='\'' || c=='"') { quote=c; started=true; }
        else if (c==' ' || c=='\t' || c=='\n') { if (started) { tokens.push_back(std::move(token)); token.clear(); started=false; } }
        else { token+=c; started=true; }
    }
    if (escape) token+='\\';
    if (quote) throw std::runtime_error("The command contains an unterminated quote.");
    if (started) tokens.push_back(std::move(token));
    return tokens;
}
std::wstring textArgument(const Json& args,size_t index,std::wstring fallback={}) {
    return args.is_array() && index<args.size() && args[index].is_string()?fromUtf8(args[index].get<std::string>()):fallback;
}
std::string credential(const std::wstring& service) {
    PCREDENTIALW value=nullptr; std::string result;
    if (CredReadW(service.c_str(),CRED_TYPE_GENERIC,0,&value)) {
        result.assign(reinterpret_cast<const char*>(value->CredentialBlob),value->CredentialBlobSize); CredFree(value);
    }
    return result;
}
bool storeCredential(const std::wstring& service,const std::wstring& username,const std::string& password) {
    CREDENTIALW value{}; value.Type=CRED_TYPE_GENERIC; value.TargetName=const_cast<LPWSTR>(service.c_str());
    value.CredentialBlobSize=static_cast<DWORD>(password.size()); value.CredentialBlob=reinterpret_cast<LPBYTE>(const_cast<char*>(password.data()));
    value.Persist=CRED_PERSIST_LOCAL_MACHINE; value.UserName=const_cast<LPWSTR>(username.c_str());
    return CredWriteW(&value,0)!=FALSE;
}
bool launchPlayer(const std::vector<std::wstring>& arguments,DWORD* processId=nullptr) {
    const auto executable=executableDirectory()/L"velin.exe";
    auto command=quoteWindowsArgument(executable.wstring());
    for (const auto& argument:arguments) command+=L" "+quoteWindowsArgument(argument);
    STARTUPINFOW startup{}; startup.cb=sizeof(startup); PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,executableDirectory().c_str(),&startup,&process)) return false;
    if (processId) *processId=process.dwProcessId;
    CloseHandle(process.hThread); CloseHandle(process.hProcess); return true;
}
constexpr ULONG_PTR pluginMessageMagic=0x5647504d;
std::string randomToken() {
    unsigned char bytes[32];if(BCryptGenRandom(nullptr,bytes,sizeof(bytes),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)throw std::runtime_error("Cannot create a plugin message token.");
    std::string result;constexpr char hex[]="0123456789abcdef";for(auto byte:bytes){result+=hex[byte>>4];result+=hex[byte&15];}return result;
}
bool sendPluginMessage(HWND sender,DWORD process,const Json& message) {
    struct Target { DWORD process; HWND window=nullptr; } target{process};
    EnumWindows([](HWND window,LPARAM context)->BOOL {
        auto& target=*reinterpret_cast<Target*>(context);DWORD id=0;GetWindowThreadProcessId(window,&id);wchar_t name[80]{};GetClassNameW(window,name,80);
        if(id==target.process && std::wstring_view(name)==L"Velin.Player"){target.window=window;return FALSE;}return TRUE;
    },reinterpret_cast<LPARAM>(&target));
    if(!target.window)return false;
    const auto text=message.dump();if(text.size()>1024*1024)return false;
    COPYDATASTRUCT data{pluginMessageMagic,static_cast<DWORD>(text.size()),const_cast<char*>(text.data())};DWORD_PTR response=0;
    return SendMessageTimeoutW(target.window,WM_COPYDATA,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&data),SMTO_ABORTIFHUNG,2000,&response)!=0 && response!=0;
}
}

bool PlayerWindow::receivePluginMessage(WPARAM sender,LPARAM payload) {
    const auto* data=reinterpret_cast<const COPYDATASTRUCT*>(payload);
    if(!data || data->dwData!=pluginMessageMagic || !data->lpData || data->cbData>1024*1024)return false;
    const auto message=Json::parse(static_cast<const char*>(data->lpData),static_cast<const char*>(data->lpData)+data->cbData,nullptr,false);
    if(!message.is_object() || !message.contains("plugin") || !message["plugin"].is_string() || !message.contains("token") || !message["token"].is_string())return false;
    DWORD process=0;GetWindowThreadProcessId(reinterpret_cast<HWND>(sender),&process);
    const auto plugin=message["plugin"].get<std::string>(),token=message["token"].get<std::string>();
    bool approved=process!=0 && process==pluginParent_ && plugin==pluginOwner_ && token==pluginSecret_;
    if(auto found=pluginPeers_.find(process);found!=pluginPeers_.end())approved=found->second.first==plugin && found->second.second==token;
    if(!approved || !plugins_ || !message.contains("name") || !message["name"].is_string())return false;
    try{plugins_->receiveGlobalMessage(plugin,message["name"].get<std::string>(),message.value("data",Json{}),process);return true;}
    catch(const std::exception& error){logLines_.push_back(error.what());return false;}
}

bool PlayerWindow::initializeEngine() {
    EngineConfiguration config;
    config.options=playbackOptions(preferences_,dataDirectory_);
    config.configDirectory=preferences_.mpvConfigDirectory.empty()?dataDirectory_/L"mpv":std::filesystem::path(fromUtf8(preferences_.mpvConfigDirectory));
    config.loadUserConfig=preferences_.loadMpvConfig; config.loadScripts=preferences_.scriptsEnabled; config.onlineMedia=preferences_.youtubeDlEnabled;
    config.ytDlpExecutable=preferences_.youtubeDlPath.empty()?executableDirectory()/L"yt-dlp.exe":std::filesystem::path(fromUtf8(preferences_.youtubeDlPath));
    if (!engine_.initialize(video_,window_,engineMessage,config)) { notice(fromUtf8(engine_.error()),true); return false; }
    engine_.setEventCallback([this](const std::string& name,const std::string& arguments) {
        if (closing_) return;
        auto data=Json::parse(arguments,nullptr,false);
        if (data.is_discarded()) data=Json::array();
        if (name=="mpv.log-message") {
            logLines_.push_back(arguments);
            if (logLines_.size()>5000) logLines_.erase(logLines_.begin(),logLines_.begin()+500);
        }
        if (plugins_ && preferences_.pluginsEnabled) plugins_->emit(name,data);
    });
    return true;
}

void PlayerWindow::applyPreferences(const Preferences& before) {
    const auto old=playbackOptions(before,dataDirectory_),next=playbackOptions(preferences_,dataDirectory_);
    const bool restart=std::any_of(next.begin(),next.end(),[&](const auto& option) {
        const auto found=old.find(option.first);
        return isInitializationOption(option.first) && (found==old.end() || found->second!=option.second);
    });
    if (restart && engine_.ready()) {
        persistPlayback();
        const auto session=playlists_.loadSession();
        const auto state=engine_.state();
        engine_.shutdown();
        if (initializeEngine() && !session.entries.empty()) {
            std::vector<std::string> paths; for (const auto& entry:session.entries) paths.push_back(entry.path);
            engine_.open(paths); engine_.setProperty("playlist-pos",std::to_string(std::max<std::int64_t>(0,session.currentIndex)));
            pendingSeek_=state.position; pendingPause_=state.paused; pendingResumePath_=state.path;
        }
    } else if (engine_.ready()) {
        for (const auto& [name,value]:next) {
            const auto previous=old.find(name);
            if (!isInitializationOption(name) && (previous==old.end() || previous->second!=value)) engine_.setProperty(name,value);
        }
    }
    showPlaylist_=preferences_.showPlaylist;
    if (musicMode_!=preferences_.musicMode) toggleMusicMode();
    applyTheme();
    SetWindowPos(window_,(pip_?preferences_.pipAlwaysOnTop:preferences_.alwaysOnTop)?HWND_TOPMOST:HWND_NOTOPMOST,
        0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    if (online_) {
        OnlineConfiguration config;
        if (preferences_.subtitleProvider=="assrt") config.assrtToken=preferences_.subtitleProviderToken;
        else { config.openSubtitlesApiKey=preferences_.subtitleProviderApiKey; config.openSubtitlesToken=preferences_.subtitleProviderToken; }
        config.ytDlpExecutable=preferences_.youtubeDlPath.empty()?executableDirectory()/L"yt-dlp.exe":std::filesystem::path(fromUtf8(preferences_.youtubeDlPath));
        online_->configure(std::move(config));
    }
    if (plugins_ && before.pluginsEnabled!=preferences_.pluginsEnabled) {
        if (preferences_.pluginsEnabled) plugins_->reload();
        else { for (const auto& plugin:plugins_->discover()) plugins_->setEnabled(plugin.identifier,false); webviews_.clear(); }
    }
    layoutVideo(); updateAccessibility(); InvalidateRect(window_,nullptr,FALSE);
}

void PlayerWindow::showPreferences(std::string section) {
    auto previous=preferences_;
    showPreferencesDialog(window_,preferences_,settings_,[this,previous](const Preferences&) mutable { applyPreferences(previous); previous=preferences_; },std::move(section));
}

void PlayerWindow::persistPlayback(bool countPlay) {
    const auto& state=engine_.state();
    if (preferences_.recordHistory && state.loaded && !state.path.empty()) {
        const auto now=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        if (!history_.record({state.path,state.title,now,state.position,state.duration,0,false},countPlay))
            logLines_.push_back("History: "+history_.error());
    }
    if (preferences_.restorePlaylist) {
        PlaylistDocument session; session.currentIndex=state.playlistPosition; session.position=state.position;
        for (const auto& item:state.playlist) session.entries.push_back({item.filename,item.title,-1});
        if (!playlists_.saveSession(session)) logLines_.push_back("Session: "+playlists_.error());
    }
    lastPersist_=GetTickCount64();
}

void PlayerWindow::restoreSession() {
    if (!preferences_.restorePlaylist || preferences_.launchAction!=LaunchAction::RestoreSession) return;
    const auto session=playlists_.loadSession(); if (session.entries.empty()) return;
    std::vector<std::string> paths; for (const auto& entry:session.entries) paths.push_back(entry.path);
    openPaths(paths);
    const auto index=std::clamp<std::int64_t>(session.currentIndex,0,static_cast<std::int64_t>(paths.size())-1);
    engine_.setProperty("playlist-pos",std::to_string(index));
    pendingResumePath_=paths[static_cast<size_t>(index)]; pendingSeek_=session.position; pendingPause_=preferences_.pauseOnOpen;
}

void PlayerWindow::savePlaylist() {
    const auto paths=choosePaths(window_,L"Save playlist",false,false,true,L"UTF-8 playlists",L"*.m3u8;*.m3u;*.pls;*.xspf");
    if (paths.empty()) return;
    auto file=std::filesystem::path(fromUtf8(paths.front())); if (!file.has_extension()) file+=L".m3u8";
    PlaylistDocument document; document.currentIndex=engine_.state().playlistPosition; document.position=engine_.state().position;
    for (const auto& item:engine_.state().playlist) document.entries.push_back({item.filename,item.title,-1});
    if (!playlists_.save(file,document)) notice(fromUtf8(playlists_.error()),true); else notice(L"Playlist saved");
}

void PlayerWindow::togglePip() {
    if (fullscreen_) toggleFullscreen();
    preview_.hide();
    pip_=!pip_; controlsVisible_=true; hovered_=-1; interactionUntil_=GetTickCount64()+static_cast<ULONGLONG>(preferences_.controlsHideSeconds*1000);
    if (pip_) {
        pipPlacement_.length=sizeof(pipPlacement_); GetWindowPlacement(window_,&pipPlacement_);
        pipStyle_=GetWindowLongPtrW(window_,GWL_STYLE);
        SetWindowLongPtrW(window_,GWL_STYLE,(pipStyle_&~static_cast<LONG_PTR>(WS_CAPTION|WS_MAXIMIZEBOX|WS_MINIMIZEBOX))|WS_THICKFRAME);
        MONITORINFO monitor{}; monitor.cbSize=sizeof(monitor); GetMonitorInfoW(MonitorFromWindow(window_,MONITOR_DEFAULTTONEAREST),&monitor);
        const int width=static_cast<int>(420*dpi_),height=static_cast<int>(276*dpi_);
        SetWindowPos(window_,preferences_.pipAlwaysOnTop?HWND_TOPMOST:HWND_TOP,monitor.rcWork.right-width-static_cast<int>(24*dpi_),
            monitor.rcWork.bottom-height-static_cast<int>(24*dpi_),width,height,SWP_FRAMECHANGED);
    } else {
        SetWindowLongPtrW(window_,GWL_STYLE,pipStyle_); SetWindowPlacement(window_,&pipPlacement_);
        SetWindowPos(window_,preferences_.alwaysOnTop?HWND_TOPMOST:HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_FRAMECHANGED);
        if (preferences_.pipPauseOnClose) engine_.setProperty("pause","yes");
    }
    if (plugins_) plugins_->emit(pip_?"iina.pip-entered":"iina.pip-exited");
    layoutVideo(); InvalidateRect(window_,nullptr,FALSE);
}

void PlayerWindow::toggleMusicMode(bool resize) {
    if (pip_) togglePip(); if (fullscreen_) toggleFullscreen();
    musicMode_=!musicMode_; preferences_.musicMode=musicMode_;
    if (resize) {
        RECT bounds{0,0,static_cast<LONG>((musicMode_?860:preferences_.windowWidth)*dpi_),static_cast<LONG>((musicMode_?560:preferences_.windowHeight)*dpi_)};
        AdjustWindowRectExForDpi(&bounds,static_cast<DWORD>(GetWindowLongPtrW(window_,GWL_STYLE)),FALSE,0,GetDpiForWindow(window_));
        SetWindowPos(window_,nullptr,0,0,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOMOVE|SWP_NOZORDER);
    }
    layoutVideo(); InvalidateRect(window_,nullptr,FALSE);
}

void PlayerWindow::updatePlatform() {
    const auto& state=engine_.state(); PlatformPlayback playback;
    playback.title=fromUtf8(state.title); playback.position=state.position; playback.duration=state.duration; playback.speed=state.speed;
    playback.idle=state.idle; playback.paused=state.paused; playback.audioOnly=state.width==0;
    playback.hasPrevious=state.playlistPosition>0; playback.hasNext=state.playlistPosition+1<static_cast<std::int64_t>(state.playlist.size());
    if (auto found=state.metadata.find("artist");found!=state.metadata.end()) playback.artist=fromUtf8(found->second);
    if (auto found=state.metadata.find("album");found!=state.metadata.end()) playback.album=fromUtf8(found->second);
    platform_.update(playback);
}

void PlayerWindow::updateAccessibility() {
    if (!window_) return;
    POINT origin{}; ClientToScreen(window_,&origin); std::vector<AccessibleNode> nodes;
    for (size_t i=0;i<hits_.size();++i) {
        const auto& hit=hits_[i]; AccessibleNode node; node.id=static_cast<int>(i+1); node.name=hit.hint;
        node.bounds={origin.x+static_cast<LONG>(hit.rect.left*dpi_),origin.y+static_cast<LONG>(hit.rect.top*dpi_),
            origin.x+static_cast<LONG>(hit.rect.right*dpi_),origin.y+static_cast<LONG>(hit.rect.bottom*dpi_)};
        node.focused=hovered_==static_cast<int>(i);
        if (hit.action==SeekBar || hit.action==VolumeBar) {
            node.role=AccessibleRole::Slider; node.value=hit.action==SeekBar?engine_.state().position:engine_.state().volume;
            node.maximum=hit.action==SeekBar?engine_.state().duration:100; node.step=hit.action==SeekBar?preferences_.seekSeconds:preferences_.volumeStep;
            node.enabled=hit.action==VolumeBar || engine_.state().duration>0;
        } else if (hit.action==PlayItem || hit.action==ChapterItem) {
            node.role=AccessibleRole::ListItem;
            if (hit.action==PlayItem && hit.data>=0 && static_cast<size_t>(hit.data)<engine_.state().playlist.size()) {
                const auto& item=engine_.state().playlist[static_cast<size_t>(hit.data)];
                node.name=fromUtf8(item.title.empty()?mediaTitle(item.filename):item.title); node.selected=item.current;
            }
        } else if (hit.action==PlaylistTab || hit.action==ChaptersTab) { node.role=AccessibleRole::Tab; node.selected=(hit.action==ChaptersTab)==chapters_; }
        nodes.push_back(std::move(node));
    }
    accessibility_.update(std::move(nodes));
}

void PlayerWindow::updateWebViews() {
    if (!window_) return;
    POINT origin{}; ClientToScreen(window_,&origin); const auto video=videoRect();
    for (auto& [key,view]:webviews_) {
        const bool sidebar=key.ends_with(":sidebar");
        const float left=sidebar?width_-std::max(240.f,sidebarWidth()):video.left;
        const float right=sidebar?width_:video.right;
        const float top=sidebar?0.f:video.top, bottom=sidebar?height_:video.bottom;
        view->setBounds({origin.x+static_cast<LONG>(left*dpi_),origin.y+static_cast<LONG>(top*dpi_),
            origin.x+static_cast<LONG>(right*dpi_),origin.y+static_cast<LONG>(bottom*dpi_)});
    }
}

void PlayerWindow::pollServices() {
    if (plugins_ && preferences_.pluginsEnabled) plugins_->poll();
    if (!online_) return;
    // Private results (such as an AList sign-in, which carries a token) are
    // only handed to a caller that names their id, so collect the requests
    // this window is waiting on explicitly before the general poll.
    std::set<std::uint64_t> waiting;
    for (const auto& [id,callback]:serviceCallbacks_) waiting.insert(id);
    auto results=waiting.empty()?std::vector<ServiceResult>{}:online_->pollFor(waiting);
    for (auto& result:online_->poll()) results.push_back(std::move(result));
    for (const auto& result:results) {
        auto found=serviceCallbacks_.find(result.id);
        if (found!=serviceCallbacks_.end()) {
            auto callback=std::move(found->second); serviceCallbacks_.erase(found);
            try { callback(result); } catch (const std::exception& error) { notice(fromUtf8(error.what()),true); }
        } else if (!result.error.empty()) logLines_.push_back("Online service: "+result.error);
    }
}

void PlayerWindow::dispatchCommand(const std::string& command) {
    static const std::map<std::string,int> commands{{"open-files",OpenFiles},{"open-folder",OpenFolder},{"open-url",OpenUrl},
        {"play-pause",TogglePause},{"stop",Stop},{"next",Next},{"previous",Previous},{"fullscreen",ToggleFullscreen},
        {"pip",PictureInPicture},{"music-mode",MusicMode},{"playlist",TogglePlaylist},{"preferences",PreferencesAction},
        {"history",HistoryAction},{"screenshot",Screenshot},{"mute",ToggleMute},{"crop",CropVideo},{"inspector",ShowInfo},
        {"online-subtitles",OnlineSubtitles},{"plugins",PluginManager},{"quit",Quit},{"add-subtitles",AddSubtitle},
        {"save-playlist",SavePlaylist},{"shuffle",ShufflePlaylist},{"zoom-in",ZoomIn},{"zoom-out",ZoomOut},
        {"alist-browse",BrowseAList},{"alist-sign-out",SignOutAList}};
    auto value=command;
    if (value.starts_with("velin:")) value.erase(0,6);
    if (auto item=commands.find(value);item!=commands.end()) execute(item->second);
    else if (value=="ignore" || value=="disabled" || value.empty()) return;
    else {
        if (value.starts_with("mpv:")) value.erase(0,4);
        try { const auto arguments=tokenize(value); if (!arguments.empty()) engine_.command(arguments); }
        catch (const std::exception& error) { notice(fromUtf8(error.what()),true); }
    }
}

void PlayerWindow::acceptLaunch(const std::vector<std::wstring>& arguments) {
    std::vector<std::string> paths; bool append=false,fullscreen=false,pip=false,music=false;
    std::vector<std::string> commands; bool positionalOnly=false;
    std::optional<std::string> alistTarget;
    for (size_t i=0;i<arguments.size();++i) {
        const auto value=toUtf8(arguments[i]);
        if (positionalOnly) { paths.push_back(value); continue; }
        if (value=="--") { positionalOnly=true; continue; }
        if (value=="--enqueue") append=true;
        else if (value=="--fullscreen") fullscreen=true;
        else if (value=="--pip") pip=true;
        else if (value=="--music") music=true;
        else if (value=="--new-window" || value=="--no-single-instance") continue;
        else if (value=="--data-dir") { if (i+1<arguments.size()) ++i; }
        else if (value=="--command" && i+1<arguments.size()) commands.push_back(toUtf8(arguments[++i]));
        else if (value.starts_with("--command=")) commands.push_back(value.substr(10));
        else if (value=="--register") execute(RegisterAssociations);
        else if (value=="--unregister") execute(UnregisterAssociations);
        else if (value.starts_with("velin://open?") || value.starts_with("iina://open?") || value.starts_with("velin://alist?")) {
            const bool alistLink=value.starts_with("velin://alist?");
            const auto query=value.substr(value.find('?')+1); size_t begin=0;
            while (begin<query.size()) {
                const auto end=query.find('&',begin); const auto part=query.substr(begin,end==std::string::npos?std::string::npos:end-begin);
                const auto equal=part.find('='); const auto key=part.substr(0,equal);
                if (equal!=std::string::npos) {
                    auto encoded=part.substr(equal+1); std::string decoded;
                    auto hex=[](char c)->int { if(c>='0'&&c<='9')return c-'0';if(c>='A'&&c<='F')return c-'A'+10;if(c>='a'&&c<='f')return c-'a'+10;return -1; };
                    for (size_t n=0;n<encoded.size();++n) {
                        if (encoded[n]=='%' && n+2<encoded.size() && hex(encoded[n+1])>=0 && hex(encoded[n+2])>=0) { decoded+=static_cast<char>(hex(encoded[n+1])*16+hex(encoded[n+2])); n+=2; }
                        else decoded+=encoded[n]=='+'?' ':encoded[n];
                    }
                    if (alistLink) { if (key=="path" && decoded.find('\0')==std::string::npos) alistTarget=decoded; }
                    else if ((key=="url" || key=="path") && decoded.find('\0')==std::string::npos) paths.push_back(decoded);
                    else if (key=="enqueue") append=decoded=="1" || decoded=="true";
                    else if (key=="full_screen") fullscreen=decoded=="1" || decoded=="true";
                    else if (key.starts_with("mpv_")) engine_.setProperty(key.substr(4),decoded);
                }
                if (end==std::string::npos) break; begin=end+1;
            }
        } else if (value.starts_with("--mpv-")) {
            const auto equal=value.find('='); if (equal!=std::string::npos) engine_.setProperty(value.substr(6,equal-6),value.substr(equal+1));
        } else if (!value.starts_with("--plugin-")) paths.push_back(value);
    }
    if (!paths.empty()) openPaths(paths,append);
    // velin://alist?path=<AList path>: play a file (with the rest of its
    // folder queued) or browse a folder, signing in first if needed.
    if (alistTarget) {
        std::string target;
        try { target=alist::normalizePath(*alistTarget); } catch (const alist::Error&) { notice(L"The AList link has an invalid path.",true); }
        if (!target.empty()) {
            if (isMediaFile(target)) browseAList(alist::parentPath(target),target); else browseAList(target);
        }
    }
    if (fullscreen && !fullscreen_) toggleFullscreen();
    if (pip && !pip_) togglePip();
    if (music && !musicMode_) toggleMusicMode();
    for (const auto& command:commands) dispatchCommand(command);
    if (IsIconic(window_)) ShowWindow(window_,SW_RESTORE);
    SetForegroundWindow(window_);
}

Json PlayerWindow::extensionUi(const PluginInfo& metadata,const std::string& operation,const Json& arguments) {
    const auto& plugin=metadata.identifier;
    if (operation=="plugin.unload") {
        for(const auto& [process,peer]:pluginPeers_)if(peer.first==plugin){
            EnumWindows([](HWND window,LPARAM argument)->BOOL{DWORD process=0;GetWindowThreadProcessId(window,&process);wchar_t type[80]{};GetClassNameW(window,type,80);if(process==static_cast<DWORD>(argument) && std::wstring_view(type)==L"Velin.Player")PostMessageW(window,WM_CLOSE,0,0);return TRUE;},static_cast<LPARAM>(process));
        }
        std::erase_if(pluginPeers_,[&](const auto& entry){return entry.second.first==plugin;});
        std::erase_if(webviews_,[&](const auto& entry) { return entry.first.starts_with(plugin+":"); }); return nullptr;
    }
    if (operation=="core.getHistory") {
        Json result=Json::array();
        for (const auto& entry:history_.load()) result.push_back({{"url",entry.path},{"name",entry.title},{"lastPlayed",entry.lastPlayed},{"position",entry.position},{"duration",entry.duration}});
        return result;
    }
    if (operation=="core.getRecentDocuments") return preferences_.recentFiles;
    if(operation=="core.setUIVisibility"){uiHidden_=!(arguments.size()>0 && arguments[0].is_boolean() && arguments[0].get<bool>());layoutVideo();InvalidateRect(window_,nullptr,FALSE);return nullptr;}
    if(operation=="file.showInFinder"){
        auto* item=ILCreateFromPathW(textArgument(arguments,0).c_str());if(!item)throw std::runtime_error("Cannot find the file in Explorer.");const HRESULT result=SHOpenFolderAndSelectItems(item,0,nullptr,0);ILFree(item);return SUCCEEDED(result);
    }
    if(operation=="file.trash"){
        IFileOperation* operation=nullptr;IShellItem* item=nullptr;
        HRESULT result=CoCreateInstance(CLSID_FileOperation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&operation));
        if(SUCCEEDED(result)){operation->SetOwnerWindow(window_);operation->SetOperationFlags(FOF_ALLOWUNDO|FOFX_RECYCLEONDELETE);result=SHCreateItemFromParsingName(textArgument(arguments,0).c_str(),nullptr,IID_PPV_ARGS(&item));if(SUCCEEDED(result)){result=operation->DeleteItem(item,nullptr);if(SUCCEEDED(result))result=operation->PerformOperations();item->Release();}operation->Release();}
        if(FAILED(result))throw std::runtime_error("Windows could not move the file to the Recycle Bin.");return nullptr;
    }
    if (operation=="subtitle.error") { notice(textArgument(arguments,1),true); return nullptr; }
    if (operation=="subtitle.downloaded") {
        if (arguments.size()>1 && arguments[1].is_array()) for (const auto& path:arguments[1]) if (path.is_string()) engine_.addSubtitle(path.get<std::string>());
        return nullptr;
    }
    if (operation=="subtitle.results") {
        if (arguments.size()<2 || !arguments[1].is_array()) return nullptr;
        const auto results=arguments[1]; std::vector<std::pair<std::wstring,std::wstring>> choices;
        for (const auto& item:results) {
            const auto description=item.value("desc",Json{});
            choices.push_back({fromUtf8(description.is_string()?description.get<std::string>():description.dump()),L"Plugin subtitle result"});
        }
        const auto selected=chooseListItem(window_,L"Plugin subtitle results",choices);
        if (selected) plugins_->downloadSubtitleProvider(plugin,toUtf8(textArgument(arguments,0)),results[*selected].value("data",Json{}));
        return nullptr;
    }
    if (operation=="osd") { notice(textArgument(arguments,0)); return nullptr; }
    if (operation=="alert") { MessageBoxW(window_,textArgument(arguments,0).c_str(),fromUtf8(plugin).c_str(),MB_OK); return nullptr; }
    if (operation=="open-url" || operation=="utils.open") {
        const auto url=textArgument(arguments,0);
        if (!(url.starts_with(L"https://") || url.starts_with(L"http://") ||
              (operation=="utils.open" && std::filesystem::path(url).is_absolute() && std::filesystem::exists(url))))
            throw std::runtime_error("Expected an HTTP(S) URL or an existing authorized local file.");
        return reinterpret_cast<INT_PTR>(ShellExecuteW(window_,L"open",url.c_str(),nullptr,nullptr,SW_SHOWNORMAL))>32;
    }
    if (operation=="preferences.open") { showPreferences(); return nullptr; }
    if (operation=="utils.ask") return MessageBoxW(window_,textArgument(arguments,0).c_str(),fromUtf8(plugin).c_str(),MB_YESNO|MB_ICONQUESTION)==IDYES;
    if (operation=="utils.prompt") { auto result=showTextPrompt(window_,fromUtf8(plugin),textArgument(arguments,0)); return result?Json(toUtf8(*result)):Json{}; }
    if (operation=="utils.chooseFile") {
        const auto options=arguments.size()>1 && arguments[1].is_object()?arguments[1]:Json::object();
        const auto paths=choosePaths(window_,textArgument(arguments,0,L"Choose files"),options.value("multiple",options.value("allowsMultipleSelection",false)),
            options.value("isDir",options.value("chooseDirectories",false)),options.value("save",false));
        if (options.value("multiple",options.value("allowsMultipleSelection",false))) return paths;
        return paths.empty()?Json{}:Json(paths.front());
    }
    if (operation=="utils.keychainWrite") {
        const auto service=L"Velin.Plugin."+fromUtf8(plugin)+L"."+textArgument(arguments,0)+L"."+textArgument(arguments,1);
        return storeCredential(service,textArgument(arguments,1),toUtf8(textArgument(arguments,2)));
    }
    if (operation=="utils.keychainRead") {
        const auto result=credential(L"Velin.Plugin."+fromUtf8(plugin)+L"."+textArgument(arguments,0)+L"."+textArgument(arguments,1));
        return result.empty()?Json(false):Json(result);
    }
    if (operation=="utils.preferredLocalizations") return Json::array({"en-US"});
    if (operation=="core.window.get") {
        const auto key=toUtf8(textArgument(arguments,0)); RECT frame{}; GetWindowRect(window_,&frame);
        if (key=="fullscreen") return fullscreen_; if (key=="pip") return pip_; if (key=="ontop") return preferences_.alwaysOnTop;
        if (key=="frame") return Json{{"x",frame.left},{"y",frame.top},{"width",frame.right-frame.left},{"height",frame.bottom-frame.top}};
        if (key=="loaded") return true;
        if (key=="visible") return IsWindowVisible(window_)!=FALSE;
        if (key=="miniaturized") return IsIconic(window_)!=FALSE;
        if (key=="sidebar") return showPlaylist_?(chapters_?"chapters":"playlist"):Json{};
        if (key=="screens") {
            Json screens=Json::array();
            EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR display,HDC,LPRECT,LPARAM context)->BOOL {
                MONITORINFOEXW info{};info.cbSize=sizeof(info);GetMonitorInfoW(display,&info);
                reinterpret_cast<Json*>(context)->push_back({{"name",toUtf8(info.szDevice)},{"x",info.rcMonitor.left},{"y",info.rcMonitor.top},
                    {"width",info.rcMonitor.right-info.rcMonitor.left},{"height",info.rcMonitor.bottom-info.rcMonitor.top},{"primary",(info.dwFlags&MONITORINFOF_PRIMARY)!=0}});return TRUE;
            },reinterpret_cast<LPARAM>(&screens));return screens;
        }
        throw std::runtime_error("Unknown player window property: "+key);
    }
    if (operation=="core.window.set") {
        const auto key=toUtf8(textArgument(arguments,0)); const auto value=arguments.size()>1?arguments[1]:Json{};
        if (key=="fullscreen") { if (value.get<bool>()!=fullscreen_) toggleFullscreen(); }
        else if (key=="pip") { if (value.get<bool>()!=pip_) togglePip(); }
        else if (key=="ontop") { if (value.get<bool>()!=preferences_.alwaysOnTop) execute(ToggleTopmost); }
        else if (key=="visible") ShowWindow(window_,value.get<bool>()?SW_SHOW:SW_HIDE);
        else if (key=="miniaturized") ShowWindow(window_,value.get<bool>()?SW_MINIMIZE:SW_RESTORE);
        else if (key=="sidebar") {showPlaylist_=!value.is_null() && value!=false;chapters_=value=="chapters";layoutVideo();InvalidateRect(window_,nullptr,FALSE);}
        else if (key=="frame" && value.is_object()) {
            RECT frame{}; GetWindowRect(window_,&frame);
            SetWindowPos(window_,nullptr,value.value("x",static_cast<int>(frame.left)),value.value("y",static_cast<int>(frame.top)),
                std::clamp(value.value("width",static_cast<int>(frame.right-frame.left)),240,16384),std::clamp(value.value("height",static_cast<int>(frame.bottom-frame.top)),135,16384),SWP_NOZORDER);
        } else throw std::runtime_error("Unknown player window property: "+key);
        return nullptr;
    }
    if (operation=="global.createPlayerInstance") {
        if(pluginParent_!=0)throw std::runtime_error("Only the plugin controller can create player instances.");
        if(pluginPeers_.size()>=32)throw std::runtime_error("The plugin player instance limit was reached.");
        const auto options=!arguments.empty() && arguments[0].is_object()?arguments[0]:Json::object();
        const auto token=randomToken();
        std::vector<std::wstring> launch{L"--new-window",L"--plugin-parent="+std::to_wstring(GetCurrentProcessId()),
            L"--plugin-owner="+fromUtf8(plugin),L"--plugin-secret="+fromUtf8(token),L"--plugin-label="+fromUtf8(options.value("label",std::string{}))};
        if (options.contains("url") && options["url"].is_string()) launch.push_back(fromUtf8(options["url"].get<std::string>()));
        if (options.value("fullscreen",false)) launch.push_back(L"--fullscreen");
        if (options.value("musicMode",false)) launch.push_back(L"--music");
        DWORD id=0; if (!launchPlayer(launch,&id)) throw std::runtime_error("Cannot create the player window.");
        pluginPeers_[id]={plugin,token};return id;
    }
    if (operation=="global.getLabel") return playerLabel_;
    if(operation=="global.postMessage") {
        DWORD target=pluginParent_;size_t index=0;std::string token=pluginSecret_;
        if(arguments.size()>=3){
            try{target=arguments[0].is_string()?static_cast<DWORD>(std::stoul(arguments[0].get<std::string>())):arguments[0].get<DWORD>();}catch(...){throw std::runtime_error("Invalid player identifier.");}
            const auto peer=pluginPeers_.find(target);if(peer==pluginPeers_.end() || peer->second.first!=plugin)throw std::runtime_error("The target player does not belong to this plugin.");
            token=peer->second.second;index=1;
        }else if(pluginOwner_!=plugin || !target)throw std::runtime_error("This plugin has no parent player.");
        if(arguments.size()<index+2 || !arguments[index].is_string())throw std::runtime_error("A message name and value are required.");
        return sendPluginMessage(window_,target,{{"plugin",plugin},{"token",token},{"name",arguments[index]},{"data",arguments[index+1]}});
    }
    if (operation.starts_with("overlay.") || operation.starts_with("sidebar.") || operation.starts_with("standalone.")) {
        const auto split=operation.find('.'); const auto kind=operation.substr(0,split),verb=operation.substr(split+1),key=plugin+":"+kind;
        auto found=webviews_.find(key);
        if (found==webviews_.end()) {
            const auto domains=metadata.permissions.contains("network-request") ? metadata.domains : std::vector<std::string>{};
            if (!pluginWebViewHasEnforceableNetworkPolicy(domains)) {
                notice(L"Plugin HTML view disabled: restricted WebSocket permissions cannot be enforced.",true);
                throw std::runtime_error("Plugin HTML views are disabled because their WebSocket network permissions cannot be enforced.");
            }
            auto view=std::make_unique<PluginWebView>(window_,kind,metadata.directory,dataDirectory_/L"WebViews"/fromUtf8(plugin)/fromUtf8(kind),domains,
                [this,plugin,kind](const std::string& name,const Json& data) { if (plugins_) plugins_->receiveWebMessage(plugin,kind,name,data); });
            found=webviews_.emplace(key,std::move(view)).first;
        }
        auto result=found->second->operate(verb,arguments); updateWebViews(); return result;
    }
    throw std::runtime_error("Unsupported native plugin operation: "+operation);
}
} // namespace velin
