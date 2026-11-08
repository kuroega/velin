// SPDX-License-Identifier: GPL-3.0-or-later
// Browsing and playing files from an AList server.
#include "velin/player_window.hpp"
#include <algorithm>
#include <cstdio>

namespace velin {
namespace {
constexpr char kDefaultServer[]="http://10.127.1.109:5244";

std::string errorKind(const Json& reply) {
    const auto error=reply.value("error",Json::object());
    return error.is_object()?error.value("kind",std::string{}):std::string{};
}
std::wstring errorMessage(const Json& reply) {
    const auto error=reply.value("error",Json::object());
    auto message=error.is_object()?error.value("message",std::string{}):std::string{};
    if (message.empty()) message="AList did not accept the request.";
    return fromUtf8(message);
}
std::wstring byteSize(std::int64_t bytes) {
    constexpr const wchar_t* units[]={L"B",L"KB",L"MB",L"GB",L"TB"};
    double value=static_cast<double>(std::max<std::int64_t>(bytes,0)); int unit=0;
    while (value>=1024 && unit<4) { value/=1024; ++unit; }
    wchar_t text[32]; std::swprintf(text,32,unit==0?L"%.0f %ls":L"%.1f %ls",value,units[unit]);
    return text;
}
std::string normalizedOrRoot(const std::string& path) {
    try { return alist::normalizePath(path); } catch (const alist::Error&) { return "/"; }
}
} // namespace

bool PlayerWindow::loadAListSession() {
    if (alistSession_) return !alistSession_->token.empty();
    std::string error;
    alistSession_=alist::loadSession(alist::sessionFile(dataDirectory_),error);
    if (!error.empty()) logLines_.push_back("AList session: "+error);
    return alistSession_ && !alistSession_->token.empty();
}

void PlayerWindow::saveAListSession() {
    if (!alistSession_) return;
    std::string error;
    if (!alist::saveSession(alist::sessionFile(dataDirectory_),*alistSession_,error))
        notice(L"Could not save the AList sign-in: "+fromUtf8(error),true);
}

void PlayerWindow::browseAList(std::string folder) {
    if (!online_) return;
    if (alistBusy_) { notice(L"Still waiting for the AList server…"); return; }
    if (!loadAListSession()) { signInAList([this,folder] { browseAList(folder); }); return; }
    // The remembered folder may have been moved or deleted since; fall back
    // to the root instead of failing.
    const bool remembered=folder.empty();
    if (remembered) folder=alistSession_->folder.empty()?"/":alistSession_->folder;
    listAListFolder(folder,remembered);
}

void PlayerWindow::signInAList(std::function<void()> then,std::wstring message) {
    SignInForm initial{fromUtf8(kDefaultServer),{},{}};
    if (alistSession_ && !alistSession_->server.empty()) {
        initial.server=fromUtf8(alistSession_->server); initial.username=fromUtf8(alistSession_->username);
    }
    auto form=showSignInPrompt(window_,L"Sign in to AList",message,initial);
    if (!form) return;
    auto password=toUtf8(form->password);
    SecureZeroMemory(form->password.data(),form->password.size()*sizeof(wchar_t));
    std::string server;
    try { server=alist::normalizeServerUrl(toUtf8(form->server)); }
    catch (const alist::Error& error) { signInAList(std::move(then),fromUtf8(error.what())); return; }
    const auto username=toUtf8(form->username);
    if (username.empty()) { signInAList(std::move(then),L"Enter your AList username."); return; }
    submitAListLogin(server,username,std::move(password),{},std::move(then));
}

void PlayerWindow::submitAListLogin(std::string server,std::string username,std::string password,
                                    std::string otpCode,std::function<void()> then) {
    // The Client-Id identifies this installation to AList's device limit, so
    // it survives signing in again or to another account.
    std::string clientId=alistSession_?alistSession_->clientId:std::string{};
    if (clientId.empty()) {
        try { clientId=alist::generateClientId(); }
        catch (const std::exception& error) { notice(fromUtf8(error.what()),true); return; }
    }
    Json parameters{{"server",server},{"clientId",clientId},{"username",username},{"password",password}};
    if (!otpCode.empty()) parameters["otpCode"]=otpCode;
    const auto id=online_->submit("alist.login",std::move(parameters),true);
    alistBusy_=true;
    serviceCallbacks_[id]=[this,server,username,password,clientId,then](const ServiceResult& result) mutable {
        alistBusy_=false;
        auto forget=[&] { SecureZeroMemory(password.data(),password.size()); };
        if (!result.error.empty()) { forget(); signInAList(then,fromUtf8(result.error)); return; }
        if (!result.data.value("ok",false)) {
            if (errorKind(result.data)=="otp-required") {
                const auto code=showTextPrompt(window_,L"Two-factor code",
                    L"Enter the code from your authenticator app for "+fromUtf8(username)+L".");
                if (code && !code->empty()) submitAListLogin(server,username,password,toUtf8(*code),then);
                forget(); return;
            }
            forget(); signInAList(then,errorMessage(result.data)); return;
        }
        forget();
        const auto signedServer=result.data.value("server",server);
        // Keep the remembered folder only while it belongs to the same server.
        std::string folder;
        if (alistSession_ && alistSession_->server==signedServer && alistSession_->username==username) folder=alistSession_->folder;
        alistSession_=alist::Session{signedServer,username,clientId,result.data.value("token",std::string{}),folder};
        alistFolderPasswords_.clear();
        saveAListSession();
        notice(L"Signed in to AList as "+fromUtf8(username));
        if (then) then();
    };
    notice(L"Signing in to AList…");
}

void PlayerWindow::listAListFolder(std::string folder,bool fallBackToRoot) {
    if (!online_ || !alistSession_) return;
    folder=normalizedOrRoot(folder);
    Json parameters{{"server",alistSession_->server},{"clientId",alistSession_->clientId},
                    {"token",alistSession_->token},{"path",folder}};
    // A folder password usually protects its subfolders too.
    for (auto path=folder;;path=alist::parentPath(path)) {
        if (const auto found=alistFolderPasswords_.find(path);found!=alistFolderPasswords_.end()) {
            parameters["folderPassword"]=found->second; break;
        }
        if (path=="/") break;
    }
    const auto id=online_->submit("alist.list",std::move(parameters));
    alistBusy_=true;
    serviceCallbacks_[id]=[this,folder,fallBackToRoot](const ServiceResult& result) {
        alistBusy_=false;
        if (!alistSession_) return;
        if (!result.error.empty()) { notice(fromUtf8(result.error),true); return; }
        const auto& reply=result.data;
        if (!reply.value("ok",false)) {
            const auto kind=errorKind(reply);
            if (kind=="unauthorized") {
                // Tokens last 48 hours, and restarting AList invalidates them all.
                alistSession_->token.clear(); saveAListSession();
                signInAList([this,folder,fallBackToRoot] { listAListFolder(folder,fallBackToRoot); },
                            L"Your AList sign-in has expired. Sign in again to continue.");
                return;
            }
            if (kind=="forbidden") {
                const auto password=showTextPrompt(window_,L"Folder password",
                    errorMessage(reply)+L"\nIf “"+fromUtf8(folder)+L"” has a password, enter it here.",{},false,true);
                if (!password) return;
                alistFolderPasswords_[folder]=toUtf8(*password);
                listAListFolder(folder,false); return;
            }
            if (fallBackToRoot && folder!="/") { listAListFolder("/",false); return; }
            notice(errorMessage(reply),true); return;
        }
        if (alistSession_->folder!=folder) { alistSession_->folder=folder; saveAListSession(); }

        std::vector<alist::Entry> entries;
        for (const auto& item:reply.value("entries",Json::array())) entries.push_back(alist::entryFromJson(item));
        alist::sortEntries(entries);
        std::vector<std::pair<std::wstring,std::wstring>> rows;
        std::vector<std::string> targets; // Folder to open, or "" with the file in files.
        std::vector<const alist::Entry*> files;
        if (folder!="/") { rows.push_back({L"..",L"Up to "+fromUtf8(alist::parentPath(folder))}); targets.push_back(alist::parentPath(folder)); files.push_back(nullptr); }
        for (const auto& entry:entries) {
            if (entry.isDirectory) { rows.push_back({fromUtf8(entry.name)+L"/",L"Folder"}); targets.push_back(entry.path); files.push_back(nullptr); }
            else if (alist::isPlayable(entry)) {
                std::wstring detail=byteSize(entry.size);
                if (entry.modified.size()>=10) detail+=L"   "+fromUtf8(entry.modified.substr(0,10));
                rows.push_back({fromUtf8(entry.name),detail}); targets.push_back({}); files.push_back(&entry);
            }
        }
        const bool empty=rows.size()==(folder=="/"?0u:1u);
        if (rows.empty()) { notice(L"This AList server has no folders or playable files at its root.",true); return; }
        const auto caption=fromUtf8(alistSession_->server+folder)+(empty?L"   \u2014   nothing playable here":L"");
        const auto title=folder=="/"?std::wstring(L"AList"):fromUtf8(folder.substr(folder.rfind('/')+1));
        const auto chosen=chooseListItem(window_,title,rows,caption);
        if (!chosen) return;
        if (files[*chosen]) playAListFolder(reply.value("entries",Json::array()),folder,files[*chosen]->path);
        else listAListFolder(targets[*chosen],false);
    };
}

void PlayerWindow::playAListFolder(const Json& entriesJson,const std::string& folder,const std::string& chosen) {
    if (!alistSession_ || !engine_.ready()) { notice(fromUtf8(engine_.error()),true); return; }
    std::vector<alist::Entry> entries;
    for (const auto& item:entriesJson) entries.push_back(alist::entryFromJson(item));
    alist::Playlist playlist;
    try { playlist=alist::buildPlaylist(alistSession_->server,entries,chosen); }
    catch (const std::exception& error) { notice(fromUtf8(error.what()),true); return; }
    if (playlist.items.empty()) { notice(L"That file is no longer in the folder.",true); return; }
    alistSubtitles_.clear(); alistSubtitlesFor_.clear();
    for (const auto& item:playlist.items)
        if (!item.subtitles.empty()) alistSubtitles_[item.url]={item.subtitles,item.subtitleNames};
    // Load the chosen file first so nothing else starts streaming, then queue
    // the files after it, and finally the earlier ones moved to the front.
    openPaths({playlist.items[playlist.start].url});
    std::vector<std::string> after,before;
    for (std::size_t i=0;i<playlist.items.size();++i)
        if (i!=playlist.start) (i<playlist.start?before:after).push_back(playlist.items[i].url);
    if (!after.empty()) engine_.open(after,true);
    if (!before.empty()) {
        engine_.open(before,true);
        const auto total=static_cast<std::int64_t>(playlist.items.size()),earlier=static_cast<std::int64_t>(before.size());
        for (std::int64_t i=0;i<earlier;++i) engine_.movePlaylist(total-earlier+i,i);
    }
    if (playlist.items.size()>1)
        notice(L"Playing "+std::to_wstring(playlist.start+1)+L" of "+std::to_wstring(playlist.items.size())+L" from "+fromUtf8(folder));
}

void PlayerWindow::applyAListSubtitles() {
    const auto& state=engine_.state();
    if (state.idle) { alistSubtitlesFor_.clear(); return; }
    if (!state.loaded || state.path==alistSubtitlesFor_) return;
    alistSubtitlesFor_=state.path;
    const auto found=alistSubtitles_.find(state.path);
    if (found==alistSubtitles_.end()) return;
    const auto& [urls,names]=found->second;
    const auto selected=alist::preferredSubtitle(names,preferences_.subtitleLanguages);
    for (std::size_t i=0;i<urls.size();++i)
        engine_.command({"sub-add",urls[i],i==selected?"select":"auto",i<names.size()?names[i]:std::string{}});
}

void PlayerWindow::signOutAList() {
    std::string error;
    if (!alist::removeSession(alist::sessionFile(dataDirectory_),error)) { notice(fromUtf8(error),true); return; }
    // Keep the server and username in memory so signing in again is quick.
    if (alistSession_) { alistSession_->token.clear(); alistSession_->folder.clear(); }
    alistFolderPasswords_.clear();
    notice(L"Signed out of AList");
}
} // namespace velin
