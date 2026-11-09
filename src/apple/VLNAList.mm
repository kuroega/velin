// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
// Objective-C face of velin::alist for the iOS app. Built with ARC.
#import "velin/apple/VLNPlayer.h"

#include "velin/alist.hpp"
#include "velin/system.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

NSErrorDomain const VLNAListErrorDomain = @"app.velin.alist";

namespace {
NSString* ns(const std::string& text) {
    NSString* result = [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
    return result ? result : @"";
}
std::string utf8(NSString* text) {
    const char* bytes = text.UTF8String;
    return bytes ? std::string(bytes) : std::string{};
}
NSError* makeError(VLNAListErrorCode code, const std::string& message) {
    return [NSError errorWithDomain:VLNAListErrorDomain code:code userInfo:@{NSLocalizedDescriptionKey: ns(message)}];
}
VLNAListErrorCode codeFor(velin::alist::ErrorKind kind) {
    using velin::alist::ErrorKind;
    switch (kind) {
    case ErrorKind::InvalidServer: return VLNAListErrorInvalidServer;
    case ErrorKind::Transport: return VLNAListErrorTransport;
    case ErrorKind::InvalidResponse: return VLNAListErrorInvalidResponse;
    case ErrorKind::Unauthorized: return VLNAListErrorUnauthorized;
    case ErrorKind::OtpRequired: return VLNAListErrorOtpRequired;
    case ErrorKind::Forbidden: return VLNAListErrorForbidden;
    case ErrorKind::RateLimited: return VLNAListErrorRateLimited;
    case ErrorKind::Server: return VLNAListErrorServer;
    }
    return VLNAListErrorServer;
}
std::filesystem::path sessionPath() { return velin::alist::sessionFile(velin::sys::dataDirectory()); }
} // namespace

NSString* VLNAListLink(NSString* path) {
    NSMutableCharacterSet* allowed = [NSMutableCharacterSet alphanumericCharacterSet];
    [allowed addCharactersInString:@"-._~/"];
    NSString* encoded = [path stringByAddingPercentEncodingWithAllowedCharacters:allowed];
    return [@"velin://alist?path=" stringByAppendingString:encoded ? encoded : @"/"];
}

@interface VLNAListEntry ()
- (instancetype)initWithEntry:(const velin::alist::Entry&)entry;
@end

@implementation VLNAListEntry
- (instancetype)initWithEntry:(const velin::alist::Entry&)entry {
    if ((self = [super init])) {
        _name = ns(entry.name);
        _path = ns(entry.path);
        _directory = entry.isDirectory;
        _playable = !entry.isDirectory && velin::alist::isPlayable(entry);
        _size = entry.size;
        _modified = ns(entry.modified);
    }
    return self;
}
@end

@interface VLNAListFolder () {
@public
    std::vector<velin::alist::Entry> _raw;  // For building playlists.
}
- (instancetype)initWithPage:(const velin::alist::DirectoryPage&)page;
@end

@implementation VLNAListFolder
- (instancetype)initWithPage:(const velin::alist::DirectoryPage&)page {
    if ((self = [super init])) {
        _path = ns(page.path);
        _raw = page.entries;
        velin::alist::sortEntries(_raw);
        NSMutableArray<VLNAListEntry*>* entries = [NSMutableArray array];
        for (const auto& entry : _raw) [entries addObject:[[VLNAListEntry alloc] initWithEntry:entry]];
        _entries = entries;
    }
    return self;
}
@end

@implementation VLNAList {
    std::optional<velin::alist::Session> _session;
    std::map<std::string, std::string> _folderPasswords;
    dispatch_queue_t _queue;  // Requests block; run them off the main thread.
}

- (instancetype)init {
    if ((self = [super init])) {
        _queue = dispatch_queue_create("app.velin.alist", DISPATCH_QUEUE_SERIAL);
        std::string error;
        _session = velin::alist::loadSession(sessionPath(), error);
        if (!error.empty()) NSLog(@"Velin AList: %s", error.c_str());
    }
    return self;
}

- (BOOL)isSignedIn { return _session && !_session->token.empty(); }
- (NSString*)server { return _session ? ns(_session->server) : @""; }
- (NSString*)username { return _session ? ns(_session->username) : @""; }
- (NSString*)lastFolder { return _session && !_session->folder.empty() ? ns(_session->folder) : @"/"; }

- (void)save {
    if (!_session) return;
    std::string error;
    if (!velin::alist::saveSession(sessionPath(), *_session, error)) NSLog(@"Velin AList: %s", error.c_str());
}

- (void)signInToServer:(NSString*)server username:(NSString*)username password:(NSString*)password
               otpCode:(NSString*)otpCode completion:(void (^)(NSError*))completion {
    std::string normalized;
    std::string clientId = _session ? _session->clientId : std::string{};
    try {
        normalized = velin::alist::normalizeServerUrl(utf8(server));
        if (clientId.empty()) clientId = velin::alist::generateClientId();
    } catch (const velin::alist::Error& error) {
        completion(makeError(codeFor(error.kind()), error.what()));
        return;
    } catch (const std::exception& error) {
        completion(makeError(VLNAListErrorServer, error.what()));
        return;
    }
    const auto user = utf8(username), secret = utf8(password), otp = utf8(otpCode ? otpCode : @"");
    __weak VLNAList* weakSelf = self;
    dispatch_async(_queue, ^{
        std::string token;
        NSError* failure = nil;
        try {
            velin::alist::Client client(normalized, clientId);
            token = client.login(user, secret, otp);
        } catch (const velin::alist::Error& error) {
            failure = makeError(codeFor(error.kind()), error.what());
        } catch (const std::exception& error) {
            failure = makeError(VLNAListErrorTransport, error.what());
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            VLNAList* strongSelf = weakSelf;
            if (strongSelf && !failure) [strongSelf finishSignIn:normalized username:user clientId:clientId token:token];
            completion(failure);
        });
    });
}

- (void)finishSignIn:(const std::string&)server username:(const std::string&)username
            clientId:(const std::string&)clientId token:(const std::string&)token {
    // Reopen the last folder when signing back in to the same account.
    std::string folder;
    if (_session && _session->server == server && _session->username == username) folder = _session->folder;
    _session = velin::alist::Session{server, username, clientId, token, folder};
    _folderPasswords.clear();
    [self save];
}

- (void)signOut {
    std::string error;
    velin::alist::removeSession(sessionPath(), error);
    // Keep the server and username so signing in again is quick.
    if (_session) { _session->token.clear(); _session->folder.clear(); }
    _folderPasswords.clear();
}

- (void)setPassword:(NSString*)password forFolder:(NSString*)path {
    try { _folderPasswords[velin::alist::normalizePath(utf8(path))] = utf8(password); } catch (const std::exception&) {}
}

- (void)listFolder:(NSString*)path refresh:(BOOL)refresh completion:(void (^)(VLNAListFolder*, NSError*))completion {
    if (!self.signedIn) { completion(nil, makeError(VLNAListErrorUnauthorized, "Sign in to the AList server first")); return; }
    std::string folder;
    try { folder = velin::alist::normalizePath(utf8(path)); }
    catch (const velin::alist::Error& error) { completion(nil, makeError(codeFor(error.kind()), error.what())); return; }
    // A folder password also unlocks the folders beneath it.
    std::string password;
    for (auto current = folder;; current = velin::alist::parentPath(current)) {
        if (const auto found = _folderPasswords.find(current); found != _folderPasswords.end()) { password = found->second; break; }
        if (current == "/") break;
    }
    const auto session = *_session;
    __weak VLNAList* weakSelf = self;
    dispatch_async(_queue, ^{
        std::optional<velin::alist::DirectoryPage> page;
        NSError* failure = nil;
        bool unauthorized = false;
        try {
            velin::alist::Client client(session.server, session.clientId, session.token);
            page = client.list(folder, 1, 0, refresh, password);
        } catch (const velin::alist::Error& error) {
            failure = makeError(codeFor(error.kind()), error.what());
            unauthorized = error.kind() == velin::alist::ErrorKind::Unauthorized;
        } catch (const std::exception& error) {
            failure = makeError(VLNAListErrorTransport, error.what());
        }
        VLNAListFolder* result = page ? [[VLNAListFolder alloc] initWithPage:*page] : nil;
        dispatch_async(dispatch_get_main_queue(), ^{
            VLNAList* strongSelf = weakSelf;
            if (strongSelf && strongSelf->_session && strongSelf->_session->token == session.token) {
                if (unauthorized) { strongSelf->_session->token.clear(); [strongSelf save]; }
                else if (result && strongSelf->_session->folder != folder) { strongSelf->_session->folder = folder; [strongSelf save]; }
            }
            completion(result, failure);
        });
    });
}

- (NSArray<VLNMediaItem*>*)playlistForFolder:(VLNAListFolder*)folder startingAt:(NSString*)path startIndex:(NSInteger*)startIndex {
    if (!_session) return nil;
    velin::alist::Playlist playlist;
    try { playlist = velin::alist::buildPlaylist(_session->server, folder->_raw, utf8(path)); }
    catch (const std::exception&) { return nil; }
    if (playlist.items.empty()) return nil;
    NSMutableArray<VLNMediaItem*>* items = [NSMutableArray array];
    for (const auto& entry : playlist.items) {
        NSURL* url = [NSURL URLWithString:ns(entry.url)];
        if (!url) continue;
        NSString* itemPath = ns(entry.path);
        VLNMediaItem* item = [[VLNMediaItem alloc] initWithURL:url title:itemPath.lastPathComponent];
        item.historyKey = VLNAListLink(itemPath);
        NSMutableArray<NSURL*>* subtitles = [NSMutableArray array];
        NSMutableArray<NSString*>* names = [NSMutableArray array];
        for (std::size_t i = 0; i < entry.subtitles.size(); ++i) {
            NSURL* subtitle = [NSURL URLWithString:ns(entry.subtitles[i])];
            if (!subtitle) continue;
            [subtitles addObject:subtitle];
            [names addObject:i < entry.subtitleNames.size() ? ns(entry.subtitleNames[i]) : @""];
        }
        item.subtitleURLs = subtitles;
        item.subtitleNames = names;
        [items addObject:item];
    }
    if (startIndex) *startIndex = static_cast<NSInteger>(std::min(playlist.start, static_cast<std::size_t>(items.count ? items.count - 1 : 0)));
    return items.count ? items : nil;
}
@end
