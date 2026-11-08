// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors

#include "velin/media_engine.hpp"
#include "velin/system.hpp"

#define MPV_ENABLE_DEPRECATED 0
#include "mpv/client.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <filesystem>
#include <limits>
#include <set>
#include <utility>

#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace velin {
namespace {
using Json = nlohmann::json;

Json nodeJson(const mpv_node& node) {
    switch (node.format) {
    case MPV_FORMAT_STRING: return node.u.string ? Json(node.u.string) : Json(nullptr);
    case MPV_FORMAT_FLAG: return node.u.flag != 0;
    case MPV_FORMAT_INT64: return node.u.int64;
    case MPV_FORMAT_DOUBLE: return std::isfinite(node.u.double_) ? Json(node.u.double_) : Json(nullptr);
    case MPV_FORMAT_NODE_ARRAY: {
        Json result = Json::array();
        if (node.u.list) for (int i = 0; i < node.u.list->num; ++i) result.push_back(nodeJson(node.u.list->values[i]));
        return result;
    }
    case MPV_FORMAT_NODE_MAP: {
        Json result = Json::object();
        if (node.u.list) for (int i = 0; i < node.u.list->num; ++i)
            if (node.u.list->keys[i]) result[node.u.list->keys[i]] = nodeJson(node.u.list->values[i]);
        return result;
    }
    default: return nullptr;
    }
}

struct OwnedNode {
    mpv_node node{};
    mpv_node_list list{};
    std::string text;
    std::vector<std::unique_ptr<OwnedNode>> children;
    std::vector<mpv_node> values;
    std::vector<std::string> names;
    std::vector<char*> keys;
    explicit OwnedNode(const Json& value) {
        if (value.is_boolean()) { node.format = MPV_FORMAT_FLAG; node.u.flag = value.get<bool>(); }
        else if (value.is_number_integer()) { node.format = MPV_FORMAT_INT64; node.u.int64 = value.get<std::int64_t>(); }
        else if (value.is_number()) { node.format = MPV_FORMAT_DOUBLE; node.u.double_ = value.get<double>(); }
        else if (value.is_string()) { text = value.get<std::string>(); node.format = MPV_FORMAT_STRING; node.u.string = text.data(); }
        else if (value.is_array() || value.is_object()) {
            node.format = value.is_array() ? MPV_FORMAT_NODE_ARRAY : MPV_FORMAT_NODE_MAP;
            for (auto it = value.begin(); it != value.end(); ++it) {
                children.push_back(std::make_unique<OwnedNode>(it.value()));
                values.push_back(children.back()->node);
                if (value.is_object()) names.push_back(it.key());
            }
            for (auto& name : names) keys.push_back(name.data());
            list.num = static_cast<int>(values.size());
            list.values = values.data(); list.keys = keys.data(); node.u.list = &list;
        }
    }
};

const mpv_node* member(const mpv_node& node, const char* key) {
    if (node.format != MPV_FORMAT_NODE_MAP || !node.u.list) return nullptr;
    for (int i = 0; i < node.u.list->num; ++i) {
        if (node.u.list->keys[i] && std::string_view(node.u.list->keys[i]) == key)
            return &node.u.list->values[i];
    }
    return nullptr;
}

std::string stringValue(const mpv_node* node) {
    return node && node->format == MPV_FORMAT_STRING && node->u.string
        ? node->u.string : "";
}

std::int64_t intValue(const mpv_node* node, std::int64_t fallback = 0) {
    return node && node->format == MPV_FORMAT_INT64 ? node->u.int64 : fallback;
}

double doubleValue(const mpv_node* node) {
    if (!node) return 0;
    if (node->format == MPV_FORMAT_DOUBLE) return node->u.double_;
    if (node->format == MPV_FORMAT_INT64) return static_cast<double>(node->u.int64);
    return 0;
}

bool flagValue(const mpv_node* node) {
    return node && node->format == MPV_FORMAT_FLAG && node->u.flag != 0;
}

template <typename Callback>
void eachArray(const mpv_node& node, Callback&& callback) {
    if (node.format != MPV_FORMAT_NODE_ARRAY || !node.u.list) return;
    for (int i = 0; i < node.u.list->num; ++i) callback(node.u.list->values[i]);
}

std::string utf8Path(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::string number(double value) {
    // std::to_string always uses a decimal point in the default C locale.
    return std::to_string(value);
}

} // namespace

struct MediaEngine::Impl {
#ifdef _WIN32
    HMODULE module = nullptr;
#else
    void* module = nullptr;
#endif
    mpv_handle* handle = nullptr;
    bool initialized = false;
    std::atomic<bool> wakeActive = false;
    EngineWakeup wakeup;  // Written only while no mpv thread can call wake().
    std::atomic<bool> wakePosted = false;
    std::uint64_t requestId = 0;
    std::string lastError;
    PlaybackState playback;
    std::function<void(const std::string&, const std::string&)> eventCallback;
    std::set<std::string> observed;

#define MPV_FUNCTION(name) decltype(&name) name##_fn = nullptr
    MPV_FUNCTION(mpv_client_api_version);
    MPV_FUNCTION(mpv_create);
    MPV_FUNCTION(mpv_initialize);
    MPV_FUNCTION(mpv_terminate_destroy);
    MPV_FUNCTION(mpv_set_option);
    MPV_FUNCTION(mpv_set_option_string);
    MPV_FUNCTION(mpv_command_async);
    MPV_FUNCTION(mpv_set_property_async);
    MPV_FUNCTION(mpv_set_property);
    MPV_FUNCTION(mpv_observe_property);
    MPV_FUNCTION(mpv_set_wakeup_callback);
    MPV_FUNCTION(mpv_wait_event);
    MPV_FUNCTION(mpv_error_string);
    MPV_FUNCTION(mpv_get_property);
    MPV_FUNCTION(mpv_free_node_contents);
    MPV_FUNCTION(mpv_hook_add);
    MPV_FUNCTION(mpv_hook_continue);
    MPV_FUNCTION(mpv_command_ret);
#undef MPV_FUNCTION

    void report(std::string message) {
        lastError = std::move(message);
        playback.error = lastError;
    }

    std::string describe(int result) const {
        const char* message = mpv_error_string_fn ? mpv_error_string_fn(result) : nullptr;
        return message ? message : "Unknown playback error";
    }

    static void wake(void* context) {
        auto& self = *static_cast<Impl*>(context);
        if (!self.wakeActive.load(std::memory_order_acquire)) return;
        if (!self.wakePosted.exchange(true, std::memory_order_acq_rel)) {
            if (!self.wakeup())
                self.wakePosted.store(false, std::memory_order_release);
        }
    }

    void release() {
        wakeActive.store(false, std::memory_order_release);
        if (handle) {
            if (mpv_set_wakeup_callback_fn) mpv_set_wakeup_callback_fn(handle, nullptr, nullptr);
            if (mpv_terminate_destroy_fn) mpv_terminate_destroy_fn(handle);
            handle = nullptr;
        }
        wakeup = nullptr;  // mpv's threads have exited.
        initialized = false;
        playback.loaded = false;
        playback.idle = true;
        playback.seeking = false;
        if (module) {
#ifdef _WIN32
            FreeLibrary(module);
#else
            dlclose(module);
#endif
            module = nullptr;
        }
        wakePosted.store(false, std::memory_order_release);
        observed.clear();
    }

    bool load() {
#if defined(VELIN_MPV_LINKED)
        // libmpv is linked into the app (iOS); no runtime loading.
#define LOAD_MPV(name) name##_fn = &name;
#else
        const auto directory = sys::executableDirectory();
        if (directory.empty()) {
            report("Cannot locate the Velin executable directory.");
            return false;
        }
#ifdef _WIN32
        for (const wchar_t* name : {L"libmpv-2.dll", L"mpv-2.dll"}) {
            const auto path = directory / name;
            module = LoadLibraryExW(path.c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (module) break;
        }
        if (!module) {
            report("Playback engine not found. Place the 64-bit libmpv-2.dll beside velin.exe, "
                   "or run scripts/Get-Mpv.ps1 and rebuild.");
            return false;
        }
#define LOAD_MPV(name) \
        name##_fn = std::bit_cast<decltype(name##_fn)>(GetProcAddress(module, #name)); \
        if (!name##_fn) { report("The playback library is missing " #name ". Install a current libmpv build."); return false; }
#else
        // Beside the executable first, then the system search path (Homebrew).
        for (const char* name : {"libmpv.2.dylib", "libmpv.dylib", "libmpv.so.2"}) {
            const auto path = directory / name;
            module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!module) module = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (module) break;
        }
#ifdef __APPLE__
        for (const char* path : {"/opt/homebrew/lib/libmpv.2.dylib", "/usr/local/lib/libmpv.2.dylib"})
            if (!module) module = dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
        if (!module) {
            report("Playback engine not found. Place libmpv beside the executable or install it (brew install mpv).");
            return false;
        }
#define LOAD_MPV(name) \
        name##_fn = std::bit_cast<decltype(name##_fn)>(dlsym(module, #name)); \
        if (!name##_fn) { report("The playback library is missing " #name ". Install a current libmpv build."); return false; }
#endif
#endif
        LOAD_MPV(mpv_client_api_version)
        LOAD_MPV(mpv_create)
        LOAD_MPV(mpv_initialize)
        LOAD_MPV(mpv_terminate_destroy)
        LOAD_MPV(mpv_set_option)
        LOAD_MPV(mpv_set_option_string)
        LOAD_MPV(mpv_command_async)
        LOAD_MPV(mpv_set_property_async)
        LOAD_MPV(mpv_set_property)
        LOAD_MPV(mpv_observe_property)
        LOAD_MPV(mpv_set_wakeup_callback)
        LOAD_MPV(mpv_wait_event)
        LOAD_MPV(mpv_error_string)
        LOAD_MPV(mpv_get_property)
        LOAD_MPV(mpv_free_node_contents)
        LOAD_MPV(mpv_hook_add)
        LOAD_MPV(mpv_hook_continue)
        LOAD_MPV(mpv_command_ret)
#undef LOAD_MPV
        if ((mpv_client_api_version_fn() >> 16U) != 2) {
            report("Incompatible playback library. Velin requires libmpv client API version 2.");
            return false;
        }
        return true;
    }

    void property(const mpv_event_property& event) {
        if (!event.name) return;
        const std::string_view name = event.name;
        // MPV_FORMAT_NONE reports unavailable properties, e.g. after unloading.
        if (event.format == MPV_FORMAT_NONE || !event.data) {
            if (eventCallback) eventCallback("mpv." + std::string(name) + ".changed", "[null]");
            return;
        }
        if (event.format == MPV_FORMAT_FLAG) {
            const bool value = *static_cast<int*>(event.data) != 0;
            if (name == "pause") playback.paused = value;
            else if (name == "idle-active") {
                playback.idle = value;
                if (value) playback.loaded = false;
            }
            else if (name == "mute") playback.muted = value;
            else if (name == "seeking") playback.seeking = value;
        } else if (event.format == MPV_FORMAT_DOUBLE) {
            const double value = *static_cast<double*>(event.data);
            if (!std::isfinite(value)) return;
            if (name == "time-pos") playback.position = std::max(0.0, value);
            else if (name == "duration") playback.duration = std::max(0.0, value);
            else if (name == "volume") playback.volume = value;
            else if (name == "speed") playback.speed = value;
            else if (name == "estimated-vf-fps") playback.videoFps = value;
            else if (name == "display-fps") playback.displayFps = value;
            else if (name == "avsync") playback.avSync = value;
            else if (name == "audio-delay") playback.audioDelay = value;
            else if (name == "sub-delay") playback.subtitleDelay = value;
            else if (name == "sub-scale") playback.subtitleScale = value;
            else if (name == "sub-pos") playback.subtitlePosition = value;
            else if (name == "video-bitrate") playback.videoBitrate = value;
            else if (name == "audio-bitrate") playback.audioBitrate = value;
        } else if (event.format == MPV_FORMAT_INT64) {
            const auto value = *static_cast<std::int64_t*>(event.data);
            if (name == "video-params/w") playback.width = value;
            else if (name == "video-params/h") playback.height = value;
            else if (name == "playlist-pos") playback.playlistPosition = value;
            else if (name == "frame-drop-count") playback.droppedFrames = value;
            else if (name == "decoder-frame-drop-count") playback.decoderDroppedFrames = value;
            else if (name == "secondary-sid") playback.secondarySubtitle = value;
        } else if (event.format == MPV_FORMAT_STRING) {
            const auto* ptr = static_cast<char**>(event.data);
            const std::string value = *ptr ? *ptr : "";
            if (name == "media-title") playback.title = value;
            else if (name == "path") playback.path = value;
            else if (name == "video-codec") playback.videoCodec = value;
            else if (name == "audio-codec-name") playback.audioCodec = value;
            else if (name == "hwdec-current") playback.hardwareDecoder = value;
            else if (name == "video-params/pixelformat") playback.pixelFormat = value;
            else if (name == "video-params/primaries") playback.colorPrimaries = value;
            else if (name == "video-params/gamma") playback.colorTransfer = value;
            else if (name == "audio-device") playback.audioDevice = value;
        } else if (event.format == MPV_FORMAT_NODE) {
            const auto& node = *static_cast<mpv_node*>(event.data);
            if (name == "track-list") {
                playback.tracks.clear();
                eachArray(node, [this](const mpv_node& item) {
                    Track track;
                    track.id = intValue(member(item, "id"));
                    track.type = stringValue(member(item, "type"));
                    track.title = stringValue(member(item, "title"));
                    track.language = stringValue(member(item, "lang"));
                    track.codec = stringValue(member(item, "codec"));
                    track.selected = flagValue(member(item, "selected"));
                    track.external = flagValue(member(item, "external"));
                    track.albumArt = flagValue(member(item, "albumart"));
                    playback.tracks.push_back(std::move(track));
                });
            } else if (name == "chapter-list") {
                playback.chapters.clear();
                eachArray(node, [this](const mpv_node& item) {
                    playback.chapters.push_back({stringValue(member(item, "title")),
                                                doubleValue(member(item, "time"))});
                });
            } else if (name == "playlist") {
                playback.playlist.clear();
                eachArray(node, [this](const mpv_node& item) {
                    playback.playlist.push_back({stringValue(member(item, "filename")),
                        stringValue(member(item, "title")), flagValue(member(item, "current"))});
                });
            } else if (name == "audio-device-list") {
                playback.audioDevices.clear();
                eachArray(node, [this](const mpv_node& item) {
                    playback.audioDevices.push_back({stringValue(member(item, "name")), stringValue(member(item, "description"))});
                });
            } else if (name == "vf") playback.videoFiltersJson = nodeJson(node).dump();
            else if (name == "af") playback.audioFiltersJson = nodeJson(node).dump();
            else if (name == "metadata") {
                playback.metadata.clear();
                const auto data = nodeJson(node);
                if (data.is_object()) for (auto it = data.begin(); it != data.end(); ++it)
                    playback.metadata[it.key()] = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
            }
        }
        if (eventCallback) {
            Json value;
            if (event.format == MPV_FORMAT_NODE) value = nodeJson(*static_cast<mpv_node*>(event.data));
            else if (event.format == MPV_FORMAT_STRING) value = *static_cast<char**>(event.data);
            else if (event.format == MPV_FORMAT_FLAG) value = *static_cast<int*>(event.data) != 0;
            else if (event.format == MPV_FORMAT_DOUBLE) value = *static_cast<double*>(event.data);
            else if (event.format == MPV_FORMAT_INT64) value = *static_cast<std::int64_t*>(event.data);
            eventCallback("mpv." + std::string(name) + ".changed", Json::array({value}).dump());
            }
    }
};

MediaEngine::MediaEngine() : impl_(std::make_unique<Impl>()) {}
MediaEngine::~MediaEngine() { shutdown(); }

#ifdef _WIN32
bool MediaEngine::initialize(HWND videoWindow, HWND notifyWindow, UINT notifyMessage) {
    return initialize(videoWindow, notifyWindow, notifyMessage, {});
}

bool MediaEngine::initialize(HWND videoWindow, HWND notifyWindow, UINT notifyMessage, const EngineConfiguration& configuration) {
    if (!videoWindow || !notifyWindow || !notifyMessage) {
        shutdown();
        impl_->lastError.clear();
        impl_->playback = {};
        impl_->report("A video window and notification window are required.");
        return false;
    }
    return initialize(VideoOutput{reinterpret_cast<std::intptr_t>(videoWindow)},
        [notifyWindow, notifyMessage] { return PostMessageW(notifyWindow, notifyMessage, 0, 0) != FALSE; }, configuration);
}
#endif

bool MediaEngine::initialize(const VideoOutput& video, EngineWakeup wakeup, const EngineConfiguration& configuration) {
    shutdown();
    impl_->lastError.clear();
    impl_->playback = {};
    if (!wakeup) {
        impl_->report("An engine wakeup callback is required.");
        return false;
    }
    if (!impl_->load()) {
        impl_->release();
        return false;
    }
    impl_->handle = impl_->mpv_create_fn();
    if (!impl_->handle) {
        impl_->report("The playback engine could not be created.");
        impl_->release();
        return false;
    }
    const std::pair<const char*, const char*> options[] = {
        {"config", "no"}, {"load-scripts", "no"}, {"terminal", "no"}, {"input-default-bindings", "no"},
        {"input-vo-keyboard", "no"}, {"input-cursor", "no"}, {"osc", "no"},
        {"osd-level", "0"}, {"idle", "yes"}, {"keep-open", "yes"},
        {"vo", "gpu-next,gpu"}, {"hwdec", "auto"}, {"volume", "70"},
        {"sub-auto", "fuzzy"}, {"audio-file-auto", "fuzzy"},
        {"ytdl", "no"}, {"force-window", "no"}, {"keepaspect-window", "no"},
        {"save-position-on-quit", "yes"}, {"resume-playback", "yes"}
    };
    for (const auto& [name, value] : options) {
        const int result = impl_->mpv_set_option_string_fn(impl_->handle, name, value);
        if (result < 0) {
            impl_->report(std::string("Cannot configure playback option '") + name + "': " + impl_->describe(result));
            impl_->release();
            return false;
        }
    }
    const std::pair<const char*, std::string> userDefaults[] = {
        {"config", configuration.loadUserConfig ? "yes" : "no"},
        {"load-scripts", configuration.loadScripts ? "yes" : "no"},
        {"ytdl", configuration.onlineMedia ? "yes" : "no"}
    };
    for (const auto& [name, value] : userDefaults)
        impl_->mpv_set_option_string_fn(impl_->handle, name, value.c_str());
    const auto data = sys::dataDirectory();
    if (!data.empty()) {
        std::error_code filesystemError;
        const auto config = configuration.configDirectory.empty() ? data / L"mpv" : configuration.configDirectory;
        const auto watchLater = config / L"watch_later";
        const auto screenshots = data / L"Screenshots";
        std::filesystem::create_directories(watchLater, filesystemError);
        if (!filesystemError) std::filesystem::create_directories(screenshots, filesystemError);
        if (filesystemError) {
            impl_->report("Cannot create the playback data directory: " + filesystemError.message());
            impl_->release();
            return false;
        }
        const std::pair<const char*, std::string> paths[] = {
            {"config-dir", utf8Path(config)}, {"watch-later-directory", utf8Path(watchLater)},
            {"screenshot-directory", utf8Path(screenshots)}
        };
        for (const auto& [name, value] : paths) {
            const int optionResult = impl_->mpv_set_option_string_fn(impl_->handle, name, value.c_str());
            if (optionResult < 0) {
                impl_->report(std::string("Cannot configure '") + name + "': " + impl_->describe(optionResult));
                impl_->release();
                return false;
            }
        }
    }
    auto configuredOptions = configuration.options;
    if (!configuration.inputConfiguration.empty()) configuredOptions["input-conf"] = utf8Path(configuration.inputConfiguration);
    if (!configuration.ytDlpExecutable.empty()) {
        auto& options=configuredOptions["script-opts"];
        if (!options.empty()) options+=',';
        options+="ytdl_hook-ytdl_path="+utf8Path(configuration.ytDlpExecutable);
    }
    for (const auto& [name, value] : configuredOptions) {
        const int optionResult = impl_->mpv_set_option_string_fn(impl_->handle, name.c_str(), value.c_str());
        if (optionResult < 0) {
            impl_->report("Invalid custom mpv option '" + name + "': " + impl_->describe(optionResult));
            impl_->release(); return false;
        }
    }
    int result = 0;
    if (video.nativeHandle) {
#ifdef _WIN32
        // Windows USER handles are 32-bit values even in a 64-bit process. Avoid
        // sign extension when passing the integer HWND representation to libmpv.
        std::int64_t windowId = static_cast<std::uint32_t>(static_cast<std::uintptr_t>(video.nativeHandle));
#else
        std::int64_t windowId = static_cast<std::int64_t>(video.nativeHandle);
#endif
        result = impl_->mpv_set_option_fn(impl_->handle, "wid", MPV_FORMAT_INT64, &windowId);
    } else {
        result = impl_->mpv_set_option_string_fn(impl_->handle, "vo", "null");
    }
    if (result >= 0) result = impl_->mpv_initialize_fn(impl_->handle);
    if (result < 0) {
        impl_->report("The playback engine could not initialize: " + impl_->describe(result));
        impl_->release();
        return false;
    }
    impl_->initialized = true;
    const std::pair<const char*, mpv_format> observations[] = {
        {"pause", MPV_FORMAT_FLAG}, {"idle-active", MPV_FORMAT_FLAG},
        {"mute", MPV_FORMAT_FLAG}, {"seeking", MPV_FORMAT_FLAG},
        {"time-pos", MPV_FORMAT_DOUBLE}, {"duration", MPV_FORMAT_DOUBLE},
        {"volume", MPV_FORMAT_DOUBLE}, {"speed", MPV_FORMAT_DOUBLE},
        {"media-title", MPV_FORMAT_STRING}, {"path", MPV_FORMAT_STRING},
        {"video-codec", MPV_FORMAT_STRING}, {"audio-codec-name", MPV_FORMAT_STRING},
        {"video-params/w", MPV_FORMAT_INT64}, {"video-params/h", MPV_FORMAT_INT64},
        {"playlist-pos", MPV_FORMAT_INT64}, {"track-list", MPV_FORMAT_NODE},
        {"chapter-list", MPV_FORMAT_NODE}, {"playlist", MPV_FORMAT_NODE}
        , {"hwdec-current", MPV_FORMAT_STRING}, {"video-params/pixelformat", MPV_FORMAT_STRING}
        , {"video-params/primaries", MPV_FORMAT_STRING}, {"video-params/gamma", MPV_FORMAT_STRING}
        , {"audio-device", MPV_FORMAT_STRING}, {"audio-device-list", MPV_FORMAT_NODE}
        , {"vf", MPV_FORMAT_NODE}, {"af", MPV_FORMAT_NODE}, {"estimated-vf-fps", MPV_FORMAT_DOUBLE}
        , {"display-fps", MPV_FORMAT_DOUBLE}, {"avsync", MPV_FORMAT_DOUBLE}, {"audio-delay", MPV_FORMAT_DOUBLE}
        , {"sub-delay", MPV_FORMAT_DOUBLE}, {"sub-scale", MPV_FORMAT_DOUBLE}, {"sub-pos", MPV_FORMAT_DOUBLE}
        , {"video-bitrate", MPV_FORMAT_DOUBLE}, {"audio-bitrate", MPV_FORMAT_DOUBLE}
        , {"frame-drop-count", MPV_FORMAT_INT64}, {"decoder-frame-drop-count", MPV_FORMAT_INT64}
        , {"secondary-sid", MPV_FORMAT_INT64}
        , {"metadata", MPV_FORMAT_NODE}
    };
    std::uint64_t id = 1;
    for (const auto& [name, format] : observations) {
        impl_->mpv_observe_property_fn(impl_->handle, id++, name, format);
        impl_->observed.insert(name);
    }
    impl_->wakeup = std::move(wakeup);
    impl_->wakeActive.store(true, std::memory_order_release);
    impl_->mpv_set_wakeup_callback_fn(impl_->handle, &Impl::wake, impl_.get());
    return true;
}

void MediaEngine::shutdown() { impl_->release(); }

void MediaEngine::poll() {
    if (!ready()) return;
    impl_->wakePosted.store(false, std::memory_order_release);
    // Bound work per window message, while guaranteeing that a busy decoder
    // eventually gets its remaining events drained in another UI turn.
    for (int count = 0; count < 512; ++count) {
        const mpv_event* event = impl_->mpv_wait_event_fn(impl_->handle, 0);
        if (!event || event->event_id == MPV_EVENT_NONE) return;
        switch (event->event_id) {
        case MPV_EVENT_PROPERTY_CHANGE:
            if (event->data) impl_->property(*static_cast<mpv_event_property*>(event->data));
            break;
        case MPV_EVENT_START_FILE:
            impl_->playback.idle = false;
            impl_->playback.loaded = false;
            impl_->playback.position = 0;
            impl_->playback.duration = 0;
            impl_->playback.width = 0;
            impl_->playback.height = 0;
            impl_->playback.title.clear();
            impl_->playback.videoCodec.clear();
            impl_->playback.audioCodec.clear();
            impl_->playback.tracks.clear();
            impl_->playback.chapters.clear();
            impl_->lastError.clear();
            impl_->playback.error.clear();
            break;
        case MPV_EVENT_FILE_LOADED:
            impl_->playback.loaded = true;
            break;
        case MPV_EVENT_END_FILE:
            if (event->data) {
                const auto& end = *static_cast<mpv_event_end_file*>(event->data);
                if (end.reason == MPV_END_FILE_REASON_ERROR)
                    impl_->report("Cannot play this media: " + impl_->describe(end.error));
                if (end.reason == MPV_END_FILE_REASON_ERROR || end.reason == MPV_END_FILE_REASON_STOP)
                    impl_->playback.idle = true;
                if (end.reason != MPV_END_FILE_REASON_EOF)
                    impl_->playback.loaded = false;
            }
            impl_->playback.seeking = false;
            break;
        case MPV_EVENT_COMMAND_REPLY:
        case MPV_EVENT_SET_PROPERTY_REPLY:
            if (event->error < 0) impl_->report("Playback command failed: " + impl_->describe(event->error));
            break;
        case MPV_EVENT_SHUTDOWN:
            impl_->playback.idle = true;
            impl_->playback.loaded = false;
            impl_->report("The playback engine stopped. Reopen Velin to restart it.");
            break;
        case MPV_EVENT_QUEUE_OVERFLOW:
            impl_->report("The playback event queue overflowed. Some playback information may be delayed.");
            break;
        case MPV_EVENT_HOOK:
            if (event->data) {
                const auto& hook = *static_cast<mpv_event_hook*>(event->data);
                if (impl_->eventCallback) impl_->eventCallback("mpv.hook", Json::array({event->reply_userdata, hook.id, hook.name ? hook.name : ""}).dump());
                else impl_->mpv_hook_continue_fn(impl_->handle, hook.id);
            }
            break;
        default:
            break;
        }
        if (impl_->eventCallback && event->event_id != MPV_EVENT_PROPERTY_CHANGE) {
            const char* name = nullptr;
            switch (event->event_id) {
            case MPV_EVENT_START_FILE: name = "mpv.start-file"; break;
            case MPV_EVENT_FILE_LOADED: name = "mpv.file-loaded"; break;
            case MPV_EVENT_END_FILE: name = "mpv.end-file"; break;
            case MPV_EVENT_SEEK: name = "mpv.seek"; break;
            case MPV_EVENT_PLAYBACK_RESTART: name = "mpv.playback-restart"; break;
            default: break;
            }
            if (name) impl_->eventCallback(name, "[]");
        }
    }
    Impl::wake(impl_.get());
}

bool MediaEngine::ready() const { return impl_->initialized && impl_->handle; }
const std::string& MediaEngine::error() const { return impl_->lastError; }
const PlaybackState& MediaEngine::state() const { return impl_->playback; }

bool MediaEngine::command(std::initializer_list<std::string> arguments) {
    return command(std::vector<std::string>(arguments));
}

bool MediaEngine::command(const std::vector<std::string>& arguments) {
    if (!ready() || arguments.size() == 0) return false;
    std::vector<const char*> pointers;
    pointers.reserve(arguments.size() + 1);
    for (const auto& argument : arguments) {
        if (argument.find('\0') != std::string::npos) {
            impl_->report("A playback command contained an invalid null character.");
            return false;
        }
        pointers.push_back(argument.c_str());
    }
    pointers.push_back(nullptr);
    const int result = impl_->mpv_command_async_fn(impl_->handle, ++impl_->requestId, pointers.data());
    if (result < 0) impl_->report("Cannot queue playback command: " + impl_->describe(result));
    return result >= 0;
}

bool MediaEngine::setPropertyJson(std::string_view name, std::string_view json) {
    if (!ready() || name.empty() || name.find('\0') != std::string_view::npos) return false;
    const auto value = Json::parse(json, nullptr, false);
    if (value.is_discarded()) return false;
    OwnedNode node(value);
    const std::string property(name);
    const int result = impl_->mpv_set_property_async_fn(impl_->handle, ++impl_->requestId,
        property.c_str(), MPV_FORMAT_NODE, &node.node);
    if (result < 0) impl_->report("Cannot set native playback property: " + impl_->describe(result));
    return result >= 0;
}

bool MediaEngine::setPropertyJsonChecked(std::string_view name, std::string_view json, std::string& error) {
    error.clear();
    if (!ready() || name.empty() || name.find('\0') != std::string_view::npos) {
        error = "The playback engine or property is unavailable."; return false;
    }
    const auto value = Json::parse(json, nullptr, false);
    if (value.is_discarded()) { error = "Invalid JSON."; return false; }
    if (name == "vf" || name == "af") {
        if (!value.is_array()) { error = "A filter chain must be an array."; return false; }
        std::string chain;
        const auto identifier=[](const std::string& text) {
            return !text.empty() && text.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-{}") == std::string::npos;
        };
        for (const auto& filter:value) {
            if (!filter.is_object() || !filter.contains("name") || !filter["name"].is_string() ||
                !identifier(filter["name"].get<std::string>())) { error="Invalid filter name."; return false; }
            if (filter.contains("enabled") && (!filter["enabled"].is_boolean() || !filter["enabled"].get<bool>())) {
                error="Remove disabled filters before editing this chain."; return false;
            }
            if (!chain.empty()) chain+=',';
            if (filter.contains("label")) {
                if (!filter["label"].is_string() || !identifier(filter["label"].get<std::string>())) { error="Invalid filter label."; return false; }
                chain+='@'+filter["label"].get<std::string>()+':';
            }
            chain+=filter["name"].get<std::string>();
            if (filter.contains("params")) {
                if (!filter["params"].is_object()) { error="Filter parameters must be an object."; return false; }
                bool first=true;
                for (const auto& [key,parameter]:filter["params"].items()) {
                    if (!identifier(key) || !parameter.is_string()) { error="Filter parameters must have named string values."; return false; }
                    const auto text=parameter.get<std::string>();
                    if (text.find('\0')!=std::string::npos) { error="Invalid filter parameter."; return false; }
                    chain+=first?'=':':'; first=false;
                    chain+=key+"=%"+std::to_string(text.size())+"%"+text;
                }
            }
        }
        const std::string property(name);
        const char* arguments[]={property.c_str(),"set",chain.c_str(),nullptr};
        mpv_node result{};
        const int status=impl_->mpv_command_ret_fn(impl_->handle,arguments,&result);
        impl_->mpv_free_node_contents_fn(&result);
        if (status<0) { error=impl_->describe(status); return false; }
        return true;
    }
    OwnedNode node(value);
    const std::string property(name);
    const int result = impl_->mpv_set_property_fn(impl_->handle, property.c_str(), MPV_FORMAT_NODE, &node.node);
    if (result < 0) { error = impl_->describe(result); return false; }
    return true;
}

std::string MediaEngine::propertyJson(std::string_view name) const {
    if (!ready() || name.empty() || name.find('\0') != std::string_view::npos) return "null";
    const std::string property(name);
    mpv_node node{};
    if (impl_->mpv_get_property_fn(impl_->handle, property.c_str(), MPV_FORMAT_NODE, &node) < 0) return "null";
    const auto result = nodeJson(node).dump();
    impl_->mpv_free_node_contents_fn(&node);
    return result;
}

bool MediaEngine::observe(std::string_view name) {
    if (!ready() || name.empty() || name.find('\0') != std::string_view::npos) return false;
    const std::string property(name);
    if (impl_->observed.contains(property)) return true;
    const int result = impl_->mpv_observe_property_fn(impl_->handle, 1000 + impl_->observed.size(), property.c_str(), MPV_FORMAT_NODE);
    if (result >= 0) impl_->observed.insert(property);
    return result >= 0;
}
void MediaEngine::setEventCallback(std::function<void(const std::string&, const std::string&)> callback) {
    impl_->eventCallback = std::move(callback);
}
bool MediaEngine::addHook(std::string_view name, int priority, std::uint64_t userdata) {
    if (!ready() || name.empty() || name.find('\0') != std::string_view::npos) return false;
    const std::string hook(name);
    return impl_->mpv_hook_add_fn(impl_->handle, userdata, hook.c_str(), priority) >= 0;
}
void MediaEngine::continueHook(std::uint64_t id) { if (ready()) impl_->mpv_hook_continue_fn(impl_->handle, id); }

std::optional<CapturedFrame> MediaEngine::captureFrame(bool subtitles) {
    if (!ready() || !impl_->playback.loaded) return std::nullopt;
    const char* args[] = {"screenshot-raw", subtitles ? "subtitles" : "video", nullptr};
    mpv_node result{};
    if (impl_->mpv_command_ret_fn(impl_->handle, args, &result) < 0) return std::nullopt;
    struct Cleanup { Impl* impl; mpv_node* node; ~Cleanup() { impl->mpv_free_node_contents_fn(node); } } cleanup{impl_.get(), &result};
    const auto width = intValue(member(result, "w"));
    const auto height = intValue(member(result, "h"));
    const auto stride = intValue(member(result, "stride"));
    const auto format = stringValue(member(result, "format"));
    const auto* data = member(result, "data");
    if (width <= 0 || height <= 0 || width > 65536 || height > 65536 || stride < width * 4
        || !data || data->format != MPV_FORMAT_BYTE_ARRAY || !data->u.ba
        || static_cast<std::uint64_t>(stride) * static_cast<std::uint64_t>(height) > data->u.ba->size) return std::nullopt;
    if (format != "bgr0" && format != "bgra" && format != "rgb0" && format != "rgba") return std::nullopt;
    CapturedFrame frame;
    frame.width = static_cast<unsigned>(width); frame.height = static_cast<unsigned>(height); frame.stride = static_cast<unsigned>(stride);
    const auto* begin = static_cast<const std::uint8_t*>(data->u.ba->data);
    frame.bgra.assign(begin, begin + static_cast<std::size_t>(stride * height));
    for (unsigned y = 0; y < frame.height; ++y) for (unsigned x = 0; x < frame.width; ++x) {
        auto* pixel = frame.bgra.data() + static_cast<std::size_t>(y) * frame.stride + static_cast<std::size_t>(x) * 4;
        if (format == "rgb0" || format == "rgba") std::swap(pixel[0], pixel[2]);
        if (format == "rgb0" || format == "bgr0") pixel[3] = 255;
    }
    return frame;
}

bool MediaEngine::setProperty(std::string_view name, std::string_view value) {
    if (!ready()) return false;
    if (name.empty() || name.find('\0') != std::string_view::npos || value.find('\0') != std::string_view::npos)
        return false;
    const std::string property(name);
    std::string text(value);
    char* ptr = text.data();
    const int result = impl_->mpv_set_property_async_fn(impl_->handle, ++impl_->requestId,
        property.c_str(), MPV_FORMAT_STRING, &ptr);
    if (result < 0) impl_->report("Cannot set playback property: " + impl_->describe(result));
    return result >= 0;
}

bool MediaEngine::open(const std::vector<std::string>& paths, bool append) {
    if (!ready() || paths.empty()) return false;
    impl_->lastError.clear();
    impl_->playback.error.clear();
    bool accepted = true;
    bool first = true;
    for (const auto& path : paths) {
        if (path.empty()) continue;
        const char* mode = first && !append ? "replace" : "append-play";
        const bool queued = command({"loadfile", path, mode});
        // Invalidate the previous file immediately: START_FILE arrives later
        // and callers may dispatch another UI action before that event.
        if (queued && first && !append) impl_->playback.loaded = false;
        accepted = queued && accepted;
        first = false;
    }
    if (!append && !first) setProperty("pause", "no");
    return !first && accepted;
}

void MediaEngine::togglePause() { command({"cycle", "pause"}); }
void MediaEngine::seek(double seconds, bool absolute) {
    if (!std::isfinite(seconds)) return;
    command({"seek", number(absolute ? std::max(0.0, seconds) : seconds),
             absolute ? "absolute+exact" : "relative+exact"});
}
void MediaEngine::setVolume(double volume) {
    if (std::isfinite(volume)) setProperty("volume", number(std::clamp(volume, 0.0, 100.0)));
}
void MediaEngine::setSpeed(double speed) {
    if (std::isfinite(speed)) setProperty("speed", number(std::clamp(speed, 0.25, 4.0)));
}
void MediaEngine::toggleMute() { command({"cycle", "mute"}); }
void MediaEngine::stop() { command({"stop"}); }
void MediaEngine::selectPlaylist(std::int64_t position) {
    if (position >= 0 && position < static_cast<std::int64_t>(impl_->playback.playlist.size()))
        setProperty("playlist-pos", std::to_string(position));
}
void MediaEngine::addSubtitle(const std::string& path) {
    if (!path.empty()) command({"sub-add", path, "select"});
}
void MediaEngine::addAudio(const std::string& path) { if (!path.empty()) command({"audio-add", path, "select"}); }
void MediaEngine::selectSecondarySubtitle(std::int64_t id) { setProperty("secondary-sid", id < 0 ? "no" : std::to_string(id)); }
void MediaEngine::movePlaylist(std::int64_t from, std::int64_t to) {
    if (from >= 0 && to >= 0) command({"playlist-move", std::to_string(from), std::to_string(to)});
}
void MediaEngine::shufflePlaylist(bool shuffle) { command({shuffle ? "playlist-shuffle" : "playlist-unshuffle"}); }
void MediaEngine::savePosition() { command({"write-watch-later-config"}); }

} // namespace velin
