// SPDX-License-Identifier: GPL-3.0-or-later
#include "velin/player_window.hpp"
#include <algorithm>
#include <fstream>
#include <regex>

namespace velin {
void PlayerWindow::showPluginManager() {
    if (!plugins_) return;
    for (;;) {
        const auto plugins=plugins_->discover();
        std::vector<std::pair<std::wstring,std::wstring>> rows{{L"Install a plugin package",L"Choose a ZIP or .iinaplgz file"},
            {L"Reload plugins",L"Restart enabled plugins"},{L"Browse plugin catalog",L"Discover plugins published for IINA"}};
        for (const auto& plugin:plugins) rows.push_back({fromUtf8(plugin.name),fromUtf8(plugin.identifier+"  "+plugin.version+(plugin.enabled?"  Enabled":"  Disabled")+(plugin.error.empty()?"":"  "+plugin.error))});
        const auto selected=chooseListItem(window_,L"Plugin manager",rows);
        if (!selected) return;
        if (*selected==0) { execute(InstallPlugin); continue; }
        if (*selected==1) { execute(ReloadPlugins); continue; }
        if (*selected==2) { browsePluginCatalog(); return; }
        const auto plugin=plugins[*selected-3];
        const auto action=chooseListItem(window_,fromUtf8(plugin.name),{{plugin.enabled?L"Disable":L"Enable",L"Change plugin state"},
            {L"Uninstall",L"Remove this plugin"},{L"Console",L"Evaluate JavaScript"},{L"Update from source",L"Download the latest published package"}});
        if (!action) continue;
        try {
            if (*action==0) { webviews_.clear(); plugins_->setEnabled(plugin.identifier,!plugin.enabled); }
            else if (*action==1 && MessageBoxW(window_,L"Uninstall this plugin?",fromUtf8(plugin.name).c_str(),MB_YESNO|MB_ICONQUESTION)==IDYES) {
                webviews_.clear(); plugins_->uninstall(plugin.identifier);
            } else if (*action==2) execute(PluginConsole);
            else if(*action==3){
                std::ifstream input(dataDirectory_/L"plugin-sources.json");Json sources=Json::object();if(input)sources=Json::parse(input,nullptr,false);
                if(sources.is_object() && sources.contains(plugin.identifier)){downloadPluginRepository(sources[plugin.identifier].get<std::string>());return;}
                notice(L"This plugin has no saved source repository. Reinstall it from the catalog to enable updates.",true);
            }
        } catch (const std::exception& error) { notice(fromUtf8(error.what()),true); }
    }
}

void PlayerWindow::installPluginPackage(const std::filesystem::path& package,std::string repository) {
    try {
        const auto info=plugins_->inspect(package);
        std::wstring message=L"Install "+fromUtf8(info.name)+L" "+fromUtf8(info.version)+L"?\n\nRequested permissions:\n";
        for(const auto& permission:info.permissions)message+=L"\u2022 "+fromUtf8(permission)+L"\n";
        if(info.permissions.empty())message+=L"No additional permissions.\n";
        if(info.permissions.contains("network-request")) {
            message+=L"\nAllowed network domains:\n";
            if(std::find(info.domains.begin(),info.domains.end(),"*")!=info.domains.end())
                message+=L"\u2022 * (unrestricted)\n";
            size_t shown=0;
            for(const auto& domain:info.domains) {
                if(domain=="*")continue;
                if(shown++==12) { message+=L"\u2022 Additional domains omitted.\n"; break; }
                auto escaped=Json(domain).dump(-1,' ',true);
                if(escaped.size()>160)escaped=escaped.substr(0,157)+"...";
                message+=L"\u2022 "+fromUtf8(escaped)+L"\n";
            }
            if(info.domains.empty())message+=L"None.\n";
        }
        if(MessageBoxW(window_,message.c_str(),L"Plugin permissions",MB_YESNO|MB_ICONQUESTION)!=IDYES)return;
        plugins_->install(package,info.permissions,true);plugins_->setEnabled(info.identifier,true);
        if(!repository.empty()){
            const auto file=dataDirectory_/L"plugin-sources.json";std::ifstream input(file);Json sources=Json::object();if(input)sources=Json::parse(input,nullptr,false);if(!sources.is_object())sources=Json::object();sources[info.identifier]=repository;
            std::string error;if(!writeTextFileAtomic(file,sources.dump(2),error))logLines_.push_back(error);
        }
        notice(L"Plugin installed and enabled");
    }catch(const std::exception& error){notice(fromUtf8(error.what()),true);}
}

void PlayerWindow::browsePluginCatalog() {
    if(!online_)return;
    const auto id=online_->submit("plugins.catalog");
    serviceCallbacks_[id]=[this](const ServiceResult& response){
        if(!response.error.empty()){notice(fromUtf8(response.error),true);return;}
        const auto entries=response.data.is_array()?response.data:response.data.value("plugins",Json::array());
        std::vector<std::pair<std::wstring,std::wstring>> choices;
        for(const auto& entry:entries)choices.push_back({fromUtf8(entry.value("name","Plugin")),fromUtf8(entry.value("desc",""))});
        if(choices.empty()){notice(L"The plugin catalog contains no entries.");return;}
        if(const auto chosen=chooseListItem(window_,L"Plugin catalog",choices,L"Search by name or description. Installation requests permission approval."))downloadPluginRepository(entries[*chosen].at("url").get<std::string>());
    };
    notice(L"Loading the plugin catalog\u2026");
}

void PlayerWindow::downloadPluginRepository(std::string repository) {
    std::smatch match;
    if(!std::regex_match(repository,match,std::regex("https://github\\.com/([A-Za-z0-9_.-]+)/([A-Za-z0-9_.-]+)/?"))){notice(L"Plugin catalogs must reference a GitHub repository.",true);return;}
    const auto base="https://api.github.com/repos/"+match[1].str()+"/"+match[2].str();
    const auto id=online_->submit("http.request",{{"url",base+"/releases/latest"},{"headers",{{"Accept","application/vnd.github+json"}}}});
    serviceCallbacks_[id]=[this,base,repository](const ServiceResult& response){
        if(!response.error.empty()){notice(fromUtf8(response.error),true);return;}
        auto url=base+"/zipball";std::string digest;
        const auto body=response.data.value("data",Json::object());
        for(const auto& asset:body.value("assets",Json::array())){
            const auto name=asset.value("name","");if(name.ends_with(".iinaplgz") || name.ends_with(".zip")){url=asset.at("browser_download_url").get<std::string>();digest=asset.value("digest","");if(name.ends_with(".iinaplgz"))break;}
        }
        const auto folder=dataDirectory_/L"PluginDownloads";std::filesystem::create_directories(folder);
        const auto file=folder/(std::to_wstring(GetTickCount64())+L".iinaplgz");Json request{{"url",url},{"destination",toUtf8(file.wstring())},{"maximumBytes",64*1024*1024}};
        const bool verified=digest.starts_with("sha256:") && digest.size()==71;if(verified)request["sha256"]=digest.substr(7);
        const auto download=online_->submit(verified?"download.verified":"plugin.download",request);
        serviceCallbacks_[download]=[this,file,repository](const ServiceResult& result){if(!result.error.empty())notice(fromUtf8(result.error),true);else installPluginPackage(file,repository);};
        notice(L"Downloading plugin package\u2026");
    };
    notice(L"Checking the plugin source\u2026");
}

void PlayerWindow::searchSubtitles() {
    if (!online_ || engine_.state().idle) { notice(L"Open media before searching for subtitles.",true); return; }
    const bool assrt=preferences_.subtitleProvider=="assrt";
    if ((assrt && preferences_.subtitleProviderToken.empty()) || (!assrt && preferences_.subtitleProviderApiKey.empty())) {
        showPreferences("Subtitles");
        if ((assrt && preferences_.subtitleProviderToken.empty()) || (!assrt && preferences_.subtitleProviderApiKey.empty())) return;
    }
    const auto query=showTextPrompt(window_,L"Search online subtitles",L"Movie or episode title",fromUtf8(engine_.state().title));
    if (!query || query->empty()) return;
    Json parameters{{"query",toUtf8(*query)}};
    if (!preferences_.subtitleLanguages.empty()) parameters["languages"]=preferences_.subtitleLanguages;
    const std::string provider=assrt?"assrt":"opensubtitles";
    const auto id=online_->submit(provider+".search",parameters);
    serviceCallbacks_[id]=[this,provider](const ServiceResult& result) {
        if (!result.error.empty()) { notice(fromUtf8(result.error),true); return; }
        const auto results=result.data.value("results",Json::array());
        if (results.empty()) { notice(L"No subtitles found."); return; }
        std::vector<std::pair<std::wstring,std::wstring>> rows;
        for (const auto& item:results) rows.push_back({fromUtf8(item.value("title","Subtitle")),fromUtf8(item.value("language",""))});
        const auto selected=chooseListItem(window_,L"Subtitle search results",rows);
        if (!selected) return;
        const auto item=results[*selected];
        const auto directory=dataDirectory_/L"Subtitles"; std::filesystem::create_directories(directory);
        // Provider filenames are untrusted; use only the final component.
        auto name=std::filesystem::path(fromUtf8(item.value("filename","subtitle.srt"))).filename();
        if (name.empty() || name==L"." || name==L"..") name=L"subtitle.srt";
        const auto target=directory/(std::to_wstring(GetTickCount64())+L"-"+name.wstring());
        Json request{{"destination",toUtf8(target.wstring())}};
        if (provider=="opensubtitles") request["file_id"]=item.at("file_id"); else request["id"]=item.at("id");
        const auto download=online_->submit(provider+".download",request);
        serviceCallbacks_[download]=[this](const ServiceResult& response) {
            if (!response.error.empty()) notice(fromUtf8(response.error),true);
            else { engine_.addSubtitle(response.data.at("path").get<std::string>()); notice(L"Subtitle downloaded and selected"); }
        };
        notice(L"Downloading subtitle\u2026");
    };
    notice(L"Searching for subtitles\u2026");
}
} // namespace velin
