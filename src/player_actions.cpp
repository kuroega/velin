// SPDX-License-Identifier: GPL-3.0-or-later
#include "velin/player_window.hpp"
#include "velin/preferences_dialog.hpp"
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace velin {
bool PlayerWindow::executeFeature(int action,int data) {
    const auto& state=engine_.state();
    switch(action) {
    case PreferencesAction: showPreferences(); break;
    case HistoryAction:
        showHistoryDialog(window_,history_,[this](const HistoryEntry& entry) {
            openPaths({entry.path});
            if (preferences_.rememberPosition) { pendingResumePath_=entry.path; pendingSeek_=entry.position; pendingPause_=preferences_.pauseOnOpen; }
        }); break;
    case SavePlaylist: savePlaylist(); break;
    case LoadPlaylist: {
        const auto paths=choosePaths(window_,L"Open playlist",false,false,false,L"Playlists",L"*.m3u8;*.m3u;*.pls;*.xspf");
        if (paths.empty()) break;
        const auto document=playlists_.load(std::filesystem::path(fromUtf8(paths.front())));
        if (!playlists_.error().empty()) { notice(fromUtf8(playlists_.error()),true); break; }
        std::vector<std::string> media; for (const auto& item:document.entries) media.push_back(item.path);
        if (!media.empty()) openPaths(media);
        break;
    }
    case ShufflePlaylist: engine_.shufflePlaylist(); shuffled_=true; notice(L"Playlist shuffled"); break;
    case UnshufflePlaylist: engine_.shufflePlaylist(false); shuffled_=false; notice(L"Original playlist order restored"); break;
    case MoveItemUp: if (data>0) engine_.movePlaylist(data,data-1); break;
    case MoveItemDown: if (data>=0 && data+1<static_cast<int>(state.playlist.size())) engine_.movePlaylist(data,data+1); break;
    case ClearEntirePlaylist: engine_.stop(); engine_.command({"playlist-clear"}); engine_.command({"playlist-remove","current"}); break;
    case PictureInPicture: togglePip(); break;
    case MusicMode: toggleMusicMode(); break;
    case VideoSettings: showPreferences("Video"); break;
    case AudioSettings: showPreferences("Audio"); break;
    case SubtitleSettings: showPreferences("Subtitles"); break;
    case NetworkSettings: showPreferences("Network"); break;
    case AdvancedSettings: showPreferences("Advanced"); break;
    case CropVideo: {
        if (!state.loaded || state.width<=0) break;
        const bool paused=state.paused; engine_.setProperty("pause","yes"); RECT bounds{}; GetWindowRect(video_,&bounds);
        const auto crop=selectVideoCrop(window_,bounds,static_cast<int>(state.width),static_cast<int>(state.height));
        if (crop) {
            const auto value=std::to_string(crop->width)+"x"+std::to_string(crop->height)+"+"+std::to_string(crop->x)+"+"+std::to_string(crop->y);
            engine_.setProperty("video-crop",value); preferences_.mpvOptions["video-crop"]=value; notice(L"Video cropped");
        }
        engine_.setProperty("pause",paused?"yes":"no"); break;
    }
    case ClearCrop: engine_.setProperty("video-crop",""); preferences_.mpvOptions.erase("video-crop"); notice(L"Crop removed"); break;
    case CustomCropAspect: {
        if (!state.loaded || state.width <= 0 || state.height <= 0) { notice(L"Open a video before cropping.",true); break; }
        const auto input=showTextPrompt(window_,L"Custom crop",L"Enter an aspect ratio, such as 16:9 or 1.85.",L"16:9");
        if (!input) break;
        const auto geometry=centeredCropGeometry(state.width,state.height,toUtf8(*input));
        if (!geometry) { notice(L"Enter a valid positive aspect ratio that fits the video.",true); break; }
        if (engine_.setProperty("video-crop",*geometry)) { preferences_.mpvOptions["video-crop"]=*geometry; notice(L"Crop requested"); }
        else notice(L"Could not request crop.",true);
        break;
    }
    case ExternalAudio: {
        const auto paths=choosePaths(window_,L"Add audio track",true,false,false,L"Audio files",L"*.flac;*.wav;*.mp3;*.m4a;*.aac;*.ogg;*.opus;*.ac3;*.dts");
        for (const auto& path:paths) engine_.addAudio(path); break;
    }
    case ToggleDeinterlace: preferences_.deinterlace=!preferences_.deinterlace; engine_.setProperty("deinterlace",preferences_.deinterlace?"yes":"no"); break;
    case ToggleDeband: preferences_.deband=!preferences_.deband; engine_.setProperty("deband",preferences_.deband?"yes":"no"); break;
    case FlipVideo: engine_.command({"vf","toggle","@velin-flip:lavfi=[vflip]"}); break;
    case MirrorVideo: engine_.command({"vf","toggle","@velin-mirror:lavfi=[hflip]"}); break;
    case SubtitleSizeUp: preferences_.subtitleScale=std::min(10.,state.subtitleScale*1.1); engine_.setProperty("sub-scale",std::to_string(preferences_.subtitleScale)); break;
    case SubtitleSizeDown: preferences_.subtitleScale=std::max(.1,state.subtitleScale/1.1); engine_.setProperty("sub-scale",std::to_string(preferences_.subtitleScale)); break;
    case GoToTime: {
        const auto time=showTextPrompt(window_,L"Go to time",L"Enter seconds or HH:MM:SS.",fromUtf8(formatTime(state.position)));
        if (!time) break;
        const auto text=toUtf8(*time); std::istringstream parts(text); std::string part; double result=0; bool valid=true; int count=0;
        while (std::getline(parts,part,':')) {
            try { size_t consumed=0; const double value=std::stod(part,&consumed); if (consumed!=part.size() || !std::isfinite(value) || value<0 || ++count>3) { valid=false; break; } result=result*60+value; }
            catch (...) { valid=false; break; }
        }
        if (valid && count>0) engine_.seek(result,true); else notice(L"Enter a valid time, such as 01:23:45.",true); break;
    }
    case FrameBack: engine_.command({"frame-back-step"}); break;
    case JumpToBeginning: if (!state.idle) engine_.seek(0,true); break;
    case ZoomIn: engine_.command({"add","video-zoom","0.1"}); break;
    case ZoomOut: engine_.command({"add","video-zoom","-0.1"}); break;
    case ResetPan: engine_.setProperty("video-zoom","0"); engine_.setProperty("video-pan-x","0"); engine_.setProperty("video-pan-y","0"); break;
    case RevealFile: {
        if (state.path.empty() || state.path.find("://")!=std::string::npos) break;
        PIDLIST_ABSOLUTE item=ILCreateFromPathW(fromUtf8(state.path).c_str());
        if (item) { SHOpenFolderAndSelectItems(item,0,nullptr,0); ILFree(item); } break;
    }
    case TrashFile: {
        if (state.path.empty() || state.path.find("://")!=std::string::npos) break;
        if (MessageBoxW(window_,L"Move the current media file to the Recycle Bin?",L"Move to Recycle Bin",MB_YESNO|MB_ICONQUESTION)!=IDYES) break;
        const auto path=fromUtf8(state.path); engine_.stop();
        IFileOperation* operation=nullptr; IShellItem* item=nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOperation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&operation)))) {
            operation->SetOwnerWindow(window_); operation->SetOperationFlags(FOF_ALLOWUNDO|FOFX_RECYCLEONDELETE);
            if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(),nullptr,IID_PPV_ARGS(&item)))) { operation->DeleteItem(item,nullptr); const HRESULT result=operation->PerformOperations(); if (FAILED(result)) notice(L"Windows could not move this file to the Recycle Bin.",true); item->Release(); }
            operation->Release();
        }
        break;
    }
    case OpenDisc: {
        const auto paths=choosePaths(window_,L"Open a DVD or Blu-ray folder",false,true);
        if (paths.empty()) break;
        const auto folder=std::filesystem::path(fromUtf8(paths.front()));
        if (std::filesystem::exists(folder/L"BDMV")) { engine_.setProperty("bluray-device",paths.front()); openPaths({"bd://"}); }
        else { engine_.setProperty("dvd-device",paths.front()); openPaths({"dvd://"}); }
        break;
    }
    case OpenClipboard: {
        if (!::OpenClipboard(window_)) break;
        const auto handle=GetClipboardData(CF_UNICODETEXT); std::wstring text;
        if (handle) { if (auto* data=static_cast<const wchar_t*>(GlobalLock(handle))) { text=data; GlobalUnlock(handle); } }
        CloseClipboard();
        if (!text.empty()) openPaths({toUtf8(text)});
        break;
    }
    case NewWindow: {
        std::wstring command=L"\""+(executableDirectory()/L"velin.exe").wstring()+L"\" --new-window";
        STARTUPINFOW startup{}; startup.cb=sizeof(startup); PROCESS_INFORMATION process{};
        if (CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&process)) { CloseHandle(process.hThread); CloseHandle(process.hProcess); }
        else notice(L"Could not create a new player window.",true);
        break;
    }
    case PluginManager: showPluginManager(); break;
    case InstallPlugin: {
        const auto paths=choosePaths(window_,L"Install an IINA plugin",false,false,false,L"IINA plugin packages",L"*.iinaplgz;*.zip");
        if (paths.empty() || !plugins_) break;
        installPluginPackage(std::filesystem::path(fromUtf8(paths.front())));
        break;
    }
    case ReloadPlugins: if (plugins_) { webviews_.clear(); plugins_->reload(); notice(L"Plugins reloaded"); } break;
    case PluginConsole: {
        if (!plugins_) break;
        const auto info=plugins_->discover(); std::vector<std::pair<std::wstring,std::wstring>> choices;
        for (const auto& plugin:info) if (plugin.running) choices.push_back({fromUtf8(plugin.identifier),fromUtf8(plugin.name)});
        if (choices.empty()) { notice(L"Enable a plugin before opening its console.",true); break; }
        const auto chosen=chooseListItem(window_,L"Plugin console",choices); if (!chosen) break;
        const auto source=showTextPrompt(window_,L"Plugin JavaScript console",L"Evaluate JavaScript in the selected plugin context.",L"iina.mpv.getString('media-title')",true);
        if (source) {
            try { const auto result=plugins_->evaluate(toUtf8(choices[*chosen].first),toUtf8(*source)); showTextPrompt(window_,L"Evaluation result",L"Result",fromUtf8(result.dump(2)),true); }
            catch (const std::exception& error) { notice(fromUtf8(error.what()),true); }
        }
        break;
    }
    case OnlineSubtitles: searchSubtitles(); break;
    case BrowseAList: browseAList(); break;
    case SignOutAList: signOutAList(); break;
    case CheckUpdates: {
        if (!online_) break;
        if (preferences_.updateManifestUrl.empty()) {
            const auto url=showTextPrompt(window_,L"Update channel",L"Enter the HTTPS release manifest URL supplied by your distributor.");
            if (!url || !url->starts_with(L"https://")) break;
            preferences_.updateManifestUrl=toUtf8(*url); savePreferences();
        }
        const auto id=online_->submit("updates.check",{{"currentVersion",VELIN_VERSION},{"url",preferences_.updateManifestUrl}});
        serviceCallbacks_[id]=[this](const ServiceResult& result) {
            if (!result.error.empty()) { notice(fromUtf8(result.error),true); return; }
            if (!result.data.value("available",false)) { notice(L"You are using the latest published version."); return; }
            const auto manifest=result.data.at("manifest");
            const auto version=manifest.at("version").get<std::string>();
            const auto message=L"Version "+fromUtf8(version)+L" is available. Download and install it now? Velin will restart after verification.";
            if (MessageBoxW(window_,message.c_str(),L"Velin update",MB_YESNO|MB_ICONQUESTION)!=IDYES) return;
            const auto directory=dataDirectory_/L"Updates"; std::filesystem::create_directories(directory);
            const auto archive=directory/L"velin-update.zip";
            const auto download=online_->submit("download.verified",{{"url",manifest.at("url")},{"sha256",manifest.at("sha256")},{"destination",toUtf8(archive.wstring())}});
            serviceCallbacks_[download]=[this,manifest,archive,directory](const ServiceResult& downloaded) {
                if (!downloaded.error.empty()) { notice(fromUtf8(downloaded.error),true); return; }
                auto script=executableDirectory()/L"scripts"/L"Apply-Update.ps1";
                if (!std::filesystem::exists(script)) script=executableDirectory()/L"Apply-Update.ps1";
                if (!std::filesystem::exists(script)) { notice(L"The update installer script is missing from this distribution.",true); return; }
                const auto stagedScript=directory/L"Apply-Update.ps1";
                std::filesystem::copy_file(script,stagedScript,std::filesystem::copy_options::overwrite_existing);
                std::wstring system(MAX_PATH,L'\0'); const UINT length=GetSystemDirectoryW(system.data(),static_cast<UINT>(system.size())); system.resize(length);
                const auto powershell=std::filesystem::path(system)/L"WindowsPowerShell"/L"v1.0"/L"powershell.exe";
                std::vector<std::wstring> arguments{powershell.wstring(),L"-NoProfile",L"-NonInteractive",L"-ExecutionPolicy",L"Bypass",L"-File",stagedScript.wstring(),
                    L"-Archive",archive.wstring(),L"-ExpectedSha256",fromUtf8(manifest.at("sha256").get<std::string>()),L"-TargetDirectory",executableDirectory().wstring(),
                    L"-WaitProcessId",std::to_wstring(GetCurrentProcessId()),L"-Restart"};
                std::wstring command;
                for (const auto& argument:arguments) { if (!command.empty()) command+=L' '; command+=quoteWindowsArgument(argument); }
                STARTUPINFOW startup{}; startup.cb=sizeof(startup); startup.dwFlags=STARTF_USESHOWWINDOW; startup.wShowWindow=SW_HIDE; PROCESS_INFORMATION process{};
                if (!CreateProcessW(powershell.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,directory.c_str(),&startup,&process)) {
                    notice(L"Windows could not start the verified update installer.",true); return;
                }
                CloseHandle(process.hThread); CloseHandle(process.hProcess); execute(Quit);
            };
            notice(L"Downloading and verifying the update\u2026");
        };
        notice(L"Checking for updates\u2026"); break;
    }
    case InstallUpdate: notice(L"Use Check for updates to download a verified release package."); break;
    case MediaLog: showLogViewer(window_,logLines_); break;
    case CopyScreenshot: {
        if (auto frame=engine_.captureFrame(preferences_.screenshotIncludeSubtitles)) {
            if (copyImageToClipboard(window_,frame->width,frame->height,frame->bgra)) notice(L"Screenshot copied to clipboard");
            else notice(L"Could not copy the screenshot to clipboard.",true);
        } else notice(L"No decoded video frame is available.",true);
        break;
    }
    case RecognizeText: {
        auto frame=engine_.captureFrame(false); if (!frame) { notice(L"Open a video before recognizing text.",true); break; }
        if (ocrWorker_.joinable()) { notice(L"Text recognition is already running."); break; }
        notice(L"Recognizing text in the current frame\u2026");
        // A worker owns the pixels. Results are posted and handled by the UI thread.
        ocrWorker_=std::jthread([this,frame=std::move(*frame)] {
            auto result=recognizeTextBgra(frame.width,frame.height,frame.bgra);
            {std::lock_guard lock(ocrMutex_);ocrResult_=std::move(result);}
            PostMessageW(window_,WM_APP+11,0,0);
        });
        break;
    }
    case RegisterAssociations: {
        std::wstring error;
        if (PlatformIntegration::registerFileAssociations(true,&error) && PlatformIntegration::registerProtocol(true,&error)) {
            PlatformIntegration::showDefaultAppsSettings(); notice(L"Velin registered. Choose your default media types in Windows Settings.");
        } else notice(error,true);
        break;
    }
    case UnregisterAssociations: {
        std::wstring error;
        if (!PlatformIntegration::registerFileAssociations(false,&error) || !PlatformIntegration::registerProtocol(false,&error)) notice(error,true);
        else notice(L"Velin file and URL registrations removed");
        break;
    }
    default: return false;
    }
    if (!closing_) InvalidateRect(window_,nullptr,FALSE);
    return true;
}
} // namespace velin
