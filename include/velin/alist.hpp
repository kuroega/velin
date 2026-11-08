// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Client for an AList server: sign-in, browsing and playable file URLs.
// Requests are blocking and intended for OnlineServices workers.
#include "velin/extensions.hpp"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace velin::alist {

enum class ErrorKind {
    InvalidServer,   // The server URL is not a plain http(s) origin.
    Transport,       // No HTTP response (refused, timed out, cancelled).
    InvalidResponse, // The reply is not an AList JSON envelope.
    Unauthorized,    // 401: sign in again.
    OtpRequired,     // 402: the account needs a two-factor code.
    Forbidden,       // 403: no permission, wrong folder password, or too many devices at sign-in.
    RateLimited,     // 429
    Server           // Any other non-200 code.
};

class Error : public std::runtime_error {
public:
    Error(ErrorKind kind, int code, const std::string& message)
        : std::runtime_error(message), kind_(kind), code_(code) {}
    [[nodiscard]] ErrorKind kind() const { return kind_; }
    [[nodiscard]] int code() const { return code_; }
private:
    ErrorKind kind_;
    int code_;
};
const char* errorKindName(ErrorKind kind);

struct Entry {
    std::string name;
    std::string path;     // Absolute AList path of this entry.
    bool isDirectory = false;
    std::int64_t size = 0;
    std::string modified; // RFC 3339 as sent by the server.
    std::string sign;     // Empty when the server does not require signed links.
    std::string thumb;
    int type = 0;         // AList file type: 0 unknown, 1 folder, 2 video, 3 audio, 4 text, 5 image.
};

struct DirectoryPage {
    std::string path;
    std::vector<Entry> entries;
    std::int64_t total = 0;
    bool hasMore = false;
    std::string provider;
    bool writable = false;
};

struct FileDetail {
    Entry entry;
    std::string rawUrl;
    // Set when the server replaced rawUrl with its re-encoded compatibility
    // stream; this is the untouched file, which libmpv plays directly.
    std::string originalUrl;
    std::string provider;
    std::vector<Entry> related; // Other files in the same folder.
};

struct User {
    std::int64_t id = 0;
    std::string username;
    std::string basePath = "/";
    std::vector<int> roles;
    bool disabled = false;
};

// "10.0.0.5:5244/" -> "http://10.0.0.5:5244"; keeps a sub-path such as
// "https://host/alist". Throws Error(InvalidServer) for credentials, query,
// fragment or a scheme other than http/https.
std::string normalizeServerUrl(std::string_view input);
// Always absolute, single slashes, no trailing slash except the root.
// Throws Error(InvalidServer) for "." or ".." segments.
std::string normalizePath(std::string_view path);
std::string joinPath(std::string_view directory, std::string_view name);
std::string parentPath(std::string_view path);
// Percent-encodes each segment and keeps the separators.
std::string encodePath(std::string_view path);
// <server>/d<encoded path>[?sign=...], the same link AList's "Copy link" gives.
std::string downloadUrl(std::string_view server, std::string_view path, std::string_view sign = {});
// The URL to hand libmpv: the original file, never the compatibility stream.
std::string playbackUrl(std::string_view server, const FileDetail& detail);
std::string playbackUrl(std::string_view server, const Entry& entry);
// AList stores password hashes of sha256("<password>-https://github.com/alist-org/alist").
std::string passwordHash(std::string_view password);
bool isPlayable(const Entry& entry);
// Subtitle siblings whose names start with the video's stem, e.g. "Ep01.mkv"
// matches "Ep01.ass" and "Ep01.zh-Hans.srt". Sorted by name.
std::vector<Entry> subtitlesFor(const Entry& video, const std::vector<Entry>& siblings);
// 32 lowercase hex characters from the system RNG; sent as the Client-Id.
std::string generateClientId();

// Folders first, then everything else, each in natural order.
void sortEntries(std::vector<Entry>& entries);
// One folder queued for playback: every playable file in natural order, each
// with the signed links of its subtitle siblings, starting at the chosen file.
struct PlaylistItem {
    std::string path;                   // AList path, for display and re-signing.
    std::string url;                    // What libmpv opens.
    std::vector<std::string> subtitles; // Subtitle links, in subtitlesFor() order.
    std::vector<std::string> subtitleNames;
};
struct Playlist {
    std::vector<PlaylistItem> items;
    std::size_t start = 0;
};
// Empty when chosenPath is not a playable file among entries.
Playlist buildPlaylist(std::string_view server, const std::vector<Entry>& entries, std::string_view chosenPath);
// The subtitle to select among names, following mpv's slang list
// ("zh-Hans,chi,eng"): the first language that matches one of the name's
// inner dot-separated tags, such as "Ep01.zh-Hans.ass" or "Ep01.eng.srt". Tags
// match whole, or by their leading subtag ("zh" matches "zh-Hans").
// Falls back to the first subtitle; 0 for an empty list.
std::size_t preferredSubtitle(const std::vector<std::string>& names, std::string_view languages);

// Parsers for the "data" member of AList's {code, message, data} envelope.
// directory is the listed folder; entries get absolute paths beneath it.
DirectoryPage parseDirectory(const Json& data, std::string_view directory);
FileDetail parseFileDetail(const Json& data, std::string_view path);
User parseUser(const Json& data);
// Returns "data" for code 200, otherwise throws Error.
Json unwrap(unsigned httpStatus, std::string_view body);

class Client {
public:
    Client(std::string server, std::string clientId, std::string token = {});
    // Uses the hashed sign-in endpoint so the password never crosses the
    // network. Stores and returns the new token.
    std::string login(std::string_view username, std::string_view password,
                      std::string_view otpCode = {}, std::stop_token stop = {});
    User me(std::stop_token stop = {});
    // perPage 0 asks for the whole folder.
    DirectoryPage list(std::string_view path, int page = 1, int perPage = 0, bool refresh = false,
                       std::string_view folderPassword = {}, std::stop_token stop = {});
    FileDetail get(std::string_view path, std::string_view folderPassword = {}, std::stop_token stop = {});

    [[nodiscard]] const std::string& server() const { return server_; }
    [[nodiscard]] const std::string& token() const { return token_; }
    void setTimeout(int milliseconds) { timeoutMilliseconds_ = milliseconds; }
private:
    Json call(const std::string& apiPath, const Json& body, bool authenticated, std::stop_token stop);
    std::string server_, clientId_, token_;
    int timeoutMilliseconds_ = 15000;
};

// A saved sign-in. The file is encrypted with Windows DPAPI for the current
// user, so another account or machine cannot read the token.
struct Session {
    std::string server;
    std::string username;
    std::string clientId;
    std::string token;
    std::string folder; // The last folder browsed, reopened next time.
};
std::string protectSecret(std::string_view plain);
std::string unprotectSecret(std::string_view protectedBytes);
bool saveSession(const std::filesystem::path& file, const Session& session, std::string& error);
// std::nullopt with an empty error when the file does not exist.
std::optional<Session> loadSession(const std::filesystem::path& file, std::string& error);
bool removeSession(const std::filesystem::path& file, std::string& error);
std::filesystem::path sessionFile(const std::filesystem::path& dataDirectory);

} // namespace velin::alist

namespace velin::alist {
Json toJson(const Entry& entry);
// Reads the toJson(Entry) shape back; missing members keep their defaults.
Entry entryFromJson(const Json& json);
Json toJson(const User& user);
// Runs an OnlineServices "alist.*" operation:
//   alist.login {server, clientId, username, password, otpCode?} -> {token, user}
//   alist.me    {server, clientId, token} -> {user}
//   alist.list  {server, clientId, token, path, page?, perPage?, refresh?, folderPassword?}
//               -> {path, entries[{..., playbackUrl?}], total, hasMore, provider, writable}
//   alist.get   {server, clientId, token, path, folderPassword?}
//               -> {entry, playbackUrl, rawUrl, originalUrl, related[], subtitles[]}
// Every reply carries "ok". AList failures are data, not exceptions, so the UI
// can react to the kind: {ok:false, error:{kind, code, message}} where kind is
// errorKindName(). Submit alist.login with privateResult, since it holds the token.
Json serviceOperation(const std::string& operation, const Json& parameters, std::stop_token stop);
} // namespace velin::alist
