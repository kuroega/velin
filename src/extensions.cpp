// SPDX-License-Identifier: GPL-3.0-or-later
#include "velin/extensions.hpp"
#include "velin/alist.hpp"
#include "velin/core.hpp"
#include "velin/plugin_websocket.hpp"
#include "velin/system.hpp"
#include "quickjs.h"
#include "miniz.h"
#include "iina_compat.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <deque>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <limits>
#include <regex>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace velin {
namespace {
std::string utf8Path(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
std::filesystem::path pathUtf8(const std::string& text) { return std::filesystem::path(fromUtf8(text)); }
std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}
void ensure(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
std::string readFile(const std::filesystem::path& path, std::size_t maximum = 32 * 1024 * 1024) {
    std::ifstream file(path, std::ios::binary);
    ensure(static_cast<bool>(file), "Cannot read file: " + utf8Path(path));
    const auto size = std::filesystem::file_size(path);
    ensure(size <= maximum, "File exceeds the size limit");
    std::string text(static_cast<std::size_t>(size), '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    ensure(file.good() || file.eof(), "File read failed");
    return text;
}
void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path; temporary += ".tmp";
    { std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
      ensure(static_cast<bool>(out), "Cannot write file: " + utf8Path(path));
      out.write(text.data(), static_cast<std::streamsize>(text.size()));
      ensure(out.good(), "File write failed"); }
    sys::replaceFile(temporary, path);
}
std::string encode(const std::string& text) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') result += static_cast<char>(c);
        else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return result;
}
bool domainAllowed(const std::string& host, const std::vector<std::string>& domains) {
    if (domains.empty()) return true;
    const auto normalized = lower(host);
    for (auto domain : domains) {
        domain = lower(domain);
        if (domain == "*" || normalized == domain) return true;
        if (domain.starts_with("*.") && normalized.size() > domain.size() - 1 && normalized.ends_with(domain.substr(1))) return true;
    }
    return false;
}
Json requestJson(HttpRequest request, std::stop_token stop) {
    const auto response = httpRequest(request, stop);
    ensure(response.ok(), response.error.empty() ? "HTTP " + std::to_string(response.status) + ": " + response.body.substr(0, 1024) : response.error);
    return Json::parse(response.body);
}
using ProcessProgress = std::function<void(bool, const std::string&)>;
struct ProcessOutput { long long status=0; std::string output,error; };
ProcessOutput executeProcess(const std::filesystem::path& executable,const std::vector<std::string>& arguments,
    std::stop_token stop,const std::filesystem::path& directory={},const ProcessProgress& progress={}) {
    const auto workingDirectory=directory.empty()?executable.parent_path():directory;
    ensure(std::filesystem::is_directory(workingDirectory),"Process working directory does not exist");
    ensure(executable.is_absolute() && std::filesystem::is_regular_file(executable),"Executable does not exist at an absolute path");
    sys::ChildProcess process(executable,arguments,workingDirectory);
    ProcessOutput result;std::string pending[2];
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(90);
    auto drain=[&](bool isError) {
        char buffer[8192];
        const auto received=process.read(isError,buffer,sizeof(buffer));
        if(!received)return;
        auto& output=isError?result.error:result.output;output.append(buffer,received);
        ensure(result.output.size()+result.error.size()<=8*1024*1024,"Helper exceeded its 8 MiB output limit");
        auto& text=pending[isError?1:0];text.append(buffer,received);
        size_t complete=text.size(),lead=complete;
        while(lead && (static_cast<unsigned char>(text[lead-1])&0xc0)==0x80)--lead;
        if(lead){--lead;const auto first=static_cast<unsigned char>(text[lead]);
            const size_t length=first<0x80?1:first>=0xc2 && first<=0xdf?2:first>=0xe0 && first<=0xef?3:first>=0xf0 && first<=0xf4?4:1;
            if(text.size()-lead<length)complete=lead;}
        if(complete){const auto chunk=text.substr(0,complete);
            ensure(!fromUtf8(chunk).empty(),"Process output is not valid UTF-8; configure the helper to emit UTF-8");
            if(progress){progress(isError,chunk);}
            text.erase(0,complete);}
    };
    try {
        while(true){
            const auto before=result.output.size()+result.error.size();
            drain(false);drain(true);
            ensure(!stop.stop_requested(),"Cancelled");
            ensure(std::chrono::steady_clock::now()<=deadline,"Helper exceeded its 90-second time limit");
            if(process.wait(before==result.output.size()+result.error.size()?10:0)){
                while(true){const auto before=result.output.size()+result.error.size();drain(false);drain(true);
                    if(before==result.output.size()+result.error.size())break;}
                break;
            }
        }
        ensure(pending[0].empty() && pending[1].empty(),"Process output ends with truncated UTF-8");
        result.status=process.exitCode();
    }catch(...){process.terminate();throw;}
    return result;
}
std::string runProcess(const std::filesystem::path& executable,const std::vector<std::string>& arguments,std::stop_token stop) {
    const auto result=executeProcess(executable,arguments,stop);
    ensure(result.status==0,"Helper process failed with exit code "+std::to_string(result.status));
    return result.output;
}
} // namespace

HttpResponse httpRequest(const HttpRequest& original, std::stop_token stop) {
    HttpResponse result;
    try {
        HttpRequest request = original;
        for (int redirects = 0; redirects < 6; ++redirects) {
            if (stop.stop_requested()) throw std::runtime_error("Cancelled");
            const auto url = sys::parseHttpUrl(request.url);
            ensure(domainAllowed(url.host, request.allowedDomains), "HTTP host is not allowed by the plugin manifest");
            for (const auto& [key, value] : request.headers)
                ensure(key.find_first_of("\r\n:") == std::string::npos && value.find_first_of("\r\n") == std::string::npos, "Invalid HTTP header");
            const auto response = sys::httpExchange({request.method, request.url, request.headers, request.body,
                request.maximumBytes, request.timeoutMilliseconds}, stop);
            result.status = response.status;
            if (response.status == 301 || response.status == 302 || response.status == 303 || response.status == 307 || response.status == 308) {
                std::string target = response.location;
                if (target.starts_with('/')) target = sys::httpOrigin(url) + target;
                const auto next = sys::parseHttpUrl(target);
                ensure(!url.secure || next.secure, "An HTTPS request cannot redirect to insecure HTTP");
                if (lower(next.host) != lower(url.host) || next.port != url.port) {
                    for (auto it = request.headers.begin(); it != request.headers.end();)
                        if (lower(it->first) == "authorization" || lower(it->first) == "api-key" || lower(it->first) == "cookie") it = request.headers.erase(it); else ++it;
                }
                if (response.status == 303 || ((response.status == 301 || response.status == 302) && request.method == "POST")) { request.method = "GET"; request.body.clear(); }
                request.url = target; continue;
            }
            result.body = response.body;
            return result;
        }
        throw std::runtime_error("Too many HTTP redirects");
    } catch (const std::exception& error) { result.error = stop.stop_requested() ? "Cancelled" : error.what(); }
    return result;
}

std::string sha256File(const std::filesystem::path& path) {
    const auto digest = sys::sha256File(path);
    std::ostringstream result; result << std::hex << std::setfill('0');
    for (unsigned byte : digest) result << std::setw(2) << byte;
    return result.str();
}

namespace {
Json serviceOperation(const std::string& operation, const Json& parameters, const OnlineConfiguration& config, std::stop_token stop, const ProcessProgress& progress = {}) {
    if (operation.starts_with("alist.")) return alist::serviceOperation(operation, parameters, stop);
    const auto query = parameters.value("query", parameters.value("q", std::string{}));
    HttpRequest request;
    request.headers = {{"User-Agent", "Velin v0.2.0"}, {"Accept", "application/json"}};
    auto openSubtitles = [&] {
        ensure(!config.openSubtitlesApiKey.empty(), "Configure an OpenSubtitles API key");
        request.headers["Api-Key"] = config.openSubtitlesApiKey;
        if (!config.openSubtitlesToken.empty()) request.headers["Authorization"] = "Bearer " + config.openSubtitlesToken;
    };
    auto download = [&](const std::string& url) -> Json {
        const auto path = pathUtf8(parameters.at("destination").get<std::string>());
        ensure(path.is_absolute(), "Download destination must be an absolute path");
        HttpRequest file; file.url = url;
        if (parameters.contains("allowedDomains")) file.allowedDomains = parameters["allowedDomains"].get<std::vector<std::string>>();
        file.maximumBytes = parameters.value("maximumBytes", std::size_t(256 * 1024 * 1024));
        file.timeoutMilliseconds = 30000;
        const auto response = httpRequest(file, stop);
        ensure(response.ok(), response.error.empty() ? "Download failed: HTTP " + std::to_string(response.status) : response.error);
        if (stop.stop_requested()) throw std::runtime_error("Cancelled");
        auto temporary = path; temporary += ".download";
        writeFile(temporary, response.body);
        try {
            const auto hash = sha256File(temporary);
            if (parameters.contains("sha256")) ensure(lower(parameters.at("sha256").get<std::string>()) == hash, "Downloaded file SHA-256 does not match the expected digest");
            try { sys::replaceFile(temporary, path); } catch (const std::exception&) { throw std::runtime_error("Cannot commit downloaded file"); }
            return {{"path", utf8Path(path)}, {"sha256", hash}, {"bytes", response.body.size()}};
        } catch (...) { std::error_code ignored; std::filesystem::remove(temporary, ignored); throw; }
    };
    if (operation == "opensubtitles.login") {
        openSubtitles(); request.method = "POST"; request.url = config.openSubtitlesBase + "/login";
        request.headers["Content-Type"] = "application/json";
        request.body = Json{{"username", parameters.value("username", config.openSubtitlesUsername)}, {"password", parameters.value("password", config.openSubtitlesPassword)}}.dump();
        return requestJson(request, stop);
    }
    if (operation == "opensubtitles.search") {
        openSubtitles(); request.url = config.openSubtitlesBase + "/subtitles?query=" + encode(query);
        if (parameters.contains("languages")) request.url += "&languages=" + encode(parameters["languages"].get<std::string>());
        for (const char* key : {"moviehash", "imdb_id", "tmdb_id", "season_number", "episode_number", "hearing_impaired", "foreign_parts_only"})
            if (parameters.contains(key)) request.url += "&" + std::string(key) + "=" + encode(parameters[key].is_string() ? parameters[key].get<std::string>() : parameters[key].dump());
        const auto response = requestJson(request, stop); Json results = Json::array();
        for (const auto& item : response.value("data", Json::array())) {
            const auto attributes = item.value("attributes", Json::object());
            for (const auto& file : attributes.value("files", Json::array()))
                results.push_back({{"provider", "opensubtitles"}, {"id", item.value("id", "")}, {"title", attributes.value("release", "")},
                    {"language", attributes.value("language", "")}, {"file_id", file.at("file_id")}, {"filename", file.value("file_name", "subtitle.srt")}});
        }
        return {{"results", results}, {"raw", response}};
    }
    if (operation == "opensubtitles.download") {
        openSubtitles(); request.method = "POST"; request.url = config.openSubtitlesBase + "/download";
        request.headers["Content-Type"] = "application/json"; request.body = Json{{"file_id", parameters.at("file_id")}}.dump();
        const auto response = requestJson(request, stop); return download(response.at("link").get<std::string>());
    }
    if (operation.starts_with("assrt.")) {
        ensure(!config.assrtToken.empty(), "Configure an Assrt API token");
        request.headers["Authorization"] = "Bearer " + config.assrtToken;
        request.url = config.assrtBase + (operation == "assrt.search" ? "/sub/search?q=" + encode(query) + "&filelist=1" : "/sub/detail?id=" + encode(parameters.at("id").is_string() ? parameters["id"].get<std::string>() : parameters["id"].dump()));
        const auto response = requestJson(request, stop);
        ensure(response.value("status", -1) == 0, "Assrt API reported an error: " + response.dump());
        if (operation == "assrt.download") {
            const auto& files = response.at("sub").at("subs").at(0).at("filelist");
            const auto index = parameters.value("fileIndex", std::size_t(0));
            ensure(index < files.size(), "Invalid Assrt file index"); return download(files[index].at("url").get<std::string>());
        }
        Json results = Json::array();
        for (const auto& item : response.at("sub").value("subs", Json::array())) results.push_back({{"provider", "assrt"}, {"id", item.at("id")},
            {"title", item.value("native_name", item.value("videoname", ""))}, {"language", item.value("lang", Json::object()).value("desc", "")}, {"files", item.value("filelist", Json::array())}});
        return {{"results", results}, {"raw", response}};
    }
    if (operation == "online.resolve") {
        const auto response = Json::parse(runProcess(config.ytDlpExecutable,
            {"--no-playlist", "--no-warnings", "--dump-single-json", "--", parameters.at("url").get<std::string>()}, stop));
        Json result{{"title", response.value("title", "")}, {"httpHeaders", response.value("http_headers", Json::object())}, {"raw", response}};
        if (response.contains("requested_formats") && !response["requested_formats"].empty()) {
            result["url"] = response["requested_formats"][0].at("url");
            if (response["requested_formats"].size() > 1) result["audioUrl"] = response["requested_formats"][1].at("url");
        } else result["url"] = response.at("url");
        return result;
    }
    if (operation == "process.exec") {
        const auto result = executeProcess(pathUtf8(parameters.at("executable").get<std::string>()),
            parameters.value("args",std::vector<std::string>{}),stop,pathUtf8(parameters.value("cwd",std::string{})),[&](bool isError,const std::string& text){
                if(progress && parameters.value(isError?"streamStderr":"streamStdout",true))progress(isError,text);
            });
        return {{"status",result.status},{"stdout",result.output},{"stderr",result.error}};
    }
    if (operation == "plugins.catalog") { request.url = config.pluginCatalog; return requestJson(request, stop); }
    if (operation == "updates.check") {
        request.url = parameters.at("url").get<std::string>();
        auto manifest = requestJson(request, stop); Json asset;
        if (manifest.contains("assets")) {
            for (const auto& candidate : manifest["assets"]) if(candidate.value("kind","")==parameters.value("kind","portable")){asset=candidate;break;}
            ensure(!asset.is_null(),"No compatible update asset was published");
            manifest["url"]=asset.at("url");manifest["sha256"]=asset.at("sha256");
        }
        ensure(manifest.contains("version") && manifest.contains("url") && manifest.contains("sha256"), "Invalid update manifest");
        const auto numbers=[](const std::string& version){std::vector<unsigned long> values;const std::regex expression("[0-9]+");for(auto it=std::sregex_iterator(version.begin(),version.end(),expression);it!=std::sregex_iterator()&&values.size()<4;++it)values.push_back(std::stoul(it->str()));values.resize(4);return values;};
        const bool available=numbers(manifest["version"].get<std::string>())>numbers(parameters.value("currentVersion",std::string("0")));
        return {{"available",available},{"manifest",manifest},{"asset",asset}};
    }
    if (operation == "download.verified") {
        ensure(parameters.contains("sha256") && std::regex_match(parameters["sha256"].get<std::string>(),std::regex("[0-9a-fA-F]{64}")), "An expected SHA-256 digest is required");
        return download(parameters.at("url").get<std::string>());
    }
    if(operation=="plugin.download")return download(parameters.at("url").get<std::string>());
    if (operation == "http.request") {
        request.url = parameters.at("url").get<std::string>(); request.method = parameters.value("method", "GET");
        request.body = parameters.value("body", "");
        if (parameters.contains("headers")) request.headers = parameters["headers"].get<std::map<std::string, std::string>>();
        if (parameters.contains("allowedDomains")) request.allowedDomains = parameters["allowedDomains"].get<std::vector<std::string>>();
        const auto response = httpRequest(request, stop);
        ensure(response.error.empty(), response.error);
        Json json = Json::parse(response.body, nullptr, false);
        const auto text=Json::parse(Json(response.body).dump(-1,' ',false,Json::error_handler_t::replace));
        return {{"statusCode", response.status}, {"ok", response.ok()}, {"text", text}, {"data", json.is_discarded() ? Json(nullptr) : json}};
    }
    throw std::runtime_error("Unknown online operation: " + operation);
}
} // namespace

struct OnlineServices::Impl {
    struct Job { std::jthread thread; std::optional<ServiceResult> result; std::string output,error; bool privateResult = false,abandoned=false; };
    std::mutex mutex;
    OnlineConfiguration configuration;
    std::map<std::uint64_t, std::shared_ptr<Job>> jobs;
    std::uint64_t next = 1;
};
OnlineServices::OnlineServices(OnlineConfiguration configuration) : impl_(std::make_unique<Impl>()) { impl_->configuration = std::move(configuration); }
OnlineServices::~OnlineServices() {
    cancelAll();
    std::map<std::uint64_t, std::shared_ptr<Impl::Job>> jobs;
    { std::lock_guard lock(impl_->mutex); jobs.swap(impl_->jobs); }
    for (auto& [id, job] : jobs) { (void)id; if (job->thread.joinable()) job->thread.join(); }
}
void OnlineServices::configure(OnlineConfiguration configuration) { std::lock_guard lock(impl_->mutex); impl_->configuration = std::move(configuration); }
std::uint64_t OnlineServices::submit(std::string operation, Json parameters, bool privateResult) {
    std::lock_guard lock(impl_->mutex);
    ensure(impl_->jobs.size() < 64, "Too many pending online operations");
    const auto id = impl_->next++; const auto config = impl_->configuration;
    auto job = std::make_shared<Impl::Job>(); job->privateResult = privateResult; impl_->jobs[id] = job;
    job->thread = std::jthread([this, job, id, operation = std::move(operation), parameters = std::move(parameters), config](std::stop_token stop) {
        ServiceResult result; result.id = id; result.operation = operation;
        try { result.data = serviceOperation(operation, parameters, config, stop,[this,job](bool isError,const std::string& text){
            std::lock_guard progressLock(impl_->mutex);if(!job->abandoned)(isError?job->error:job->output)+=text;
        }); }
        catch (const std::exception& error) { result.error = error.what(); }
        result.cancelled = stop.stop_requested();
        std::lock_guard resultLock(impl_->mutex); job->result = std::move(result);
    });
    return id;
}
void OnlineServices::cancel(std::uint64_t id) { std::lock_guard lock(impl_->mutex); if (auto it = impl_->jobs.find(id); it != impl_->jobs.end()) it->second->thread.request_stop(); }
void OnlineServices::abandon(std::uint64_t id){std::shared_ptr<Impl::Job> job;{std::lock_guard lock(impl_->mutex);if(auto found=impl_->jobs.find(id);found!=impl_->jobs.end()){job=found->second;job->abandoned=true;}}if(job)job->thread.request_stop();}
void OnlineServices::cancelAll() { std::lock_guard lock(impl_->mutex); for (auto& [id, job] : impl_->jobs) { (void)id; job->thread.request_stop(); } }
std::vector<ServiceResult> OnlineServices::poll() {
    return pollFor({});
}
std::vector<ServiceResult> OnlineServices::pollFor(const std::set<std::uint64_t>& ids) {
    std::vector<ServiceResult> results; std::vector<std::shared_ptr<Impl::Job>> finished;
    { std::lock_guard lock(impl_->mutex);
      for (auto it = impl_->jobs.begin(); it != impl_->jobs.end();) {
          auto& job=*it->second;
          if(!job.abandoned && ((ids.empty() && !job.privateResult) || ids.contains(it->first))) {
              for(bool isError:{false,true}){auto& text=isError?job.error:job.output;
                  if(!text.empty()){results.push_back({it->first,isError?"process.stderr":"process.stdout",std::move(text),"",false});text.clear();}}
          }
          if (it->second->result && (it->second->abandoned || (ids.empty() && !it->second->privateResult) || ids.contains(it->first))) { if(!it->second->abandoned)results.push_back(std::move(*it->second->result)); finished.push_back(it->second); it = impl_->jobs.erase(it); }
          else ++it;
      } }
    for (auto& job : finished) if (job->thread.joinable()) job->thread.join();
    return results;
}

namespace {
bool sameComponent(const std::filesystem::path& left, const std::filesystem::path& right) {
#ifdef _WIN32
    return _wcsicmp(left.c_str(), right.c_str()) == 0;  // Windows paths are case-insensitive.
#else
    return left == right;
#endif
}
bool linkOrReparsePoint(const std::filesystem::directory_entry& entry) {
#ifdef _WIN32
    return (GetFileAttributesW(entry.path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return entry.is_symlink();
#endif
}
bool within(const std::filesystem::path& path, const std::filesystem::path& root) {
    const auto normalized = std::filesystem::weakly_canonical(path);
    const auto base = std::filesystem::weakly_canonical(root);
    auto current = normalized.begin();
    for (const auto& component : base) {
        if (current == normalized.end() || !sameComponent(*current, component)) return false;
        ++current;
    }
    return true;
}
std::filesystem::path safeRelative(const std::string& value) {
    ensure(!value.empty() && value.find('\0') == std::string::npos && value.find(':') == std::string::npos, "Invalid package-relative path");
    auto path = pathUtf8(value);
    ensure(!path.is_absolute() && !path.has_root_path(), "Package path must be relative");
    for (const auto& part : path) {
        const auto name = lower(utf8Path(part));
        ensure(name != ".." && !name.ends_with('.') && !name.ends_with(' '), "Unsafe package path");
        const auto stem = name.substr(0, name.find('.'));
        ensure(stem != "con" && stem != "prn" && stem != "aux" && stem != "nul"
            && !std::regex_match(stem, std::regex("(?:com|lpt)[1-9]")), "Reserved Windows file name");
    }
    return path;
}
PluginInfo manifestAt(const std::filesystem::path& directory) {
    PluginInfo info; info.directory = directory;
    info.manifest = Json::parse(readFile(directory / "Info.json", 1024 * 1024));
    const auto& manifest = info.manifest;
    info.name = manifest.at("name").get<std::string>();
    info.identifier = manifest.at("identifier").get<std::string>();
    info.version = manifest.at("version").get<std::string>();
    info.entry = manifest.value("entry", "");
    info.description = manifest.value("description", "");
    if (manifest.contains("author")) info.author = manifest["author"].is_string() ? manifest["author"].get<std::string>() : manifest["author"].value("name", "");
    ensure(std::regex_match(info.identifier, std::regex("[A-Za-z0-9_-]+(?:\\.[A-Za-z0-9_-]+)+")) && info.identifier.size() <= 200, "Plugin identifier must use reverse domain notation");
    ensure(!info.name.empty() && !info.version.empty(), "Plugin name and version are required");
    ensure(!info.entry.empty() || manifest.contains("globalEntry"), "Plugin requires entry or globalEntry");
    for (const auto& key : {"entry", "globalEntry"}) if (manifest.contains(key)) {
        const auto entry = directory / safeRelative(manifest.at(key).get<std::string>());
        ensure(within(entry, directory) && std::filesystem::is_regular_file(entry), "Plugin entry does not exist inside the package");
    }
    const std::set<std::string> known{"network-request", "show-osd", "show-alert", "video-overlay", "file-system"};
    for (const auto& permission : manifest.value("permissions", Json::array())) {
        const auto name = permission.get<std::string>();
        ensure(known.contains(name), "Unsupported plugin permission: " + name); info.permissions.insert(name);
    }
    if (manifest.contains("allowedDomains")) info.domains = manifest["allowedDomains"].get<std::vector<std::string>>();
    return info;
}
std::filesystem::path packageRoot(const std::filesystem::path& directory) {
    if (std::filesystem::is_regular_file(directory / "Info.json")) return directory;
    std::filesystem::path found;
    for (const auto& item : std::filesystem::recursive_directory_iterator(directory)) {
        if (item.path().filename() == L"Info.json") {
            ensure(found.empty(), "Archive contains multiple plugin manifests"); found = item.path().parent_path();
        }
    }
    ensure(!found.empty(), "Archive contains no Info.json"); return found;
}
void extractPackage(const std::filesystem::path& source, const std::filesystem::path& destination) {
    std::filesystem::create_directories(destination);
    std::uint64_t total = 0;
    if (std::filesystem::is_directory(source)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(source)) {
            ensure(!linkOrReparsePoint(entry), "Plugin packages cannot contain symbolic links or reparse points");
            const auto relative = safeRelative(utf8Path(std::filesystem::relative(entry.path(), source)));
            const auto target = destination / relative;
            if (entry.is_directory()) std::filesystem::create_directories(target);
            else if (entry.is_regular_file()) {
                const auto size = entry.file_size(); total += size;
                ensure(total <= 128 * 1024 * 1024 && size <= 64 * 1024 * 1024, "Plugin package exceeds its size limit");
                std::filesystem::create_directories(target.parent_path()); std::filesystem::copy_file(entry.path(), target);
            } else throw std::runtime_error("Unsupported package file type");
        }
        return;
    }
    const auto bytes = readFile(source, 64 * 1024 * 1024);
    mz_zip_archive archive{};
    ensure(mz_zip_reader_init_mem(&archive, bytes.data(), bytes.size(), 0) != 0, "Invalid plugin ZIP archive");
    struct ZipCleanup { mz_zip_archive* archive; ~ZipCleanup() { mz_zip_reader_end(archive); } } cleanup{&archive};
    const auto count = mz_zip_reader_get_num_files(&archive); ensure(count <= 4096, "Plugin archive has too many files");
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{}; ensure(mz_zip_reader_file_stat(&archive, i, &stat) != 0, "Cannot read archive entry");
        ensure(((stat.m_external_attr >> 16) & 0170000) != 0120000, "Plugin archive contains a symbolic link");
        std::string name = stat.m_filename; std::replace(name.begin(), name.end(), '\\', '/');
        while (!name.empty() && name.back() == '/') name.pop_back();
        if (name.empty()) continue;
        const auto target = destination / safeRelative(name);
        ensure(within(target, destination), "Archive entry escapes the staging directory");
        if (mz_zip_reader_is_file_a_directory(&archive, i)) { std::filesystem::create_directories(target); continue; }
        total += stat.m_uncomp_size;
        ensure(stat.m_uncomp_size <= 64 * 1024 * 1024 && total <= 128 * 1024 * 1024, "Plugin archive exceeds its decompressed size limit");
        size_t length = 0; void* data = mz_zip_reader_extract_to_heap(&archive, i, &length, 0);
        ensure(data != nullptr, "Cannot decompress plugin entry");
        try { writeFile(target, std::string(static_cast<char*>(data), length)); } catch (...) { mz_free(data); throw; }
        mz_free(data);
    }
}
Json jsJson(JSContext* context, JSValueConst value) {
    if (JS_IsUndefined(value) || JS_IsNull(value)) return nullptr;
    JSValue serialized = JS_JSONStringify(context, value, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(serialized)) { JS_FreeValue(context, JS_GetException(context)); return nullptr; }
    const char* text = JS_ToCString(context, serialized);
    Json result = text ? Json::parse(text, nullptr, false) : Json(nullptr);
    if (text) JS_FreeCString(context, text);
    JS_FreeValue(context, serialized);
    return result.is_discarded() ? Json(nullptr) : result;
}
JSValue jsonJs(JSContext* context, const Json& value) {
    const auto text = value.dump(); return JS_ParseJSON(context, text.data(), text.size(), "<host>");
}
} // namespace

struct PluginHost::Impl {
    bool globalEntriesEnabled=true;
    unsigned executionDepth=0;
    bool reloadPending=false;
    std::deque<std::function<void()>> pendingEvents;
    struct Execution {Impl& owner;explicit Execution(Impl& value):owner(value){++owner.executionDepth;}~Execution(){--owner.executionDepth;}};
    struct Runtime {
        Impl& host; PluginInfo info; JSRuntime* runtime = nullptr; JSContext* context = nullptr;
        Json preferences = Json::object();
        std::vector<PluginMenuItem> menus;
        std::set<std::string> contextMenuIds;
        std::map<std::uint64_t, std::string> tasks;
        std::set<std::uint64_t> hooks;
        std::unique_ptr<PluginWebSocketServer> websocket;
        struct FileHandle {std::fstream stream;std::uint64_t offset=0;bool writable=false;};
        std::map<std::uint64_t,std::unique_ptr<FileHandle>> files;
        std::uint64_t nextFile=0;
        std::chrono::steady_clock::time_point deadline;
        Runtime(Impl& owner, PluginInfo plugin) : host(owner), info(std::move(plugin)) {
            runtime = JS_NewRuntime(); ensure(runtime != nullptr, "Cannot create JavaScript runtime");
            JS_SetMemoryLimit(runtime, 64 * 1024 * 1024); JS_SetMaxStackSize(runtime, 512 * 1024);
            JS_SetInterruptHandler(runtime, [](JSRuntime*, void* opaque) -> int {
                return std::chrono::steady_clock::now() > static_cast<Runtime*>(opaque)->deadline;
            }, this);
            context = JS_NewContext(runtime); ensure(context != nullptr, "Cannot create JavaScript context");
            JS_SetContextOpaque(context, this);
            auto global = JS_GetGlobalObject(context);
            JS_SetPropertyStr(context, global, "__velinHost", JS_NewCFunction(context, &Runtime::bridge, "__velinHost", 2));
            JS_FreeValue(context, global);
            const auto file = host.root / ".preferences" / (info.identifier + ".json");
            if (std::filesystem::is_regular_file(file)) preferences = Json::parse(readFile(file), nullptr, false);
            if (!preferences.is_object()) preferences = Json::object();
            try { eval(iinaCompatibilityScript); }
            catch (...) { JS_FreeContext(context); context = nullptr; JS_FreeRuntime(runtime); runtime = nullptr; throw; }
        }
        ~Runtime() {
            for (const auto& [id, kind] : tasks) { (void)kind; host.services.abandon(id); }
            if (host.callbacks.ui) try { host.callbacks.ui(info,"plugin.unload",Json::array()); } catch (...) {}
            if (context) JS_FreeContext(context);
            if (runtime) JS_FreeRuntime(runtime);
        }
        static JSValue bridge(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = static_cast<Runtime*>(JS_GetContextOpaque(context));
            if (!self || argc < 1) return JS_ThrowTypeError(context, "Invalid native invocation");
            const char* name = JS_ToCString(context, argv[0]);
            if (!name) return JS_EXCEPTION;
            const std::string operation(name); JS_FreeCString(context, name);
            try { return jsonJs(context, self->host.invoke(*self, operation, argc > 1 ? jsJson(context, argv[1]) : Json::array())); }
            catch (const std::exception& error) { return JS_ThrowInternalError(context, "%s", error.what()); }
        }
        Json eval(const std::string& source) {
            Execution execution(host);
            deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            JSValue value = JS_Eval(context, source.data(), source.size(), info.entry.c_str(), JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(value)) {
                JSValue exception = JS_GetException(context); const char* message = JS_ToCString(context, exception);
                const std::string error = message ? message : "JavaScript exception";
                if (message) JS_FreeCString(context, message);
                JS_FreeValue(context, exception); JS_FreeValue(context, value);
                throw std::runtime_error(error);
            }
            const auto result = jsJson(context, value); JS_FreeValue(context, value); return result;
        }
        void jobs() {
            Execution execution(host);
            deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
            for (int i = 0; i < 128 && JS_IsJobPending(runtime); ++i) {
                JSContext* jobContext = nullptr;
                if (JS_ExecutePendingJob(runtime, &jobContext) < 0) {
                    JSValue error = JS_GetException(jobContext); const char* text = JS_ToCString(jobContext, error);
                    host.log(info.identifier, text ? text : "Unhandled Promise rejection");
                    if (text) JS_FreeCString(jobContext, text);
                    JS_FreeValue(jobContext, error); break;
                }
            }
        }
    };
    std::filesystem::path root;
    MediaEngine& engine; OnlineServices& services; ExtensionCallbacks callbacks;
    Json settings = Json::object();
    std::map<std::string, std::unique_ptr<Runtime>> runtimes;
    std::map<std::string, PluginInfo> known;
    std::uint64_t nextHook = 100000;
    Impl(std::filesystem::path data, MediaEngine& player, OnlineServices& online, ExtensionCallbacks hooks, std::filesystem::path pluginDirectory)
        : root(pluginDirectory.empty() ? std::move(data) / "plugins" : std::move(pluginDirectory)), engine(player), services(online), callbacks(std::move(hooks)) {
        std::filesystem::create_directories(root);
        if (std::filesystem::is_regular_file(root / "state.json")) settings = Json::parse(readFile(root / "state.json"), nullptr, false);
        if (!settings.is_object()) settings = Json::object();
    }
    void log(const std::string& plugin, const std::string& text) { if (callbacks.log) callbacks.log(plugin, text); }
    void saveSettings() { writeFile(root / "state.json", settings.dump(2)); }
    bool granted(const Runtime& runtime, const std::string& permission) const { return runtime.info.permissions.contains(permission); }
    void requirePermission(const Runtime& runtime, const std::string& permission) const {
        ensure(granted(runtime, permission), "Plugin requires the '" + permission + "' permission");
    }
    std::filesystem::path resolve(Runtime& runtime, const std::string& value, bool write = false) {
        const auto id = runtime.info.identifier;
        std::filesystem::path base, path;
        bool localWrite = false;
        if (value == "@data" || value.starts_with("@data/")) { base = root / ".data" / id; localWrite = true; }
        else if (value == "@tmp" || value.starts_with("@tmp/")) { base = root / ".tmp" / id; localWrite = true; }
        else if (value.starts_with('@')) throw std::runtime_error("Unknown plugin path prefix");
        if (!base.empty()) {
            std::filesystem::create_directories(base);
            const auto slash = value.find('/'); path = slash == std::string::npos ? base : base / safeRelative(value.substr(slash + 1));
            ensure(within(path, base), "Plugin path escapes its data directory");
        } else {
            path = pathUtf8(value);
            if (!path.is_absolute()) { base = runtime.info.directory; path = base / safeRelative(value); }
            if (!base.empty()) ensure(within(path, base), "Plugin path escapes its package");
            else requirePermission(runtime, "file-system");
        }
        if (write && !localWrite) requirePermission(runtime, "file-system");
        return path;
    }
    std::string playable(Runtime& runtime, const std::string& value) {
        if (value.starts_with("http://") || value.starts_with("https://")) {
            requirePermission(runtime, "network-request");
            ensure(!runtime.info.domains.empty() && domainAllowed(sys::parseHttpUrl(value).host, runtime.info.domains), "URL is not allowed by plugin domains");
            return value;
        }
        return utf8Path(resolve(runtime, value));
    }
    Json ui(Runtime& runtime, const std::string& name, const Json& args) {
        if (name == "osd") requirePermission(runtime, "show-osd");
        if (name == "alert" || name == "utils.ask" || name == "utils.prompt" || name == "utils.chooseFile") requirePermission(runtime, "show-alert");
        if (name.starts_with("overlay.")) requirePermission(runtime, "video-overlay");
        ensure(static_cast<bool>(callbacks.ui), "Native UI operation is unavailable: " + name);
        const auto start=std::chrono::steady_clock::now();
        struct WaitBudget{Runtime& runtime;std::chrono::steady_clock::time_point start;~WaitBudget(){const auto elapsed=std::chrono::steady_clock::now()-start;if(elapsed>std::chrono::milliseconds(100))runtime.deadline+=elapsed;}} budget{runtime,start};
        return callbacks.ui(runtime.info, name, args);
    }
    Json invoke(Runtime& runtime, const std::string& name, const Json& args);
};

Json PluginHost::Impl::invoke(Runtime& runtime, const std::string& name, const Json& args) {
    const auto string = [&](std::size_t index) { ensure(args.is_array() && index < args.size() && args[index].is_string(), "Expected a string argument"); return args[index].get<std::string>(); };
    if (name.starts_with("console.")) {
        std::string text; for (const auto& value : args) { if (!text.empty()) text += ' '; text += value.is_string() ? value.get<std::string>() : value.dump(); }
        log(runtime.info.identifier, text); return nullptr;
    }
    if (name == "module.read") {
        auto specifier = string(0); const auto parent = args.size() > 1 ? string(1) : std::string{};
        std::filesystem::path path;
        if (!parent.empty()) {
            const auto base = pathUtf8(parent); ensure(within(base, runtime.info.directory), "Module parent is outside the plugin");
            path = base / pathUtf8(specifier);
        } else path = runtime.info.directory / pathUtf8(specifier);
        ensure(within(path, runtime.info.directory), "Module path escapes its plugin");
        if (!std::filesystem::is_regular_file(path) && path.extension().empty()) path += L".js";
        if (std::filesystem::is_directory(path)) path /= L"index.js";
        ensure(within(path, runtime.info.directory), "Module path escapes its plugin");
        if (path.extension() == L".json") return {{"path", utf8Path(path)}, {"source", "module.exports=" + Json::parse(readFile(path)).dump() + ";"}};
        return {{"path", utf8Path(path)}, {"source", readFile(path, 4 * 1024 * 1024)}};
    }
    if (name == "mpv.get") return Json::parse(engine.propertyJson(string(0)));
    if (name == "mpv.set") {
        const auto property = string(0);
        const std::set<std::string> safe{"pause","volume","mute","speed","time-pos","playlist-pos","chapter","aid","sid","vid","secondary-sid","sub-delay","sub-scale","sub-pos","sub-visibility","audio-delay","brightness","contrast","gamma","saturation","hue","deinterlace","video-rotate","video-aspect-override","video-zoom","video-pan-x","video-pan-y","loop-file","loop-playlist","ab-loop-a","ab-loop-b"};
        if (!safe.contains(property)) requirePermission(runtime, "file-system");
        ensure(args.size() > 1 && engine.setPropertyJson(property, args[1].dump()), "mpv rejected the property update"); return true;
    }
    if (name == "mpv.command") {
        const auto command = string(0); const std::set<std::string> safe{"seek","cycle","set","add","multiply","playlist-next","playlist-prev","playlist-remove","playlist-move","playlist-shuffle","playlist-unshuffle","stop","frame-step","frame-back-step","ab-loop","revert-seek","sub-seek","sub-step","show-text"};
        if (!safe.contains(command)) requirePermission(runtime, "file-system");
        std::vector<std::string> values{command};
        if (args.size() > 1) for (const auto& argument : args[1]) values.push_back(argument.is_string() ? argument.get<std::string>() : argument.dump());
        // set/add/cycle can mutate file-system-sensitive options, too.
        if ((command == "set" || command == "add" || command == "cycle") && values.size() > 1) {
            const std::set<std::string> allowed{"pause","volume","mute","speed","aid","sid","vid","secondary-sid","chapter","playlist-pos","sub-delay","sub-scale","sub-pos","sub-visibility","audio-delay","brightness","contrast","gamma","saturation","hue","deinterlace","video-rotate","loop-file","loop-playlist"};
            if (!allowed.contains(values[1])) requirePermission(runtime, "file-system");
        }
        return engine.command(values);
    }
    if (name == "mpv.addHook") {
        const auto id = ++nextHook; ensure(engine.addHook(string(0), args.at(1).get<int>(), id), "Cannot register mpv hook"); runtime.hooks.insert(id); return id;
    }
    if (name == "mpv.continueHook") { engine.continueHook(args.at(0).get<std::uint64_t>()); return nullptr; }
    if (name == "event.observe") {
        const auto event = string(0);
        ensure(event.starts_with("mpv.") || event.starts_with("iina."), "Event names must begin with mpv. or iina.");
        if (event.starts_with("mpv.") && event.ends_with(".changed")) engine.observe(event.substr(4, event.size() - 12));
        return true;
    }
    if (name == "core.open") return engine.open({playable(runtime, string(0))});
    if (name == "core.version") return {{"iina", "1.4.0"}, {"build", "velin-windows"}, {"velin", VELIN_VERSION}, {"mpv", Json::parse(engine.propertyJson("mpv-version"))}};
    if (name == "core.chapters") { Json values = Json::array(); for (const auto& chapter : engine.state().chapters) values.push_back({{"title", chapter.title}, {"start", chapter.time}}); return values; }
    if (name == "core.track.load") {
        const auto type = string(0), path = playable(runtime, string(1));
        if (type == "audio") engine.addAudio(path); else if (type == "subtitle") engine.addSubtitle(path);
        else ensure(engine.command({"video-add", path, "select"}), "Cannot load external video track");
        return true;
    }
    if (name == "core.get" || name == "core.set") {
        const auto group = string(0), property = string(1); const bool write = name == "core.set";
        if (group == "window") return ui(runtime, write ? "core.window.set" : "core.window.get", write ? Json::array({property, args.at(2)}) : Json::array({property}));
        const auto& state = engine.state();
        if (group == "status") {
            ensure(!write, "core.status is read-only");
            const Json values{{"paused",state.paused},{"idle",state.idle},{"position",state.position},{"duration",state.duration},{"speed",state.speed},
                {"videoWidth",state.width},{"videoHeight",state.height},{"isNetworkResource",state.path.starts_with("http")},{"url",state.path},{"title",state.title}};
            ensure(values.contains(property), "Unknown core.status property"); return values.at(property);
        }
        const std::string type = group == "subtitle" ? "sub" : group;
        if (property == "tracks" || property == "currentTrack") {
            ensure(!write, "Track metadata is read-only"); Json tracks = Json::array();
            for (const auto& track : state.tracks) if (track.type == type) {
                Json value{{"id",track.id},{"title",track.title},{"formattedTitle",track.title},{"lang",track.language},{"codec",track.codec},{"isSelected",track.selected},{"isExternal",track.external}};
                if (property == "currentTrack" && track.selected) return value;
                tracks.push_back(std::move(value));
            }
            return property == "tracks" ? tracks : Json(nullptr);
        }
        std::map<std::string,std::string> mapping;
        if (group == "audio") mapping={{"id","aid"},{"delay","audio-delay"},{"volume","volume"},{"muted","mute"}};
        else if (group == "subtitle") mapping={{"id","sid"},{"secondID","secondary-sid"},{"delay","sub-delay"},{"scale","sub-scale"},{"position","sub-pos"},{"visible","sub-visibility"}};
        else if (group == "video") mapping={{"id","vid"},{"aspect","video-aspect-override"},{"rotation","video-rotate"},{"crop","video-crop"},{"brightness","brightness"},{"contrast","contrast"},{"saturation","saturation"},{"gamma","gamma"},{"hue","hue"},{"deinterlace","deinterlace"}};
        ensure(mapping.contains(property), "Unknown core property: " + group + "." + property);
        if (write) return engine.setPropertyJson(mapping.at(property), args.at(2).dump());
        return Json::parse(engine.propertyJson(mapping.at(property)));
    }
    if (name == "playlist.list") {
        Json values = Json::array(); for (const auto& item : engine.state().playlist) values.push_back({{"filename",item.filename},{"title",item.title},{"isPlaying",item.current},{"isCurrent",item.current}}); return values;
    }
    if (name == "playlist.add") {
        std::vector<std::string> paths; const auto input = args.at(0).is_array() ? args[0] : Json::array({args[0]});
        for (const auto& path : input) paths.push_back(playable(runtime, path.get<std::string>()));
        const auto at = args.size() > 1 ? args[1].get<std::int64_t>() : -1;
        auto count = static_cast<std::int64_t>(engine.state().playlist.size()); ensure(at < count, "Invalid playlist insertion index");
        for (const auto& path : paths) { ensure(engine.open({path}, true), "Cannot append playlist entry"); if (at >= 0) engine.movePlaylist(count, at + count - static_cast<std::int64_t>(engine.state().playlist.size())); ++count; } return true;
    }
    if (name == "playlist.remove") {
        std::vector<std::int64_t> indices = args.at(0).is_array() ? args[0].get<std::vector<std::int64_t>>() : std::vector<std::int64_t>{args[0].get<std::int64_t>()};
        std::sort(indices.rbegin(), indices.rend()); for (auto index : indices) { ensure(index >= 0 && index < static_cast<std::int64_t>(engine.state().playlist.size()), "Invalid playlist index"); engine.command({"playlist-remove", std::to_string(index)}); } return true;
    }
    if (name == "playlist.move") { engine.movePlaylist(args.at(0).get<std::int64_t>(),args.at(1).get<std::int64_t>()); return true; }
    if (name == "playlist.play") { engine.selectPlaylist(args.at(0).get<std::int64_t>()); return true; }
    if (name == "menu.replace") {
        runtime.menus.clear();
        std::function<void(const Json&,unsigned,bool)> append = [&](const Json& items,unsigned depth,bool parentEnabled) {
            ensure(items.is_array() && depth<=16,"Plugin menu exceeds nesting limit");
            for (const auto& item : items) {
                ensure(runtime.menus.size()<1000,"Plugin menu exceeds item limit");
                const bool enabled=parentEnabled && item.value("enabled",true);
                runtime.menus.push_back({runtime.info.identifier,item.at("identifier").get<std::string>(),item.value("title",""),item.value("keyBinding",""),enabled,item.value("selected",false),item.value("separator",false),depth});
                if (item.contains("subItems") && !item["subItems"].empty()) append(item["subItems"],depth+1,enabled);
            }
        };
        append(args,0,true); if (callbacks.menusChanged) callbacks.menusChanged(); return nullptr;
    }

    if (name == "input.bindings") return Json::parse(engine.propertyJson("input-bindings"));
    if (name == "preferences.get") {
        const auto key = string(0); if (runtime.preferences.contains(key)) return runtime.preferences[key];
        return runtime.info.manifest.value("preferenceDefaults", Json::object()).value(key, Json(nullptr));
    }
    if (name == "preferences.set") { runtime.preferences[string(0)] = args.at(1); return nullptr; }
    if (name == "preferences.sync") { writeFile(root / ".preferences" / (runtime.info.identifier + ".json"), runtime.preferences.dump(2)); return nullptr; }
    if(name.starts_with("file.handle.")) {
        const auto operation=name.substr(12);
        if(operation=="open"){
            const auto mode=string(1);ensure(mode=="read" || mode=="write" || mode=="r" || mode=="w" || mode=="a","Unknown file mode");
            const bool writable=mode!="read" && mode!="r";const auto path=resolve(runtime,string(0),writable);
            if(writable && !std::filesystem::exists(path))writeFile(path,"");
            auto file=std::make_unique<Runtime::FileHandle>();file->writable=writable;
            file->stream.open(path,std::ios::binary|std::ios::in|(writable?std::ios::out:std::ios::openmode{}));ensure(file->stream.is_open(),"Cannot open plugin file handle");
            if(mode=="a"){file->stream.seekg(0,std::ios::end);file->offset=static_cast<std::uint64_t>(file->stream.tellg());}
            const auto id=++runtime.nextFile;runtime.files[id]=std::move(file);return id;
        }
        const auto id=args.at(0).get<std::uint64_t>();const auto iterator=runtime.files.find(id);ensure(iterator!=runtime.files.end(),"File handle is closed");auto& file=*iterator->second;
        if(operation=="close"){runtime.files.erase(iterator);return nullptr;}
        if(operation=="offset")return file.offset;
        file.stream.clear();
        if(operation=="seek"){file.offset=args.at(1).get<std::uint64_t>();ensure(file.offset<=static_cast<std::uint64_t>(INT64_MAX),"File position exceeds limit");return nullptr;}
        if(operation=="end"){file.stream.seekg(0,std::ios::end);const auto position=file.stream.tellg();ensure(position>=0,"Cannot seek file");file.offset=static_cast<std::uint64_t>(position);return file.offset;}
        if(operation=="read"){
            std::uint64_t length=0;
            if(args.size()>1 && !args[1].is_null())length=args[1].get<std::uint64_t>();
            else{file.stream.seekg(0,std::ios::end);const auto end=file.stream.tellg();ensure(end>=0,"Cannot determine file length");length=static_cast<std::uint64_t>(end)>file.offset?static_cast<std::uint64_t>(end)-file.offset:0;}
            ensure(length<=16*1024*1024,"File reads are limited to 16 MiB per call; read large files in chunks");
            std::vector<std::uint8_t> bytes(static_cast<size_t>(length));file.stream.seekg(static_cast<std::streamoff>(file.offset));file.stream.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(length));bytes.resize(static_cast<size_t>(file.stream.gcount()));file.offset+=bytes.size();return bytes;
        }
        if(operation=="write"){
            ensure(file.writable,"File handle is read-only");const auto bytes=args.at(1).get<std::vector<std::uint8_t>>();ensure(bytes.size()<=16*1024*1024,"File write exceeds limit");file.stream.seekp(static_cast<std::streamoff>(file.offset));file.stream.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));file.stream.flush();ensure(file.stream.good(),"File write failed");file.offset+=bytes.size();return nullptr;
        }
        throw std::runtime_error("Unknown file handle operation");
    }
    if (name.starts_with("file.")) {
        const auto operation = name.substr(5); const auto path = resolve(runtime,string(0),operation == "write" || operation == "delete" || operation == "trash");
        if (operation == "exists") return std::filesystem::exists(path);
        if (operation == "read") {
            const auto options=args.size()>1 && args[1].is_object()?args[1]:Json::object();
            auto encoding=lower(options.value("encoding","utf-8"));
            const std::map<std::string,std::string> aliases{{"unicode","utf-16"},{"utf16littleendian","utf-16le"},{"utf16bigendian","utf-16be"},
                {"utf32","utf-32"},{"utf32littleendian","utf-32le"},{"utf32bigendian","utf-32be"}};
            if(auto alias=aliases.find(encoding);alias!=aliases.end())encoding=alias->second;
            auto text=readFile(path);
            if (encoding=="utf-8" || encoding=="utf8") {
                if(text.starts_with("\xef\xbb\xbf"))text.erase(0,3);
                ensure(text.empty() || !fromUtf8(text).empty(),"File contains invalid UTF-8");return text;
            }
            if (encoding=="utf-16" || encoding=="utf16" || encoding=="utf-16le" || encoding=="utf-16be") {
                bool big=encoding=="utf-16be";size_t offset=0;
                if(text.starts_with("\xfe\xff")){big=true;offset=2;}
                else if(text.starts_with("\xff\xfe")){big=false;offset=2;}
                ensure((text.size()-offset)%2==0,"File contains truncated UTF-16");
                std::wstring wide;
                for(size_t i=offset;i<text.size();i+=2) {
                    const auto a=static_cast<unsigned char>(text[i]),b=static_cast<unsigned char>(text[i+1]);
                    wide.push_back(static_cast<wchar_t>(big?(a<<8)|b:(b<<8)|a));
                }
                const auto converted=toUtf8(wide);ensure(wide.empty() || !converted.empty(),"File contains invalid UTF-16");return converted;
            }
            if(encoding=="utf-32" || encoding=="utf-32le" || encoding=="utf-32be") {
                bool big=encoding=="utf-32be";size_t offset=0;
                if(text.starts_with(std::string("\0\0\xfe\xff",4))){big=true;offset=4;}
                else if(text.starts_with(std::string("\xff\xfe\0\0",4))){big=false;offset=4;}
                ensure((text.size()-offset)%4==0,"File contains truncated UTF-32");
                std::wstring wide;
                for(size_t i=offset;i<text.size();i+=4){std::uint32_t value=0;
                    for(size_t j=0;j<4;++j)value=(value<<8)|static_cast<unsigned char>(text[i+(big?j:3-j)]);
                    ensure(value<=0x10ffff && !(value>=0xd800 && value<=0xdfff),"File contains invalid UTF-32");
                    if(value<0x10000)wide.push_back(static_cast<wchar_t>(value));
                    else {value-=0x10000;wide.push_back(static_cast<wchar_t>(0xd800+(value>>10)));wide.push_back(static_cast<wchar_t>(0xdc00+(value&1023)));}
                }
                return toUtf8(wide);
            }
            if(encoding=="ascii" || encoding=="isolatin1" || encoding=="iso-8859-1") {
                std::wstring wide;for(unsigned char value:text){ensure(encoding!="ascii" || value<128,"File contains non-ASCII bytes");wide.push_back(value);}return toUtf8(wide);
            }
            const std::map<std::string,unsigned> pages{{"windowscp1250",1250},{"windowscp1251",1251},{"windowscp1252",1252},{"windowscp1253",1253},{"windowscp1254",1254},
                {"windowscp1255",1255},{"windowscp1256",1256},{"windowscp1257",1257},{"windowscp1258",1258},
                {"windowscyrillic",1251},{"windowsgreek",1253},{"windowslatin5",1254},{"windowshebrew",1255},{"windowsarabic",1256},{"windowsbalticrim",1257},{"windowsvietnamese",1258},
                {"windowslatin2",1250},{"isolatin2",28592},{"shiftjis",932},{"shift-jis",932},{"big5",950},{"gbk",936},{"gb_18030_2000",54936},{"gb18030",54936},{"euc_kr",949},{"macosroman",10000}};
            const auto page=pages.find(encoding);
            ensure(page!=pages.end(),"Unsupported text encoding; use file.handle for binary data");
            return sys::decodeCodePage(page->second,text);
        }
        if (operation == "write") { writeFile(path,string(1)); return true; }
        if (operation == "list") {
            const auto options=args.size()>1 && args[1].is_object()?args[1]:Json::object();
            Json files=Json::array();
            auto append=[&](const auto& entry) {
                ensure(files.size()<10000,"Directory listing exceeds 10000 entries");
                // Resolve every result so junctions cannot expose paths outside permissions.
                (void)resolve(runtime,string(0)+"/"+utf8Path(entry.path().lexically_relative(path)));
                files.push_back({{"filename",utf8Path(entry.path().filename())},{"path","/"+toUtf8(entry.path().lexically_relative(path).generic_wstring())},{"isDir",entry.is_directory()}});
            };
            if(options.value("includeSubDir",false)) {
                for(const auto& entry:std::filesystem::recursive_directory_iterator(path))append(entry);
            } else for(const auto& entry:std::filesystem::directory_iterator(path))append(entry);
            return files;
        }
        if (operation == "delete") { ensure(within(path,root/".data"/runtime.info.identifier) || within(path,root/".tmp"/runtime.info.identifier), "file.delete is limited to @data and @tmp"); return std::filesystem::remove_all(path) > 0; }
        return ui(runtime,name,Json::array({utf8Path(path)}));
    }
    if (name == "async.start") {
        auto operation = string(0); auto parameters = args.at(1);
        if (operation == "process.exec") {
            requirePermission(runtime,"file-system"); const auto file = parameters.at("file").get<std::string>();
            auto executable = pathUtf8(file);
            if (!executable.has_parent_path()) {
                executable=sys::findExecutable(file);
                ensure(!executable.empty(),"Executable not found");
            } else executable=resolve(runtime,file);
            parameters["executable"] = utf8Path(executable);
            if (parameters.contains("cwd") && !parameters["cwd"].is_null()) {
                const auto directory=resolve(runtime,parameters.at("cwd").get<std::string>());
                ensure(std::filesystem::is_directory(directory),"Process working directory does not exist");
                parameters["cwd"]=utf8Path(directory);
            } else parameters.erase("cwd");
        } else {
            requirePermission(runtime,"network-request"); ensure(!runtime.info.domains.empty(), "Plugin has no allowed network domains");
            const auto url = parameters.at("url").get<std::string>(); ensure(domainAllowed(sys::parseHttpUrl(url).host,runtime.info.domains), "URL is not allowed by plugin domains");
            parameters["allowedDomains"] = runtime.info.domains;
        }
        if (operation == "plugin.download") {
            const auto destination = resolve(runtime,parameters.at("destination").get<std::string>(),true);
            parameters["destination"] = utf8Path(destination);
        }
        const auto id = services.submit(operation,parameters,true); runtime.tasks[id] = parameters.value("downloadPath",""); return id;
    }
    if (name == "utils.resolvePath") return utf8Path(resolve(runtime,string(0)));
    if (name.starts_with("ws.")) {
        requirePermission(runtime,"network-request");
        if(name=="ws.create") {
            const auto options=args.at(0);const auto host=options.value("host",std::string("127.0.0.1"));
            ensure(host=="127.0.0.1" || host=="::1" || domainAllowed(host,runtime.info.domains),"WebSocket bind address is not permitted");
            runtime.websocket=std::make_unique<PluginWebSocketServer>();runtime.websocket->create(options);return true;
        }
        ensure(runtime.websocket!=nullptr,"Create a WebSocket server first");
        if(name=="ws.start"){runtime.websocket->start();return runtime.websocket->port();}
        if(name=="ws.stop"){runtime.websocket->stop();return true;}
        if(name=="ws.send"){runtime.websocket->sendText(string(0),string(1),string(2));return true;}
    }
    if (name == "global.createPlayerInstance") {
        auto options=args.at(0); if(options.contains("url")) options["url"]=playable(runtime,options["url"].get<std::string>());
        return ui(runtime,name,Json::array({options}));
    }
    if (name == "utils.fileInPath") { requirePermission(runtime,"file-system"); return !sys::findExecutable(string(0)).empty(); }
    if (name == "utils.open") { const auto path = playable(runtime,string(0)); return ui(runtime,name,Json::array({path})); }
    if (name == "utils.preferredLocalizations") { return Json::array({sys::preferredLocale(),"en-US"}); }
    if (name == "subtitle.register") { return true; }
    if(name=="subtitle.downloaded"){
        auto data=args;if(data.size()>1 && data[1].is_array())for(auto& path:data[1])if(path.is_string())path=utf8Path(resolve(runtime,path.get<std::string>()));
        return ui(runtime,name,data);
    }
    if (name.starts_with("subtitle.")) return ui(runtime,name,args);
    if (name.starts_with("overlay.") || name.starts_with("sidebar.") || name.starts_with("standalone.")) {
        auto arguments = args;
        if (name.ends_with(".loadFile")) arguments[0] = utf8Path(resolve(runtime,string(0)));
        return ui(runtime,name,arguments);
    }
    return ui(runtime,name,args);
}

PluginHost::PluginHost(std::filesystem::path data, MediaEngine& engine, OnlineServices& services, ExtensionCallbacks callbacks,std::filesystem::path pluginDirectory)
    : impl_(std::make_unique<Impl>(std::move(data),engine,services,std::move(callbacks),std::move(pluginDirectory))) {}
PluginHost::~PluginHost() = default;
std::vector<PluginInfo> PluginHost::discover() {
    std::vector<PluginInfo> plugins;
    for (const auto& entry : std::filesystem::directory_iterator(impl_->root)) {
        if (!entry.is_directory() || entry.path().filename().wstring().starts_with(L'.')) continue;
        try {
            auto info = manifestAt(entry.path()); const auto state = impl_->settings.value(info.identifier,Json::object());
            const auto grants = state.value("permissions",std::set<std::string>{});
            info.enabled = state.value("enabled",false) && std::includes(grants.begin(),grants.end(),info.permissions.begin(),info.permissions.end());
            if (auto it = impl_->runtimes.find(info.identifier); it != impl_->runtimes.end()) { info.running = true; info.error = it->second->info.error; }
            else if (auto previous = impl_->known.find(info.identifier); previous != impl_->known.end()) info.error = previous->second.error;
            impl_->known[info.identifier]=info; plugins.push_back(std::move(info));
        } catch (const std::exception& error) { impl_->log(utf8Path(entry.path().filename()),error.what()); }
    }
    return plugins;
}
PluginInfo PluginHost::inspect(const std::filesystem::path& source) {
    if (std::filesystem::is_directory(source)) return manifestAt(source);
    const auto stage=impl_->root/(".inspect-"+std::to_string(sys::processId()));
    try { std::filesystem::remove_all(stage); extractPackage(source,stage); auto info=manifestAt(packageRoot(stage)); info.directory=source; std::filesystem::remove_all(stage); return info; }
    catch (...) { std::error_code ignored; std::filesystem::remove_all(stage,ignored); throw; }
}
PluginInfo PluginHost::install(const std::filesystem::path& source,const std::set<std::string>& approved,bool replace) {
    ensure(!busy(),"Wait for the active plugin callback before installing a package");
    const auto stage=impl_->root/(".install-"+std::to_string(sys::processId()));
    const auto backup=impl_->root/(".backup-"+std::to_string(sys::processId()));
    try {
        std::filesystem::remove_all(stage); extractPackage(source,stage); const auto package=packageRoot(stage); auto info=manifestAt(package);
        ensure(std::includes(approved.begin(),approved.end(),info.permissions.begin(),info.permissions.end()),"All declared plugin permissions require explicit approval");
        const auto target=impl_->root/(info.identifier+".iinaplugin");
        ensure(replace || !std::filesystem::exists(target),"Plugin is already installed");
        impl_->runtimes.erase(info.identifier);
        if (std::filesystem::exists(target)) { std::filesystem::remove_all(backup); std::filesystem::rename(target,backup); }
        try { std::filesystem::rename(package,target); }
        catch (...) { if (std::filesystem::exists(backup)) std::filesystem::rename(backup,target); throw; }
        std::filesystem::remove_all(stage); std::filesystem::remove_all(backup);
        impl_->settings[info.identifier]={{"enabled",true},{"permissions",approved}}; impl_->saveSettings(); reload();
        return manifestAt(target);
    } catch (...) { std::error_code ignored; std::filesystem::remove_all(stage,ignored); throw; }
}
void PluginHost::uninstall(const std::string& identifier) {
    ensure(!busy(),"Wait for the active plugin callback before uninstalling");
    auto plugins=discover(); auto found=std::find_if(plugins.begin(),plugins.end(),[&](const auto& info){return info.identifier==identifier;});
    ensure(found!=plugins.end(),"Plugin not found");
    impl_->runtimes.erase(identifier); ensure(within(found->directory,impl_->root),"Invalid plugin removal path");
    std::filesystem::remove_all(found->directory); impl_->settings.erase(identifier); impl_->known.erase(identifier); impl_->saveSettings();
    if (impl_->callbacks.menusChanged) impl_->callbacks.menusChanged();
}
void PluginHost::setEnabled(const std::string& identifier,bool enabled) {
    const auto plugins=discover(); ensure(std::any_of(plugins.begin(),plugins.end(),[&](const auto& info){return info.identifier==identifier;}),"Plugin not found");
    impl_->settings[identifier]["enabled"]=enabled; impl_->saveSettings(); reload();
}
void PluginHost::reload() {
    if(busy()){impl_->reloadPending=true;return;}
    impl_->reloadPending=false;impl_->pendingEvents.clear();
    impl_->runtimes.clear(); const auto plugins=discover();
    for (const auto& info:plugins) if (info.enabled) {
        try {
            auto runtime=std::make_unique<Impl::Runtime>(*impl_,info);
            if (impl_->globalEntriesEnabled && info.manifest.contains("globalEntry")) runtime->eval("__velinLoad("+Json(info.manifest["globalEntry"]).dump()+")");
            if (!info.entry.empty()) runtime->eval("__velinLoad("+Json(info.entry).dump()+")");
            impl_->runtimes[info.identifier]=std::move(runtime); impl_->known[info.identifier].error.clear();
        } catch (const std::exception& error) { impl_->known[info.identifier].error=error.what(); impl_->log(info.identifier,error.what()); }
    }
    if (impl_->callbacks.menusChanged) impl_->callbacks.menusChanged();
}
std::vector<PluginMenuItem> PluginHost::menuItems() const {
    std::vector<PluginMenuItem> items; for (const auto& [id,runtime]:impl_->runtimes) { (void)id; items.insert(items.end(),runtime->menus.begin(),runtime->menus.end()); } return items;
}
bool PluginHost::invokeMenu(const std::string& plugin,const std::string& identifier) {
    const auto found=impl_->runtimes.find(plugin); if (found==impl_->runtimes.end()) return false;
    const auto& items=found->second->menus;
    if (!found->second->contextMenuIds.contains(identifier) && !std::any_of(items.begin(),items.end(),[&](const auto& item){return item.identifier==identifier&&item.enabled&&!item.separator;})) return false;
    try { found->second->eval("__velinMenuInvoke("+Json(identifier).dump()+")"); found->second->jobs(); return true; }
    catch (const std::exception& error) { impl_->log(plugin,error.what()); return false; }
}
void PluginHost::emit(const std::string& event,const Json& arguments) {
    if(busy()){if(impl_->pendingEvents.size()<4096)impl_->pendingEvents.push_back([this,event,arguments]{emit(event,arguments);});return;}
    if (event=="mpv.hook" && arguments.is_array() && arguments.size()>=2) {
        for (auto& [id,runtime]:impl_->runtimes) if (runtime->hooks.contains(arguments[0].get<std::uint64_t>())) {
            try { runtime->eval("__velinHook("+Json(std::to_string(arguments[0].get<std::uint64_t>())).dump()+","+arguments[1].dump()+")"); runtime->jobs(); }
            catch (const std::exception& error) { impl_->log(id,error.what()); impl_->engine.continueHook(arguments[1].get<std::uint64_t>()); } return;
        }
        impl_->engine.continueHook(arguments[1].get<std::uint64_t>()); return;
    }
    for (auto& [id,runtime]:impl_->runtimes) try {
        runtime->eval("__velinEmit("+Json(event).dump()+","+(arguments.is_array()?arguments:Json::array({arguments})).dump()+")"); runtime->jobs();
    } catch (const std::exception& error) { impl_->log(id,error.what()); }
}
std::vector<PluginMenuItem> PluginHost::playlistMenuItems(const std::vector<std::string>& filenames){
    std::vector<PluginMenuItem> result;if(busy())return result;
    for(auto& [id,runtime]:impl_->runtimes){runtime->contextMenuIds.clear();try{
        const auto entries=runtime->eval("typeof __velinPlaylistMenuBuilder==='function'?__velinPlaylistMenuBuilder("+Json(filenames).dump()+"):[]");
        std::function<void(const Json&,unsigned,bool)> append=[&](const Json& values,unsigned depth,bool parentEnabled) {
            if(!values.is_array() || depth>16)return;
            for(const auto& value:values) {
                if(result.size()>=1000)return;
                if(!value.is_object() || !value.contains("identifier"))continue;
                const auto key=value["identifier"].get<std::string>();
                const bool enabled=parentEnabled && value.value("enabled",true);
                if(enabled && !value.value("separator",false))runtime->contextMenuIds.insert(key);
                result.push_back({id,key,value.value("title",""),value.value("keyBinding",""),enabled,value.value("selected",false),value.value("separator",false),depth});
                if(value.contains("subItems"))append(value["subItems"],depth+1,enabled);
            }
        };
        append(entries,0,true);
    }catch(const std::exception& error){impl_->log(id,error.what());}}
    return result;
}
void PluginHost::receiveWebMessage(const std::string& plugin,const std::string& kind,const std::string& name,const Json& data) {
    if(busy()){if(impl_->pendingEvents.size()<4096)impl_->pendingEvents.push_back([this,plugin,kind,name,data]{receiveWebMessage(plugin,kind,name,data);});return;}
    const auto found=impl_->runtimes.find(plugin); if (found==impl_->runtimes.end()) return;
    try { found->second->eval("__velinWebMessage("+Json(kind).dump()+","+Json(name).dump()+","+data.dump()+")"); found->second->jobs(); }
    catch (const std::exception& error) { impl_->log(plugin,error.what()); }
}
void PluginHost::receiveGlobalMessage(const std::string& plugin,const std::string& name,const Json& data,const Json& source) {
    if(busy()){if(impl_->pendingEvents.size()<4096)impl_->pendingEvents.push_back([this,plugin,name,data,source]{receiveGlobalMessage(plugin,name,data,source);});return;}
    const auto found=impl_->runtimes.find(plugin); if(found==impl_->runtimes.end())return;
    try { found->second->eval("__velinGlobalMessage("+Json(name).dump()+","+data.dump()+","+source.dump()+")"); found->second->jobs(); }
    catch(const std::exception& error){impl_->log(plugin,error.what());}
}
void PluginHost::searchSubtitleProvider(const std::string& plugin,const std::string& provider) {
    evaluate(plugin,"__velinSearchSubtitle("+Json(provider).dump()+")");
}
void PluginHost::downloadSubtitleProvider(const std::string& plugin,const std::string& provider,const Json& data) {
    evaluate(plugin,"__velinDownloadSubtitle("+Json(provider).dump()+","+data.dump()+")");
}
bool PluginHost::input(const std::string& event,const std::string& key,const Json& arguments) {
    for (auto& [id,runtime]:impl_->runtimes) try {
        if (runtime->eval("__velinInput("+Json(event).dump()+","+Json(key).dump()+","+arguments.dump()+")")==true) return true;
    } catch (const std::exception& error) { impl_->log(id,error.what()); } return false;
}
void PluginHost::poll() {
    if(busy())return;
    if(impl_->reloadPending)reload();
    for(int count=0;count<128 && !impl_->pendingEvents.empty();++count){auto event=std::move(impl_->pendingEvents.front());impl_->pendingEvents.pop_front();event();if(impl_->reloadPending)return;}
    for(auto& [id,runtime]:impl_->runtimes) if(runtime->websocket) {
        for(const auto& event:runtime->websocket->poll()) try {
            runtime->eval("__velinSocketEvent("+Json(event.event).dump()+","+event.args.dump()+")");runtime->jobs();
        }catch(const std::exception& error){impl_->log(id,error.what());}
    }
    std::set<std::uint64_t> pending; for (const auto& [id,runtime]:impl_->runtimes) { (void)id; for (const auto& [task,kind]:runtime->tasks) { (void)kind; pending.insert(task); } }
    const auto results=pending.empty()?std::vector<ServiceResult>{}:impl_->services.pollFor(pending);
    for (const auto& result:results) for (auto& [id,runtime]:impl_->runtimes) if (auto task=runtime->tasks.find(result.id);task!=runtime->tasks.end()) {
        if(result.operation=="process.stdout" || result.operation=="process.stderr") {
            try{runtime->eval("__velinProcessChunk("+std::to_string(result.id)+","+Json(result.operation=="process.stderr"?"stderr":"stdout").dump()+","+result.data.dump()+")");runtime->jobs();}
            catch(const std::exception& error){impl_->log(id,error.what());}
            continue;
        }
        auto error=result.error; auto data=result.data;
        try { if (!task->second.empty() && error.empty()) { ensure(data.value("ok",false),"Download returned an HTTP error"); writeFile(pathUtf8(task->second),data.at("text").get<std::string>()); data={{"path",task->second}}; } }
        catch (const std::exception& exception) { error=exception.what(); }
        runtime->tasks.erase(task);
        try { runtime->eval("__velinAsyncComplete("+std::to_string(result.id)+","+data.dump()+","+Json(error).dump()+")"); runtime->jobs(); }
        catch (const std::exception& exception) { impl_->log(id,exception.what()); }
    }
    for (auto& [id,runtime]:impl_->runtimes) try { runtime->eval("__velinTick()"); runtime->jobs(); } catch (const std::exception& error) { impl_->log(id,error.what()); }
}
Json PluginHost::evaluate(const std::string& identifier,const std::string& source) {
    const auto found=impl_->runtimes.find(identifier); ensure(found!=impl_->runtimes.end(),"Plugin is not running"); return found->second->eval(source);
}
void PluginHost::setGlobalEntriesEnabled(bool enabled) { impl_->globalEntriesEnabled=enabled; }
bool PluginHost::busy() const {return impl_->executionDepth!=0;}
Json PluginHost::compatibility() const {
    return {{"engine","QuickJS-ng 0.11.0"},{"namespaces",Json::array({"mpv","event","core","playlist","menu","input","preferences","file","http","utils","console","overlay","sidebar","standaloneWindow","subtitle"})},
        {"platform","windows"},{"compatibilityTarget","IINA 1.4 JavaScript API; macOS frameworks and native binaries are unavailable"},
        {"memoryLimitBytes",64*1024*1024},{"executionLimitMilliseconds",500}};
}
} // namespace velin
