// SPDX-License-Identifier: GPL-3.0-or-later
// AList client tests. HTTP contracts run against an in-process mock server on
// 127.0.0.1, so the suite never leaves the machine.
#include <winsock2.h>
#include <ws2tcpip.h>
#include "velin/alist.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>

namespace {
namespace alist = velin::alist;
using velin::Json;

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
template <typename Action>
alist::ErrorKind failsWith(Action action, const std::string& message) {
    try {
        action();
    } catch (const alist::Error& error) {
        return error.kind();
    }
    throw std::runtime_error("Expected an AList error: " + message);
}

struct Request {
    std::string method, path;
    std::map<std::string, std::string> headers; // Lower-case names.
    std::string body;
};
struct Reply {
    unsigned status = 200;
    std::string body;
    std::string contentType = "application/json";
};

class MockServer {
public:
    using Handler = std::function<Reply(const Request&)>;
    explicit MockServer(Handler handler) : handler_(std::move(handler)) {
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        check(listener_ != INVALID_SOCKET, "Create mock listener");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "Bind mock listener");
        check(listen(listener_, 16) == 0, "Listen on mock listener");
        int length = sizeof(address);
        check(getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) == 0, "Read mock port");
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~MockServer() {
        stopping_ = true;
        closesocket(listener_);
        if (thread_.joinable()) thread_.join();
    }
    [[nodiscard]] std::string base() const { return "http://127.0.0.1:" + std::to_string(port_); }
    std::vector<Request> requests() {
        std::lock_guard lock(mutex_);
        return requests_;
    }
private:
    void serve() {
        while (!stopping_) {
            const SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;
            DWORD timeout = 5000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
            try {
                handle(client);
            } catch (...) {
            }
            closesocket(client);
        }
    }
    void handle(SOCKET client) {
        std::string data;
        char buffer[4096];
        std::size_t headerEnd = std::string::npos;
        while ((headerEnd = data.find("\r\n\r\n")) == std::string::npos) {
            const int count = recv(client, buffer, sizeof(buffer), 0);
            if (count <= 0) return;
            data.append(buffer, static_cast<std::size_t>(count));
        }
        Request request;
        std::size_t lineEnd = data.find("\r\n");
        const auto requestLine = data.substr(0, lineEnd);
        const auto firstSpace = requestLine.find(' ');
        const auto secondSpace = requestLine.find(' ', firstSpace + 1);
        request.method = requestLine.substr(0, firstSpace);
        request.path = requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
        std::size_t position = lineEnd + 2;
        while (position < headerEnd) {
            const auto end = data.find("\r\n", position);
            const auto line = data.substr(position, end - position);
            const auto colon = line.find(':');
            if (colon != std::string::npos) {
                auto name = line.substr(0, colon);
                for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                auto value = line.substr(colon + 1);
                value.erase(0, value.find_first_not_of(' '));
                request.headers[name] = value;
            }
            position = end + 2;
        }
        const auto length = request.headers.contains("content-length") ? std::stoul(request.headers["content-length"]) : 0UL;
        request.body = data.substr(headerEnd + 4);
        while (request.body.size() < length) {
            const int count = recv(client, buffer, sizeof(buffer), 0);
            if (count <= 0) return;
            request.body.append(buffer, static_cast<std::size_t>(count));
        }
        {
            std::lock_guard lock(mutex_);
            requests_.push_back(request);
        }
        const auto reply = handler_(request);
        std::string response = "HTTP/1.1 " + std::to_string(reply.status) + " Fixture\r\nContent-Type: " + reply.contentType +
                               "\r\nContent-Length: " + std::to_string(reply.body.size()) + "\r\nConnection: close\r\n\r\n" + reply.body;
        std::size_t sent = 0;
        while (sent < response.size()) {
            const int count = send(client, response.data() + sent, static_cast<int>(response.size() - sent), 0);
            if (count <= 0) return;
            sent += static_cast<std::size_t>(count);
        }
    }
    Handler handler_;
    SOCKET listener_ = INVALID_SOCKET;
    unsigned short port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::vector<Request> requests_;
};

// AList answers errors with HTTP 200 and the real code in the envelope.
Reply envelope(int code, const std::string& message, const Json& data = nullptr) {
    return {200, Json{{"code", code}, {"message", message}, {"data", data}}.dump()};
}

const std::string kToken = "fixture-token";
const std::string kPasswordHash = "8a119a191d7f34278647d21217ff550a9f6748d48a6952379405487747a840c3"; // "secret"

Reply alistFixture(const Request& request, const std::string& base) {
    const auto body = Json::parse(request.body.empty() ? "{}" : request.body, nullptr, false);
    const auto authorized = request.headers.contains("authorization") && request.headers.at("authorization") == kToken;
    if (request.path == "/api/auth/login/hash") {
        if (request.method != "POST") return {405, "method not allowed", "text/plain"};
        if (!request.headers.contains("client-id")) return envelope(400, "fixture expects a Client-Id");
        if (body.value("password", "") != kPasswordHash) return envelope(400, "password is incorrect");
        if (body.value("username", "") == "otp-user" && body.value("otp_code", "") != "123456") return envelope(402, "Invalid 2FA code");
        if (body.value("username", "") == "busy") return envelope(403, "too many active devices");
        if (body.value("username", "") == "tokenless") return envelope(200, "success", Json::object());
        return envelope(200, "success", {{"token", kToken}, {"device_key", "fixture"}});
    }
    if (request.path == "/api/me") {
        if (request.method != "GET") return {405, "method not allowed", "text/plain"};
        if (request.headers.contains("authorization") && request.headers.at("authorization") == "proxy")
            return {502, "<html>Bad gateway</html>", "text/html"};
        if (!authorized) return envelope(401, "token is invalidated");
        return envelope(200, "success", {{"id", 2}, {"username", "kuroega"}, {"base_path", "/"}, {"role", {0}}, {"disabled", false}});
    }
    if (request.path == "/api/fs/list") {
        if (!authorized) return envelope(401, "token is invalidated");
        const auto path = body.value("path", "");
        if (path == "/empty") return envelope(200, "success", {{"content", nullptr}, {"total", 0}, {"has_more", false}});
        if (path == "/locked" && body.value("password", "") != "open sesame") return envelope(403, "password is incorrect or you have no permission");
        if (path == "/broken") return envelope(200, "success", {{"content", "not an array"}});
        if (path == "/Shows/Season 1" || path == "/locked") {
            return envelope(200, "success", {
                {"content", Json::array({
                    {{"name", "Extras"}, {"is_dir", true}, {"size", 0}, {"type", 1}, {"sign", ""}},
                    {{"name", "Ep01.mkv"}, {"is_dir", false}, {"size", 1234567890123LL}, {"type", 2}, {"sign", "abc_-=:0"},
                     {"modified", "2026-10-01T12:00:00+08:00"}, {"thumb", ""}},
                    {{"name", "Ep01.zh-Hans.ass"}, {"is_dir", false}, {"size", 2048}, {"type", 4}, {"sign", "sub1:0"}},
                    {{"name", "../escape.mkv"}, {"is_dir", false}, {"type", 2}},
                    {{"name", "notes.nfo"}, {"is_dir", false}, {"size", 10}, {"type", 0}}
                })},
                {"total", 4}, {"has_more", false}, {"provider", "Local"}, {"write", true}});
        }
        return envelope(500, "object not found");
    }
    if (request.path == "/api/fs/get") {
        if (!authorized) return envelope(401, "token is invalidated");
        const auto path = body.value("path", "");
        if (path == "/Shows/Season 1/Ep01.mkv") {
            return envelope(200, "success", {
                {"name", "Ep01.mkv"}, {"is_dir", false}, {"size", 1234567890123LL}, {"type", 2}, {"sign", "abc_-=:0"},
                {"raw_url", base + "/playback/session/master.m3u8"},
                {"original_url", base + "/d/Shows/Season%201/Ep01.mkv?sign=abc_-=:0"},
                {"provider", "Local"},
                {"related", Json::array({
                    {{"name", "Ep01.zh-Hans.ass"}, {"is_dir", false}, {"type", 4}, {"sign", "sub1:0"}},
                    {{"name", "Ep01.srt"}, {"is_dir", false}, {"type", 4}, {"sign", ""}},
                    {{"name", "Ep010.ass"}, {"is_dir", false}, {"type", 4}},
                    {{"name", "Ep01.mkv.jpg"}, {"is_dir", false}, {"type", 5}},
                    {{"name", "Ep02.mkv"}, {"is_dir", false}, {"type", 2}}
                })}});
        }
        if (path == "/Music/Track.flac") {
            return envelope(200, "success", {{"name", "Track.flac"}, {"is_dir", false}, {"type", 3},
                                             {"raw_url", base + "/d/Music/Track.flac"}, {"related", nullptr}});
        }
        return envelope(500, "object not found");
    }
    return envelope(404, "unknown fixture route");
}

alist::Entry entry(std::string name, std::string path = {}, bool directory = false, int type = 0) {
    alist::Entry result;
    result.name = std::move(name);
    result.path = std::move(path);
    result.isDirectory = directory;
    result.type = type;
    return result;
}

void testPathsAndUrls() {
    check(alist::normalizeServerUrl(" 10.127.1.109:5244/ ") == "http://10.127.1.109:5244", "Bare host gets http and loses the slash");
    check(alist::normalizeServerUrl("HTTPS://Media.Example.org/alist//") == "https://media.example.org/alist", "Scheme/host lower-cased, sub-path kept");
    check(failsWith([] { alist::normalizeServerUrl("ftp://host"); }, "ftp") == alist::ErrorKind::InvalidServer, "Non-HTTP scheme rejected");
    check(failsWith([] { alist::normalizeServerUrl("http://user:pw@host"); }, "userinfo") == alist::ErrorKind::InvalidServer, "Credentials in URL rejected");
    check(failsWith([] { alist::normalizeServerUrl("http://host/?x=1"); }, "query") == alist::ErrorKind::InvalidServer, "Query rejected");
    check(failsWith([] { alist::normalizeServerUrl("   "); }, "empty") == alist::ErrorKind::InvalidServer, "Empty server rejected");

    check(alist::normalizePath("") == "/" && alist::normalizePath("//Shows//S1/") == "/Shows/S1", "Path normalization");
    check(alist::normalizePath("Shows\\S1") == "/Shows/S1", "Backslashes treated as separators");
    check(failsWith([] { alist::normalizePath("/Shows/../etc"); }, "dotdot") == alist::ErrorKind::InvalidServer, "Parent segments rejected");
    check(alist::joinPath("/", "Movies") == "/Movies" && alist::joinPath("/Shows/S1/", "Ep01.mkv") == "/Shows/S1/Ep01.mkv", "Path joining");
    check(alist::parentPath("/Shows/S1/Ep01.mkv") == "/Shows/S1" && alist::parentPath("/Movies") == "/" && alist::parentPath("/") == "/", "Parent paths");

    const std::string cjk = "/\xE5\x8B\x95\xE7\x95\xAB/\xE7\xAC\xAC 1 \xE8\xA9\xB1#?.mkv"; // /動畫/第 1 話#?.mkv
    check(alist::encodePath(cjk) == "/%E5%8B%95%E7%95%AB/%E7%AC%AC%201%20%E8%A9%B1%23%3F.mkv", "CJK, spaces, # and ? are percent-encoded");
    check(alist::downloadUrl("10.127.1.109:5244", "/Shows/Season 1/Ep01.mkv") == "http://10.127.1.109:5244/d/Shows/Season%201/Ep01.mkv", "Unsigned /d link");
    check(alist::downloadUrl("http://h", "/a.mkv", "ab+c/d=:0") == "http://h/d/a.mkv?sign=ab%2Bc%2Fd%3D:0", "Sign is query-encoded");

    alist::FileDetail detail;
    detail.entry.name = "Ep01.mkv";
    detail.entry.path = "/Ep01.mkv";
    detail.rawUrl = "http://h/playback/x/master.m3u8";
    detail.originalUrl = "http://h/d/Ep01.mkv?sign=s";
    check(alist::playbackUrl("http://h", detail) == detail.originalUrl, "Original file preferred over the compatibility stream");
    detail.originalUrl.clear();
    check(alist::playbackUrl("http://h", detail) == detail.rawUrl, "raw_url used when no original exists");
    detail.rawUrl.clear();
    detail.entry.sign = "s";
    check(alist::playbackUrl("http://h", detail) == "http://h/d/Ep01.mkv?sign=s", "Falls back to a signed /d link");

    check(alist::passwordHash("secret") == kPasswordHash, "Static password hash matches AList's model.StaticHash");
    const auto first = alist::generateClientId(), second = alist::generateClientId();
    check(first.size() == 32 && first.find_first_not_of("0123456789abcdef") == std::string::npos && first != second, "Client identifiers are random hex");

    const auto video = entry("Ep01.mkv", "/S/Ep01.mkv");
    const std::vector<alist::Entry> siblings{entry("Ep01.srt", "/S/Ep01.srt"), entry("ep01.ZH.ass", "/S/ep01.ZH.ass"),
                                             entry("Ep010.ass", "/S/Ep010.ass"), entry("Ep01.nfo", "/S/Ep01.nfo"),
                                             entry("Ep01.ass", "/S/Ep01.ass", true)};
    const auto subtitles = alist::subtitlesFor(video, siblings);
    check(subtitles.size() == 2 && subtitles[0].name == "Ep01.srt" && subtitles[1].name == "ep01.ZH.ass",
          "Subtitle matching uses the stem, ignores case, other episodes, non-subtitles and folders");
    check(alist::isPlayable(entry("a.flac")) && alist::isPlayable(entry("x.bin", "", false, 2)) && !alist::isPlayable(entry("a.nfo")),
          "Playable by type or extension");
}

void testPlaylist() {
    auto signedEntry = [](std::string name, std::string sign, bool directory = false, int type = 0) {
        auto result = entry(name, "/Show/" + name, directory, type);
        result.sign = std::move(sign);
        return result;
    };
    std::vector<alist::Entry> folder{
        signedEntry("Ep10.mkv", "s10"), signedEntry("Extras", "", true, 1), signedEntry("Ep2.mkv", "s2"),
        signedEntry("ep1.MKV", "s1"), signedEntry("Ep2.zh-Hans.ass", "z2"), signedEntry("Ep2.en.srt", "e2"),
        signedEntry("Ep10.ass", "a10"), signedEntry("notes.nfo", ""), signedEntry("Archive", "", true, 1),
        signedEntry("Ep2.mkv.jpg", "j")};

    auto sorted = folder;
    alist::sortEntries(sorted);
    check(sorted[0].name == "Archive" && sorted[1].name == "Extras" && sorted[2].name == "ep1.MKV" &&
          sorted[3].name == "Ep2.en.srt", "Folders first, then natural order ignoring case");

    const auto playlist = alist::buildPlaylist("http://h:5244", folder, "/Show/Ep2.mkv");
    check(playlist.items.size() == 3 && playlist.start == 1, "Every playable file is queued, starting at the chosen one");
    check(playlist.items[0].path == "/Show/ep1.MKV" && playlist.items[1].path == "/Show/Ep2.mkv" &&
          playlist.items[2].path == "/Show/Ep10.mkv", "Playlist is in natural order");
    check(playlist.items[1].url == "http://h:5244/d/Show/Ep2.mkv?sign=s2", "Items carry signed /d links");
    check(playlist.items[1].subtitles.size() == 2 && playlist.items[1].subtitleNames[0] == "Ep2.en.srt" &&
          playlist.items[1].subtitles[0] == "http://h:5244/d/Show/Ep2.en.srt?sign=e2" &&
          playlist.items[1].subtitles[1] == "http://h:5244/d/Show/Ep2.zh-Hans.ass?sign=z2", "Subtitles attach to their own video");
    check(playlist.items[2].subtitles.size() == 1 && playlist.items[0].subtitles.empty(), "Ep10 gets only its own subtitle");
    check(alist::buildPlaylist("http://h", folder, "/Show/notes.nfo").items.empty() &&
          alist::buildPlaylist("http://h", folder, "/Show/Missing.mkv").items.empty(), "Unplayable or missing choice gives no playlist");

    const std::vector<std::string> names{"Ep2.en.srt", "Ep2.zh-Hans.ass", "Ep2.chi.forced.ass"};
    check(alist::preferredSubtitle(names, "zh-Hans,eng") == 1, "First preferred language wins");
    check(alist::preferredSubtitle(names, " ZH , en") == 1, "Leading subtag matches, case and spaces ignored");
    check(alist::preferredSubtitle(names, "jpn,chi") == 2, "Later languages are tried in order");
    check(alist::preferredSubtitle(names, "en") == 0 && alist::preferredSubtitle(names, "") == 0 &&
          alist::preferredSubtitle(names, "fre") == 0, "Falls back to the first subtitle");
    check(alist::preferredSubtitle({"Ep2.srt"}, "srt") == 0 && alist::preferredSubtitle({}, "eng") == 0, "Extensions are not language tags");

    const auto round = alist::entryFromJson(alist::toJson(signedEntry("Ep2.mkv", "s2", false, 2)));
    check(round.name == "Ep2.mkv" && round.path == "/Show/Ep2.mkv" && round.sign == "s2" && round.type == 2 && !round.isDirectory,
          "Entries survive the service JSON round trip");
    check(alist::entryFromJson(Json::array()).name.empty(), "Malformed entry JSON gives an empty entry");
}

void testEnvelopes() {
    check(alist::unwrap(200, R"({"code":200,"message":"success","data":{"a":1}})")["a"] == 1, "Success envelope");
    check(alist::unwrap(200, R"({"code":200,"message":"success"})").is_null(), "Missing data is null");
    check(failsWith([] { alist::unwrap(200, R"({"code":401,"message":"token is expired","data":null})"); }, "401") == alist::ErrorKind::Unauthorized, "401 in envelope");
    check(failsWith([] { alist::unwrap(200, R"({"code":402,"message":"otp"})"); }, "402") == alist::ErrorKind::OtpRequired, "402 in envelope");
    check(failsWith([] { alist::unwrap(200, R"({"code":403,"message":"nope"})"); }, "403") == alist::ErrorKind::Forbidden, "403 in envelope");
    check(failsWith([] { alist::unwrap(200, R"({"code":500,"message":"boom"})"); }, "500") == alist::ErrorKind::Server, "Other codes");
    check(failsWith([] { alist::unwrap(401, "Unauthorized"); }, "bare 401") == alist::ErrorKind::Unauthorized, "Non-envelope HTTP 401");
    check(failsWith([] { alist::unwrap(200, "<html></html>"); }, "html") == alist::ErrorKind::InvalidResponse, "Non-AList reply");
    try {
        alist::unwrap(200, R"({"code":500,"message":"object not found"})");
    } catch (const alist::Error& error) {
        check(error.code() == 500 && std::string(error.what()) == "object not found", "Server message and code are kept");
    }
}

void testSessionStore(const std::filesystem::path& root) {
    const auto secret = alist::protectSecret("token value");
    check(secret != "token value" && alist::unprotectSecret(secret) == "token value", "DPAPI round trip");
    const auto file = alist::sessionFile(root);
    std::string error;
    check(!alist::loadSession(file, error) && error.empty(), "Missing session is not an error");
    const alist::Session session{"10.127.1.109:5244/", "kuroega", "0123456789abcdef0123456789abcdef", "jwt.token.value", "/Shows/S1"};
    check(alist::saveSession(file, session, error), "Save session: " + error);
    std::ifstream raw(file, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(raw)), std::istreambuf_iterator<char>());
    raw.close();
    check(bytes.find("jwt.token.value") == std::string::npos && bytes.find("kuroega") == std::string::npos, "Session file is encrypted");
    const auto loaded = alist::loadSession(file, error);
    check(loaded && loaded->server == "http://10.127.1.109:5244" && loaded->username == "kuroega" &&
          loaded->clientId == session.clientId && loaded->token == session.token && loaded->folder == "/Shows/S1", "Session round trip: " + error);
    {
        std::ofstream damaged(file, std::ios::binary | std::ios::trunc);
        damaged << bytes.substr(0, bytes.size() - 8) << "garbage!";
    }
    check(!alist::loadSession(file, error) && !error.empty(), "Tampered session is rejected");
    check(alist::removeSession(file, error) && !std::filesystem::exists(file), "Remove session");
}

void testClient() {
    std::string base;
    MockServer server([&base](const Request& request) { return alistFixture(request, base); });
    base = server.base();
    const std::string clientId = "0123456789abcdef0123456789abcdef";

    alist::Client client(base + "/", clientId);
    check(failsWith([&] { client.list("/"); }, "no token") == alist::ErrorKind::Unauthorized, "Browsing needs a token first");
    check(client.login("kuroega", "secret") == kToken && client.token() == kToken, "Hashed sign-in returns the token");
    const auto login = server.requests().back();
    check(login.path == "/api/auth/login/hash" && login.headers.at("client-id") == clientId, "Sign-in sends the Client-Id");
    check(login.body.find("secret\"") == std::string::npos, "The plain password never crosses the network");
    check(failsWith([&] { alist::Client(base, clientId).login("kuroega", "wrong"); }, "bad password") == alist::ErrorKind::Server, "Wrong password");
    check(failsWith([&] { alist::Client(base, clientId).login("otp-user", "secret"); }, "otp") == alist::ErrorKind::OtpRequired, "2FA required");
    check(alist::Client(base, clientId).login("otp-user", "secret", " 123456 ") == kToken, "2FA code accepted");
    check(failsWith([&] { alist::Client(base, clientId).login("busy", "secret"); }, "devices") == alist::ErrorKind::Forbidden, "Too many devices");
    check(failsWith([&] { alist::Client(base, clientId).login("tokenless", "secret"); }, "tokenless") == alist::ErrorKind::InvalidResponse, "Missing token");

    const auto user = client.me();
    check(user.username == "kuroega" && user.roles == std::vector<int>{0} && user.basePath == "/", "Current user");
    check(server.requests().back().headers.at("authorization") == kToken, "Bare token in Authorization");
    check(failsWith([&] { alist::Client(base, clientId, "expired").me(); }, "expired") == alist::ErrorKind::Unauthorized, "Expired token");
    check(failsWith([&] { alist::Client(base, clientId, "proxy").me(); }, "proxy") == alist::ErrorKind::Server, "Non-AList gateway error");

    const auto page = client.list("Shows/Season 1/");
    check(server.requests().back().body.find("\"/Shows/Season 1\"") != std::string::npos, "Listed path is normalized");
    check(page.path == "/Shows/Season 1" && page.entries.size() == 4 && page.provider == "Local" && page.writable, "Folder listing");
    check(page.entries[0].isDirectory && page.entries[0].path == "/Shows/Season 1/Extras", "Folder entry path");
    check(page.entries[1].size == 1234567890123LL && page.entries[1].sign == "abc_-=:0", "Large size and sign kept");
    check(alist::playbackUrl(base, page.entries[1]) == base + "/d/Shows/Season%201/Ep01.mkv?sign=abc_-%3D:0", "Signed playback link from a listing");
    check(client.list("/empty").entries.empty(), "Empty folder (null content)");
    check(failsWith([&] { client.list("/locked"); }, "locked") == alist::ErrorKind::Forbidden, "Folder password required");
    check(client.list("/locked", 1, 0, false, "open sesame").entries.size() == 4, "Folder password accepted");
    check(failsWith([&] { client.list("/broken"); }, "broken") == alist::ErrorKind::InvalidResponse, "Malformed listing");
    check(failsWith([&] { client.list("/missing"); }, "missing") == alist::ErrorKind::Server, "Missing folder");

    const auto detail = client.get("/Shows/Season 1/Ep01.mkv");
    check(alist::playbackUrl(base, detail) == base + "/d/Shows/Season%201/Ep01.mkv?sign=abc_-=:0", "Detail plays the original file");
    check(detail.related.size() == 5 && detail.related[0].path == "/Shows/Season 1/Ep01.zh-Hans.ass", "Related files get absolute paths");
    const auto subtitles = alist::subtitlesFor(detail.entry, detail.related);
    check(subtitles.size() == 2 && subtitles[0].name == "Ep01.srt" && subtitles[1].name == "Ep01.zh-Hans.ass", "Detail subtitles");
    const auto track = client.get("Music/Track.flac");
    check(track.related.empty() && alist::playbackUrl(base, track) == base + "/d/Music/Track.flac", "Audio without siblings");

    // The asynchronous service layer the player UI uses.
    velin::OnlineServices services;
    std::set<std::uint64_t> ids;
    const auto loginId = services.submit("alist.login", {{"server", base}, {"clientId", clientId}, {"username", "kuroega"}, {"password", "secret"}}, true);
    const auto listId = services.submit("alist.list", {{"server", base}, {"clientId", clientId}, {"token", kToken}, {"path", "/Shows/Season 1"}});
    const auto getId = services.submit("alist.get", {{"server", base}, {"clientId", clientId}, {"token", kToken}, {"path", "/Shows/Season 1/Ep01.mkv"}});
    const auto expiredId = services.submit("alist.list", {{"server", base}, {"clientId", clientId}, {"token", "expired"}, {"path", "/"}});
    const auto missingId = services.submit("alist.get", {{"clientId", clientId}, {"path", "/x"}});
    ids = {loginId, listId, getId, expiredId, missingId};
    std::map<std::uint64_t, velin::ServiceResult> results;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (results.size() < ids.size() && std::chrono::steady_clock::now() < deadline) {
        for (auto& result : services.pollFor(ids)) results[result.id] = std::move(result);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(results.size() == ids.size(), "AList service operations complete");
    const auto& loginResult = results[loginId].data;
    check(results[loginId].error.empty() && loginResult["ok"] == true && loginResult["token"] == kToken &&
          loginResult["user"]["username"] == "kuroega" && loginResult["server"] == base, "alist.login");
    const auto& listResult = results[listId].data;
    check(listResult["ok"] == true && listResult["entries"].size() == 4 && !listResult["entries"][0].contains("playbackUrl") &&
          listResult["entries"][1]["playable"] == true && listResult["entries"][3]["playable"] == false &&
          listResult["entries"][1]["playbackUrl"].get<std::string>().starts_with(base + "/d/Shows/Season%201/Ep01.mkv?sign="), "alist.list");
    const auto& getResult = results[getId].data;
    check(getResult["ok"] == true && getResult["playbackUrl"] == base + "/d/Shows/Season%201/Ep01.mkv?sign=abc_-=:0" &&
          getResult["subtitles"].size() == 2 && getResult["subtitles"][1]["playbackUrl"] == base + "/d/Shows/Season%201/Ep01.zh-Hans.ass?sign=sub1:0",
          "alist.get");
    const auto& expired = results[expiredId].data;
    check(results[expiredId].error.empty() && expired["ok"] == false && expired["error"]["kind"] == "unauthorized" && expired["error"]["code"] == 401,
          "AList failures are reported as data");
    check(!results[missingId].error.empty(), "Missing parameters are service errors");
}

void testUnreachable() {
    unsigned short port = 0;
    {
        MockServer server([](const Request&) { return Reply{}; });
        port = static_cast<unsigned short>(std::stoi(server.base().substr(server.base().rfind(':') + 1)));
    }
    alist::Client client("http://127.0.0.1:" + std::to_string(port), "id");
    client.setTimeout(2000);
    check(failsWith([&] { client.login("kuroega", "secret"); }, "refused") == alist::ErrorKind::Transport, "Unreachable server is a transport error");
}
} // namespace

int main() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "FAIL WSAStartup\n";
        return 1;
    }
    const auto root = std::filesystem::current_path() / "alist-test-data";
    int status = 0;
    try {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        testPathsAndUrls();
        std::cout << "PASS server URLs, paths, encoding, playback URL choice, password hash, subtitles\n";
        testPlaylist();
        std::cout << "PASS folder sorting, playlist building, subtitle preference\n";
        testEnvelopes();
        std::cout << "PASS envelope and error mapping\n";
        testSessionStore(root);
        std::cout << "PASS DPAPI session store\n";
        testClient();
        std::cout << "PASS sign-in, current user, listing, details, folder passwords, service operations (loopback mock)\n";
        testUnreachable();
        std::cout << "PASS unreachable server\n";
        std::cout << "All AList checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        status = 1;
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    WSACleanup();
    return status;
}
