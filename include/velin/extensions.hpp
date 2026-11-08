// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "velin/media_engine.hpp"
#include "nlohmann/json.hpp"
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stop_token>

namespace velin {
using Json = nlohmann::json;

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
    std::size_t maximumBytes = 32 * 1024 * 1024;
    int timeoutMilliseconds = 15000;
    std::vector<std::string> allowedDomains;
};
struct HttpResponse {
    unsigned status = 0;
    std::string body;
    std::string error;
    [[nodiscard]] bool ok() const { return error.empty() && status >= 200 && status < 300; }
};
// Blocking transport intended for workers. Cancellation aborts WinHTTP I/O.
HttpResponse httpRequest(const HttpRequest& request, std::stop_token stop = {});
std::string sha256File(const std::filesystem::path& path);

struct OnlineConfiguration {
    std::string openSubtitlesApiKey, openSubtitlesUsername, openSubtitlesPassword, openSubtitlesToken;
    std::string assrtToken;
    std::filesystem::path ytDlpExecutable;
    std::string openSubtitlesBase = "https://api.opensubtitles.com/api/v1";
    std::string assrtBase = "https://api.assrt.net/v1";
    std::string pluginCatalog = "https://raw.githubusercontent.com/iina/iina/develop/plugins.json";
};
struct ServiceResult {
    std::uint64_t id = 0;
    std::string operation;
    Json data;
    std::string error;
    bool cancelled = false;
};
class OnlineServices {
public:
    explicit OnlineServices(OnlineConfiguration configuration = {});
    ~OnlineServices();
    OnlineServices(const OnlineServices&) = delete;
    OnlineServices& operator=(const OnlineServices&) = delete;
    void configure(OnlineConfiguration configuration);
    // Operations: opensubtitles.login/search/download, assrt.search/detail/download,
    // online.resolve, plugins.catalog, updates.check, download.verified, http.request, process.exec.
    // alist.login/me/list/get are described in velin/alist.hpp.
    // process.exec emits process.stdout/process.stderr string chunks before its final result.
    // Results are delivered on the caller's UI thread through poll().
    std::uint64_t submit(std::string operation, Json parameters = Json::object(), bool privateResult = false);
    void cancel(std::uint64_t id);
    void abandon(std::uint64_t id);
    void cancelAll();
    std::vector<ServiceResult> poll();
    std::vector<ServiceResult> pollFor(const std::set<std::uint64_t>& ids);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct PluginInfo {
    std::string identifier, name, version, author, description, entry;
    std::filesystem::path directory;
    std::set<std::string> permissions;
    std::vector<std::string> domains;
    Json manifest;
    bool enabled = false;
    bool running = false;
    std::string error;
};
struct PluginMenuItem {
    std::string plugin, identifier, title, keyBinding;
    bool enabled = true, selected = false, separator = false;
    unsigned depth = 0;
};
struct ExtensionCallbacks {
    std::function<void(const std::string& plugin, const std::string& text)> log;
    // Required for requested native UI functionality. Unhandled operations must
    // throw std::runtime_error; their JS caller receives an exception.
    // Operations include osd, alert, open-url, core.window.get/set,
    // overlay.*, sidebar.*, standalone.*, preferences.open, utils.*.
    // The plugin information is the approved snapshot held by the running instance.
    std::function<Json(const PluginInfo& plugin, const std::string& operation, const Json& arguments)> ui;
    std::function<void()> menusChanged;
};

class PluginHost {
public:
    PluginHost(std::filesystem::path dataDirectory, MediaEngine& engine,
               OnlineServices& services, ExtensionCallbacks callbacks = {},
               std::filesystem::path pluginDirectory = {});
    ~PluginHost();
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;
    std::vector<PluginInfo> discover();
    // Source is a directory, .iinaplugin package directory, or ZIP/.iinaplgz.
    // Every declared permission must be included in explicitly approvedPermissions.
    PluginInfo inspect(const std::filesystem::path& source);
    PluginInfo install(const std::filesystem::path& source,
                       const std::set<std::string>& approvedPermissions, bool replace = false);
    void uninstall(const std::string& identifier);
    void setEnabled(const std::string& identifier, bool enabled);
    void reload();
    void setGlobalEntriesEnabled(bool enabled);
    bool busy() const;
    std::vector<PluginMenuItem> menuItems() const;
    std::vector<PluginMenuItem> playlistMenuItems(const std::vector<std::string>& filenames);
    bool invokeMenu(const std::string& plugin, const std::string& identifier);
    // Emit IINA names, e.g. mpv.file-loaded, mpv.time-pos.changed,
    // mpv.pause.changed, iina.window-resized. Engine events can be forwarded here.
    void emit(const std::string& event, const Json& arguments = Json::array());
    void receiveWebMessage(const std::string& plugin, const std::string& kind,
                           const std::string& name, const Json& data);
    void receiveGlobalMessage(const std::string& plugin, const std::string& name,
                              const Json& data, const Json& source = nullptr);
    void searchSubtitleProvider(const std::string& plugin, const std::string& provider);
    void downloadSubtitleProvider(const std::string& plugin, const std::string& provider, const Json& data);
    bool input(const std::string& event, const std::string& key, const Json& arguments = Json::object());
    void poll();
    Json evaluate(const std::string& identifier, const std::string& source);
    Json compatibility() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace velin
