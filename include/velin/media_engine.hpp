// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <initializer_list>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace velin {

struct Track {
    std::int64_t id = 0;
    std::string type;
    std::string title;
    std::string language;
    std::string codec;
    bool selected = false;
    bool external = false;
    bool albumArt = false;
};

struct Chapter {
    std::string title;
    double time = 0;
};

struct PlaylistItem {
    std::string filename;
    std::string title;
    bool current = false;
};

struct EngineConfiguration {
    std::map<std::string, std::string> options;
    std::filesystem::path configDirectory;
    std::filesystem::path inputConfiguration;
    std::filesystem::path ytDlpExecutable;
    bool loadUserConfig = false;
    bool loadScripts = false;
    bool onlineMedia = false;
};

struct AudioDevice {
    std::string name, description;
};
struct CapturedFrame {
    unsigned width = 0, height = 0, stride = 0;
    std::vector<std::uint8_t> bgra;
};

struct PlaybackState {
    bool idle = true;
    bool loaded = false;
    bool paused = false;
    bool muted = false;
    bool seeking = false;
    double position = 0;
    double duration = 0;
    double volume = 70;
    double speed = 1;
    std::string title;
    std::string path;
    std::string error;
    std::string videoCodec;
    std::string audioCodec;
    std::int64_t width = 0;
    std::int64_t height = 0;
    std::vector<Track> tracks;
    std::vector<Chapter> chapters;
    std::vector<PlaylistItem> playlist;
    std::int64_t playlistPosition = -1;
    std::string hardwareDecoder, pixelFormat, colorPrimaries, colorTransfer, audioDevice;
    double videoFps = 0, displayFps = 0, avSync = 0, audioDelay = 0, subtitleDelay = 0;
    double subtitleScale = 1, subtitlePosition = 100, videoBitrate = 0, audioBitrate = 0;
    std::int64_t droppedFrames = 0, decoderDroppedFrames = 0, secondarySubtitle = 0;
    std::vector<AudioDevice> audioDevices;
    std::string videoFiltersJson = "[]", audioFiltersJson = "[]";
    std::map<std::string, std::string> metadata;
};

// All methods except the internal libmpv wakeup callback run on the UI thread.
// The supplied notification window must outlive shutdown(). Its message handler
// calls poll(), then reads state(). No libmpv types escape this boundary.
class MediaEngine final {
public:
    MediaEngine();
    ~MediaEngine();
    MediaEngine(const MediaEngine&) = delete;
    MediaEngine& operator=(const MediaEngine&) = delete;
    MediaEngine(MediaEngine&&) = delete;
    MediaEngine& operator=(MediaEngine&&) = delete;

    bool initialize(HWND videoWindow, HWND notifyWindow, UINT notifyMessage);
    bool initialize(HWND videoWindow, HWND notifyWindow, UINT notifyMessage, const EngineConfiguration& configuration);
    void shutdown();
    void poll();
    [[nodiscard]] bool ready() const;
    [[nodiscard]] const std::string& error() const;
    [[nodiscard]] const PlaybackState& state() const;

    bool command(std::initializer_list<std::string> arguments);
    bool command(const std::vector<std::string>& arguments);
    bool setProperty(std::string_view name, std::string_view value);
    bool setPropertyJson(std::string_view name, std::string_view json);
    // Synchronous node write for dialogs that must report mpv validation errors.
    bool setPropertyJsonChecked(std::string_view name, std::string_view json, std::string& error);
    // Generic libmpv node access. Called on the UI thread, like other methods.
    std::string propertyJson(std::string_view name) const;
    bool observe(std::string_view name);
    void setEventCallback(std::function<void(const std::string&, const std::string&)> callback);
    bool addHook(std::string_view name, int priority, std::uint64_t userdata);
    void continueHook(std::uint64_t id);
    std::optional<CapturedFrame> captureFrame(bool subtitles = true);
    bool open(const std::vector<std::string>& paths, bool append = false);
    void togglePause();
    void seek(double seconds, bool absolute = false);
    void setVolume(double volume);
    void setSpeed(double speed);
    void toggleMute();
    void stop();
    void selectPlaylist(std::int64_t position);
    void addSubtitle(const std::string& path);
    void addAudio(const std::string& path);
    void selectSecondarySubtitle(std::int64_t id);
    void movePlaylist(std::int64_t from, std::int64_t to);
    void shufflePlaylist(bool shuffle = true);
    void savePosition();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace velin
