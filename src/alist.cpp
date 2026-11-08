// SPDX-License-Identifier: GPL-3.0-or-later
#include "velin/alist.hpp"
#include "velin/core.hpp"
#include "velin/library.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <dpapi.h>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

namespace velin::alist {
namespace {
constexpr std::string_view kSessionMagic = "VELIN-ALIST-1\n";
constexpr std::string_view kPasswordSalt = "https://github.com/alist-org/alist";

[[noreturn]] void fail(ErrorKind kind, int code, const std::string& message) { throw Error(kind, code, message); }

std::string lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}
std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}
std::string hex(const unsigned char* bytes, std::size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        result += digits[bytes[i] >> 4];
        result += digits[bytes[i] & 15];
    }
    return result;
}
std::string sha256(std::string_view data) {
    unsigned char digest[32];
    const auto status = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
        reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()),
        digest, sizeof(digest));
    if (status < 0) throw std::runtime_error("SHA-256 failed");
    return hex(digest, sizeof(digest));
}
std::string stem(std::string_view name) {
    const auto dot = name.rfind('.');
    return std::string(dot == std::string_view::npos || dot == 0 ? name : name.substr(0, dot));
}
std::string text(const Json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}
template <typename T>
T number(const Json& object, const char* key, T fallback = {}) {
    const auto it = object.find(key);
    return it != object.end() && it->is_number() ? it->get<T>() : fallback;
}
bool boolean(const Json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() && it->get<bool>();
}
Entry parseEntry(const Json& item, std::string path) {
    if (!item.is_object()) fail(ErrorKind::InvalidResponse, 0, "AList returned a malformed file entry");
    Entry entry;
    entry.name = text(item, "name");
    entry.path = std::move(path);
    entry.isDirectory = boolean(item, "is_dir");
    entry.size = number<std::int64_t>(item, "size");
    entry.modified = text(item, "modified");
    entry.sign = text(item, "sign");
    entry.thumb = text(item, "thumb");
    entry.type = number<int>(item, "type");
    return entry;
}
std::vector<Entry> parseEntries(const Json& data, const char* key, std::string_view directory) {
    std::vector<Entry> entries;
    const auto it = data.find(key);
    if (it == data.end() || it->is_null()) return entries; // AList sends null for an empty folder.
    if (!it->is_array()) fail(ErrorKind::InvalidResponse, 0, std::string("AList returned a malformed ") + key);
    entries.reserve(it->size());
    for (const auto& item : *it) {
        const auto name = text(item, "name");
        if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos) continue;
        entries.push_back(parseEntry(item, joinPath(directory, name)));
    }
    return entries;
}
ErrorKind kindForCode(int code) {
    switch (code) {
    case 401: return ErrorKind::Unauthorized;
    case 402: return ErrorKind::OtpRequired;
    case 403: return ErrorKind::Forbidden;
    case 429: return ErrorKind::RateLimited;
    default: return ErrorKind::Server;
    }
}
} // namespace

const char* errorKindName(ErrorKind kind) {
    switch (kind) {
    case ErrorKind::InvalidServer: return "invalid-server";
    case ErrorKind::Transport: return "transport";
    case ErrorKind::InvalidResponse: return "invalid-response";
    case ErrorKind::Unauthorized: return "unauthorized";
    case ErrorKind::OtpRequired: return "otp-required";
    case ErrorKind::Forbidden: return "forbidden";
    case ErrorKind::RateLimited: return "rate-limited";
    case ErrorKind::Server: return "server";
    }
    return "server";
}

std::string normalizeServerUrl(std::string_view input) {
    auto value = trim(input);
    if (value.empty()) fail(ErrorKind::InvalidServer, 0, "Enter the AList server address");
    if (value.find("://") == std::string::npos) value = "http://" + value;
    const auto schemeEnd = value.find("://");
    const auto scheme = lower(value.substr(0, schemeEnd));
    if (scheme != "http" && scheme != "https") fail(ErrorKind::InvalidServer, 0, "The AList server must use http or https");
    auto rest = value.substr(schemeEnd + 3);
    if (rest.find_first_of("?#") != std::string::npos) fail(ErrorKind::InvalidServer, 0, "The AList server address cannot contain a query or fragment");
    const auto slash = rest.find('/');
    auto authority = rest.substr(0, slash);
    auto path = slash == std::string::npos ? std::string{} : rest.substr(slash);
    if (authority.empty()) fail(ErrorKind::InvalidServer, 0, "The AList server address has no host");
    if (authority.find('@') != std::string::npos) fail(ErrorKind::InvalidServer, 0, "Put the username in the sign-in form, not the server address");
    if (std::any_of(authority.begin(), authority.end(), [](unsigned char c) { return std::isspace(c) || c < 0x20; }))
        fail(ErrorKind::InvalidServer, 0, "The AList server address contains spaces");
    while (!path.empty() && path.back() == '/') path.pop_back();
    if (!path.empty()) path = normalizePath(path);
    return scheme + "://" + lower(authority) + (path == "/" ? "" : path);
}

std::string normalizePath(std::string_view path) {
    std::vector<std::string_view> segments;
    std::size_t start = 0;
    while (start <= path.size()) {
        auto end = path.find_first_of("/\\", start);
        if (end == std::string_view::npos) end = path.size();
        const auto segment = path.substr(start, end - start);
        if (segment == "." || segment == "..") fail(ErrorKind::InvalidServer, 0, "AList paths cannot contain '.' or '..'");
        if (!segment.empty()) segments.push_back(segment);
        start = end + 1;
    }
    std::string result;
    for (const auto segment : segments) {
        result += '/';
        result += segment;
    }
    return result.empty() ? "/" : result;
}

std::string joinPath(std::string_view directory, std::string_view name) {
    auto base = normalizePath(directory);
    if (base != "/") base += '/';
    return normalizePath(base + std::string(name));
}

std::string parentPath(std::string_view path) {
    const auto normalized = normalizePath(path);
    const auto slash = normalized.rfind('/');
    return slash == 0 ? "/" : normalized.substr(0, slash);
}

std::string encodePath(std::string_view path) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : normalizePath(path)) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') result += static_cast<char>(c);
        else {
            result += '%';
            result += digits[c >> 4];
            result += digits[c & 15];
        }
    }
    return result;
}

std::string downloadUrl(std::string_view server, std::string_view path, std::string_view sign) {
    auto url = normalizeServerUrl(server) + "/d" + encodePath(path);
    if (!sign.empty()) {
        url += "?sign=";
        for (unsigned char c : sign) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == ':') url += static_cast<char>(c);
            else {
                static constexpr char digits[] = "0123456789ABCDEF";
                url += '%';
                url += digits[c >> 4];
                url += digits[c & 15];
            }
        }
    }
    return url;
}

std::string playbackUrl(std::string_view server, const FileDetail& detail) {
    if (!detail.originalUrl.empty()) return detail.originalUrl;
    if (!detail.rawUrl.empty()) return detail.rawUrl;
    return playbackUrl(server, detail.entry);
}

std::string playbackUrl(std::string_view server, const Entry& entry) {
    if (entry.isDirectory) fail(ErrorKind::InvalidResponse, 0, "A folder has no playback URL");
    return downloadUrl(server, entry.path, entry.sign);
}

std::string passwordHash(std::string_view password) {
    std::string salted(password);
    salted += '-';
    salted += kPasswordSalt;
    return sha256(salted);
}

bool isPlayable(const Entry& entry) {
    return !entry.isDirectory && (entry.type == 2 || entry.type == 3 || isMediaFile(entry.name));
}

std::vector<Entry> subtitlesFor(const Entry& video, const std::vector<Entry>& siblings) {
    const auto base = lower(stem(video.name));
    std::vector<Entry> result;
    if (base.empty()) return result;
    for (const auto& sibling : siblings) {
        if (sibling.isDirectory || !isSubtitleFile(sibling.name)) continue;
        const auto name = lower(sibling.name);
        // Same stem, followed by the extension or a language/variant suffix.
        if (name.size() > base.size() && name.starts_with(base) && name[base.size()] == '.') result.push_back(sibling);
    }
    std::sort(result.begin(), result.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
    return result;
}

std::string generateClientId() {
    unsigned char bytes[16];
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw std::runtime_error("Cannot generate a client identifier");
    return hex(bytes, sizeof(bytes));
}

DirectoryPage parseDirectory(const Json& data, std::string_view directory) {
    if (!data.is_object()) fail(ErrorKind::InvalidResponse, 0, "AList returned a malformed folder listing");
    DirectoryPage page;
    page.path = normalizePath(directory);
    page.entries = parseEntries(data, "content", page.path);
    page.total = number<std::int64_t>(data, "total", static_cast<std::int64_t>(page.entries.size()));
    page.hasMore = boolean(data, "has_more");
    page.provider = text(data, "provider");
    page.writable = boolean(data, "write");
    return page;
}

FileDetail parseFileDetail(const Json& data, std::string_view path) {
    if (!data.is_object()) fail(ErrorKind::InvalidResponse, 0, "AList returned malformed file details");
    FileDetail detail;
    const auto normalized = normalizePath(path);
    detail.entry = parseEntry(data, normalized);
    if (detail.entry.name.empty()) detail.entry.name = normalized.substr(normalized.rfind('/') + 1);
    detail.rawUrl = text(data, "raw_url");
    detail.originalUrl = text(data, "original_url");
    detail.provider = text(data, "provider");
    detail.related = parseEntries(data, "related", parentPath(normalized));
    return detail;
}

User parseUser(const Json& data) {
    if (!data.is_object()) fail(ErrorKind::InvalidResponse, 0, "AList returned a malformed user");
    User user;
    user.id = number<std::int64_t>(data, "id");
    user.username = text(data, "username");
    user.basePath = text(data, "base_path");
    if (user.basePath.empty()) user.basePath = "/";
    if (const auto it = data.find("role"); it != data.end()) {
        if (it->is_array()) {
            for (const auto& role : *it) if (role.is_number_integer()) user.roles.push_back(role.get<int>());
        } else if (it->is_number_integer()) user.roles.push_back(it->get<int>());
    }
    user.disabled = boolean(data, "disabled");
    return user;
}

Json unwrap(unsigned httpStatus, std::string_view body) {
    const auto json = Json::parse(body, nullptr, false);
    if (json.is_discarded() || !json.is_object() || !json.contains("code") || !json["code"].is_number_integer()) {
        // Gin answers some failures (401 from a proxy, 404 for an old server) without the envelope.
        if (httpStatus >= 400) fail(kindForCode(static_cast<int>(httpStatus)), static_cast<int>(httpStatus),
                                    "AList server returned HTTP " + std::to_string(httpStatus));
        fail(ErrorKind::InvalidResponse, static_cast<int>(httpStatus), "The server did not answer like AList");
    }
    const int code = json["code"].get<int>();
    auto message = text(json, "message");
    if (code == 200 && httpStatus >= 200 && httpStatus < 300) return json.contains("data") ? json["data"] : Json(nullptr);
    const int effective = code != 200 ? code : static_cast<int>(httpStatus);
    if (message.empty()) message = "AList request failed (" + std::to_string(effective) + ")";
    fail(kindForCode(effective), effective, message);
}

Client::Client(std::string server, std::string clientId, std::string token)
    : server_(normalizeServerUrl(server)), clientId_(std::move(clientId)), token_(std::move(token)) {}

Json Client::call(const std::string& apiPath, const Json& body, bool authenticated, std::stop_token stop) {
    HttpRequest request;
    request.method = "POST";
    request.url = server_ + apiPath;
    request.timeoutMilliseconds = timeoutMilliseconds_;
    request.maximumBytes = 64 * 1024 * 1024;
    request.headers = {{"Accept", "application/json"}, {"Content-Type", "application/json"},
                       {"User-Agent", "Velin/" VELIN_VERSION}};
    if (!clientId_.empty()) request.headers["Client-Id"] = clientId_;
    if (authenticated) {
        if (token_.empty()) fail(ErrorKind::Unauthorized, 401, "Sign in to the AList server first");
        request.headers["Authorization"] = token_; // AList expects the bare token.
    }
    request.body = body.dump();
    const auto response = httpRequest(request, stop);
    if (!response.error.empty()) fail(ErrorKind::Transport, 0, response.error);
    return unwrap(response.status, response.body);
}

std::string Client::login(std::string_view username, std::string_view password, std::string_view otpCode, std::stop_token stop) {
    if (trim(username).empty()) fail(ErrorKind::Unauthorized, 401, "Enter your AList username");
    Json body{{"username", std::string(username)}, {"password", passwordHash(password)}};
    if (!otpCode.empty()) body["otp_code"] = trim(otpCode);
    const auto data = call("/api/auth/login/hash", body, false, stop);
    auto token = data.is_object() ? text(data, "token") : std::string{};
    if (token.empty()) fail(ErrorKind::InvalidResponse, 0, "AList did not return a sign-in token");
    token_ = std::move(token);
    return token_;
}

User Client::me(std::stop_token stop) {
    // /api/me is registered for GET only, unlike the POST fs endpoints.
    HttpRequest request;
    request.url = server_ + "/api/me";
    request.timeoutMilliseconds = timeoutMilliseconds_;
    request.headers = {{"Accept", "application/json"}, {"User-Agent", "Velin/" VELIN_VERSION}};
    if (!clientId_.empty()) request.headers["Client-Id"] = clientId_;
    if (token_.empty()) fail(ErrorKind::Unauthorized, 401, "Sign in to the AList server first");
    request.headers["Authorization"] = token_;
    const auto response = httpRequest(request, stop);
    if (!response.error.empty()) fail(ErrorKind::Transport, 0, response.error);
    return parseUser(unwrap(response.status, response.body));
}

DirectoryPage Client::list(std::string_view path, int page, int perPage, bool refresh,
                           std::string_view folderPassword, std::stop_token stop) {
    const auto normalized = normalizePath(path);
    Json body{{"path", normalized}, {"page", std::max(1, page)}, {"per_page", std::max(0, perPage)},
              {"refresh", refresh}, {"password", std::string(folderPassword)}};
    return parseDirectory(call("/api/fs/list", body, true, stop), normalized);
}

FileDetail Client::get(std::string_view path, std::string_view folderPassword, std::stop_token stop) {
    const auto normalized = normalizePath(path);
    Json body{{"path", normalized}, {"password", std::string(folderPassword)}};
    return parseFileDetail(call("/api/fs/get", body, true, stop), normalized);
}

std::string protectSecret(std::string_view plain) {
    DATA_BLOB input{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"Velin AList session", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
        throw std::runtime_error("Cannot encrypt the AList session (Windows error " + std::to_string(GetLastError()) + ")");
    std::string result(reinterpret_cast<const char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return result;
}

std::string unprotectSecret(std::string_view protectedBytes) {
    DATA_BLOB input{static_cast<DWORD>(protectedBytes.size()), reinterpret_cast<BYTE*>(const_cast<char*>(protectedBytes.data()))};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
        throw std::runtime_error("Cannot decrypt the AList session (Windows error " + std::to_string(GetLastError()) + ")");
    std::string result(reinterpret_cast<const char*>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
}

bool saveSession(const std::filesystem::path& file, const Session& session, std::string& error) {
    try {
        Json json{{"server", normalizeServerUrl(session.server)}, {"username", session.username},
                  {"clientId", session.clientId}, {"token", session.token}};
        auto plain = json.dump();
        std::string bytes(kSessionMagic);
        bytes += protectSecret(plain);
        SecureZeroMemory(plain.data(), plain.size());
        return writeTextFileAtomic(file, bytes, error);
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

std::optional<Session> loadSession(const std::filesystem::path& file, std::string& error) {
    error.clear();
    std::error_code status;
    if (!std::filesystem::exists(file, status)) return std::nullopt;
    try {
        std::ifstream input(file, std::ios::binary);
        if (!input) throw std::runtime_error("Cannot read the AList session file");
        std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (bytes.size() > 1024 * 1024 || !std::string_view(bytes).starts_with(kSessionMagic))
            throw std::runtime_error("The AList session file is not recognised");
        auto plain = unprotectSecret(std::string_view(bytes).substr(kSessionMagic.size()));
        const auto json = Json::parse(plain, nullptr, false);
        SecureZeroMemory(plain.data(), plain.size());
        if (json.is_discarded() || !json.is_object()) throw std::runtime_error("The AList session file is damaged");
        Session session{text(json, "server"), text(json, "username"), text(json, "clientId"), text(json, "token")};
        session.server = normalizeServerUrl(session.server);
        return session;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

bool removeSession(const std::filesystem::path& file, std::string& error) {
    error.clear();
    std::error_code status;
    std::filesystem::remove(file, status);
    if (status) error = "Cannot remove the AList session: " + status.message();
    return !status;
}

std::filesystem::path sessionFile(const std::filesystem::path& dataDirectory) { return dataDirectory / L"alist-session.dat"; }

Json toJson(const Entry& entry) {
    return {{"name", entry.name}, {"path", entry.path}, {"isDirectory", entry.isDirectory}, {"size", entry.size},
            {"modified", entry.modified}, {"sign", entry.sign}, {"thumb", entry.thumb}, {"type", entry.type}};
}

Json toJson(const User& user) {
    return {{"id", user.id}, {"username", user.username}, {"basePath", user.basePath},
            {"roles", user.roles}, {"disabled", user.disabled}};
}

namespace {
std::string required(const Json& parameters, const char* key) {
    const auto value = text(parameters, key);
    if (value.empty()) throw std::invalid_argument(std::string("Missing AList parameter: ") + key);
    return value;
}
Client clientFor(const Json& parameters) {
    Client client(required(parameters, "server"), text(parameters, "clientId"), text(parameters, "token"));
    if (const auto timeout = number<int>(parameters, "timeoutMilliseconds"); timeout > 0) client.setTimeout(timeout);
    return client;
}
Json entriesJson(const std::vector<Entry>& entries, const std::string& server, bool withPlayback) {
    Json result = Json::array();
    for (const auto& entry : entries) {
        auto json = toJson(entry);
        if (withPlayback && !entry.isDirectory) {
            json["playable"] = isPlayable(entry);
            json["playbackUrl"] = playbackUrl(server, entry);
        }
        result.push_back(std::move(json));
    }
    return result;
}
} // namespace

Json serviceOperation(const std::string& operation, const Json& parameters, std::stop_token stop) {
    try {
        if (operation == "alist.login") {
            auto client = clientFor(parameters);
            const auto token = client.login(required(parameters, "username"), text(parameters, "password"),
                                            text(parameters, "otpCode"), stop);
            return {{"ok", true}, {"server", client.server()}, {"token", token}, {"user", toJson(client.me(stop))}};
        }
        if (operation == "alist.me") {
            auto client = clientFor(parameters);
            return {{"ok", true}, {"user", toJson(client.me(stop))}};
        }
        if (operation == "alist.list") {
            auto client = clientFor(parameters);
            const auto page = client.list(text(parameters, "path").empty() ? "/" : text(parameters, "path"),
                number<int>(parameters, "page", 1), number<int>(parameters, "perPage", 0),
                boolean(parameters, "refresh"), text(parameters, "folderPassword"), stop);
            return {{"ok", true}, {"path", page.path}, {"entries", entriesJson(page.entries, client.server(), true)},
                    {"total", page.total}, {"hasMore", page.hasMore}, {"provider", page.provider}, {"writable", page.writable}};
        }
        if (operation == "alist.get") {
            auto client = clientFor(parameters);
            const auto detail = client.get(required(parameters, "path"), text(parameters, "folderPassword"), stop);
            Json result{{"ok", true}, {"entry", toJson(detail.entry)}, {"rawUrl", detail.rawUrl},
                        {"originalUrl", detail.originalUrl}, {"provider", detail.provider},
                        {"related", entriesJson(detail.related, client.server(), true)},
                        {"subtitles", entriesJson(subtitlesFor(detail.entry, detail.related), client.server(), true)}};
            result["playbackUrl"] = detail.entry.isDirectory ? std::string{} : playbackUrl(client.server(), detail);
            return result;
        }
    } catch (const Error& error) {
        return {{"ok", false}, {"error", {{"kind", errorKindName(error.kind())}, {"code", error.code()}, {"message", error.what()}}}};
    }
    throw std::runtime_error("Unknown online operation: " + operation);
}

} // namespace velin::alist
