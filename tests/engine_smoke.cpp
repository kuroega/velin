// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
// Real libmpv integration tests. Generate fixtures with scripts/Test-Playback.ps1.

#include "velin/media_engine.hpp"
#include <shellapi.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace std::chrono_literals;

std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct TestWindow {
    HWND parent = CreateWindowExW(0, L"STATIC", L"Velin playback integration test",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        660, 420, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND video = parent ? CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 640, 360, parent, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;

    ~TestWindow() { if (parent) DestroyWindow(parent); }
};

void pump(velin::MediaEngine& engine) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    engine.poll();
}

void settle(velin::MediaEngine& engine, std::chrono::milliseconds delay = 120ms) {
    const auto deadline = std::chrono::steady_clock::now() + delay;
    do {
        pump(engine);
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    pump(engine);
}

void waitFor(velin::MediaEngine& engine, const std::string& operation,
             const std::function<bool(const velin::PlaybackState&)>& predicate,
             std::chrono::milliseconds timeout = 8s, bool expectError = false) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        pump(engine);
        if (!expectError && !engine.state().error.empty())
            throw std::runtime_error(operation + ": " + engine.state().error);
        if (predicate(engine.state())) return;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    const auto& state = engine.state();
    throw std::runtime_error(operation + " timed out (position=" + std::to_string(state.position)
        + ", duration=" + std::to_string(state.duration) + ", playlist="
        + std::to_string(state.playlist.size()) + ", error=" + state.error + ")");
}

bool hasTrack(const velin::PlaybackState& state, const std::string& type, bool external = false,
              bool selected = false) {
    for (const auto& track : state.tracks)
        if (track.type == type && (!external || track.external) && (!selected || track.selected)) return true;
    return false;
}

bool loaded(const velin::PlaybackState& state, const std::string& path) {
    return state.loaded && !state.idle && state.path == path && state.duration > 2.5
        && state.width == 320 && state.height == 180
        && hasTrack(state, "video") && hasTrack(state, "audio");
}

void configure(velin::MediaEngine& engine, bool headless) {
    // Null audio still decodes samples and keeps the integration test silent.
    require(engine.setProperty("options/ao", "null"), "Cannot configure silent audio output");
    if (headless) require(engine.setProperty("options/vo", "null"), "Cannot configure headless video output");
    require(engine.setProperty("options/resume-playback", "no"), "Cannot disable resume in tests");
    require(engine.setProperty("options/save-position-on-quit", "no"), "Cannot disable watch-later writes in tests");
    settle(engine);
    require(engine.state().error.empty(), "Test configuration failed: " + engine.state().error);
}

void run(const std::filesystem::path& directory, bool headless) {
    const auto firstPath = std::filesystem::absolute(directory / std::filesystem::path(u8"Velin 測試 — chaptered.mkv"));
    const auto secondPath = std::filesystem::absolute(directory / "second.mp4");
    const auto subtitlePath = std::filesystem::absolute(directory / "external.srt");
    for (const auto& path : {firstPath, secondPath, subtitlePath})
        require(std::filesystem::is_regular_file(path), "Missing playback fixture: " + utf8(path));
    const std::string first = utf8(firstPath);
    const std::string second = utf8(secondPath);

    TestWindow window;
    require(window.parent && window.video, "Cannot create native test windows");
    // Engine is destroyed before window, including when a test throws.
    velin::MediaEngine engine;
    velin::EngineConfiguration configuration;
    configuration.ytDlpExecutable=directory / "yt-dlp.exe";
    configuration.options["script-opts"]="ytdl_hook-try_ytdl_first=yes";
    require(engine.initialize(window.video, window.parent, WM_APP + 71,configuration), "Engine initialization: " + engine.error());
    require(engine.ready(), "Engine must report ready after successful initialization");
    require(!engine.state().loaded, "Engine must not report loaded before opening media");
    configure(engine, headless);
    std::cout << "PASS runtime initialization and event integration\n";

    require(engine.open({first}), "Cannot enqueue Unicode fixture");
    waitFor(engine, "Load Unicode MKV", [&](const auto& state) { return loaded(state, first); });
    engine.setProperty("pause", "yes");
    waitFor(engine, "Pause after load", [](const auto& state) { return state.paused; });
    waitFor(engine, "Read chapter metadata", [](const auto& state) {
        return state.chapters.size() == 2 && state.chapters[0].title == "Opening"
            && state.chapters[1].title == "Second act" && std::abs(state.chapters[1].time - 2.0) < 0.05;
    });
    require(!engine.state().title.empty(), "Media title must be reported");
    require(!engine.state().videoCodec.empty() && !engine.state().audioCodec.empty(), "Codec metadata must be reported");
    std::cout << "PASS Unicode media, decoded video/audio, metadata, and chapters\n";

    std::string filterError;
    const std::string labeledFilter=R"([{"name":"hflip","label":"velin-preset-{00000000-0000-0000-0000-000000000001}"}])";
    require(engine.setPropertyJsonChecked("vf",labeledFilter,filterError), "Apply labeled video filter: "+filterError);
    require(engine.propertyJson("vf").find("velin-preset-{00000000-0000-0000-0000-000000000001}")!=std::string::npos,
        "Video filter label must appear in the active chain");
    require(!engine.setPropertyJsonChecked("vf",R"([{"name":"velin-filter-does-not-exist"}])",filterError) && !filterError.empty(),
        "An invalid video filter must return an mpv error");
    require(engine.propertyJson("vf").find("velin-preset-{00000000-0000-0000-0000-000000000001}")!=std::string::npos,
        "A rejected filter must leave the previous chain intact");
    require(engine.setPropertyJsonChecked("vf","[]",filterError), "Clear video filters: "+filterError);
    require(engine.setPropertyJsonChecked("af",R"([{"name":"volume","params":{"volume":"0.8"}}])",filterError), "Apply audio filter: "+filterError);
    require(engine.propertyJson("af").find("volume")!=std::string::npos, "Audio filter must appear in the active chain");
    require(engine.setPropertyJsonChecked("af","[]",filterError), "Clear audio filters: "+filterError);
    std::cout << "PASS filter chains, preset labels, rejection and error feedback\n";

    engine.seek(1.25, true);
    waitFor(engine, "Exact seek", [](const auto& state) {
        return !state.seeking && std::abs(state.position - 1.25) < 0.15;
    });
    engine.setVolume(37);
    engine.setSpeed(1.5);
    waitFor(engine, "Volume and speed", [](const auto& state) {
        return std::abs(state.volume - 37) < 0.01 && std::abs(state.speed - 1.5) < 0.01;
    });
    engine.toggleMute();
    waitFor(engine, "Mute", [](const auto& state) { return state.muted; });
    engine.toggleMute();
    waitFor(engine, "Unmute", [](const auto& state) { return !state.muted; });
    engine.setVolume(999);
    waitFor(engine, "Clamp volume", [](const auto& state) { return std::abs(state.volume - 100) < 0.01; });
    const double beforePlayback = engine.state().position;
    engine.togglePause();
    waitFor(engine, "Decode and advance playback", [&](const auto& state) {
        return !state.paused && state.position > beforePlayback + 0.15;
    });
    engine.togglePause();
    waitFor(engine, "Pause advancing playback", [](const auto& state) { return state.paused; });
    std::cout << "PASS pause, seek, playback progress, volume bounds, mute, and speed\n";

    if (!headless) {
        const auto frame = engine.captureFrame();
        require(frame && frame->width == 320 && frame->height == 180 && frame->bgra.size() >= 320 * 180 * 4,
                "Raw frame capture must return decoded BGRA pixels");
        const auto image = std::filesystem::absolute(directory / "decoded-frame.png");
        std::error_code ignored;
        std::filesystem::remove(image, ignored);
        require(engine.command({"screenshot-to-file", utf8(image), "video"}), "Cannot enqueue screenshot");
        waitFor(engine, "Capture decoded video frame", [&](const auto&) {
            std::error_code error;
            return std::filesystem::file_size(image, error) > 512 && !error;
        });
        std::ifstream input(image, std::ios::binary);
        char signature[8]{};
        input.read(signature, sizeof(signature));
        require(std::string(signature, sizeof(signature)) == std::string("\x89PNG\r\n\x1a\n", 8),
                "Decoded frame is not a PNG");
        std::cout << "PASS GPU output and decoded frame screenshot\n";
    }

    engine.addSubtitle(utf8(subtitlePath));
    waitFor(engine, "Load external subtitle", [](const auto& state) { return hasTrack(state, "sub", true, true); });
    std::cout << "PASS external subtitle discovery and selection\n";

    require(engine.open({second}, true), "Cannot append playlist entry");
    waitFor(engine, "Append playlist", [&](const auto& state) {
        return state.playlist.size() == 2 && state.playlist[0].filename == first
            && state.playlist[1].filename == second && state.playlistPosition == 0;
    });
    engine.selectPlaylist(1);
    waitFor(engine, "Select playlist entry", [&](const auto& state) {
        return state.playlistPosition == 1 && loaded(state, second);
    });
    engine.command({"playlist-prev", "force"});
    waitFor(engine, "Previous playlist entry", [&](const auto& state) {
        return state.playlistPosition == 0 && loaded(state, first);
    });
    engine.command({"playlist-next", "force"});
    waitFor(engine, "Next playlist entry", [&](const auto& state) {
        return state.playlistPosition == 1 && loaded(state, second);
    });
    require(engine.open({first, second}), "Cannot replace with multiple playlist entries");
    require(!engine.state().loaded, "Replacing media must invalidate loaded state synchronously");
    waitFor(engine, "Replace ordered playlist", [&](const auto& state) {
        return state.playlist.size() == 2 && state.playlist[0].filename == first
            && state.playlist[1].filename == second && state.playlistPosition == 0 && loaded(state, first);
    });
    std::cout << "PASS playlist append, replace, order, selection, previous, and next\n";

    const auto missing = utf8(std::filesystem::absolute(directory / "does-not-exist.mkv"));
    engine.open({missing});
    waitFor(engine, "Report missing file", [](const auto& state) {
        return state.idle && !state.loaded && !state.error.empty();
    }, 8s, true);
    require(engine.open({first}), "Cannot enqueue recovery playback");
    waitFor(engine, "Recover from missing file", [&](const auto& state) {
        return state.error.empty() && loaded(state, first);
    });
    engine.stop();
    waitFor(engine, "Stop", [](const auto& state) { return state.idle && !state.loaded; });
    std::cout << "PASS missing-file error, recovery, and stop\n";

    engine.shutdown();
    require(!engine.ready(), "Engine must report stopped after shutdown");
    require(!engine.state().loaded, "Engine must clear loaded state after shutdown");
    engine.shutdown();
    require(engine.initialize(window.video, window.parent, WM_APP + 71), "Engine reinitialization: " + engine.error());
    configure(engine, headless);
    require(engine.open({second}), "Cannot enqueue playback after reinitialization");
    waitFor(engine, "Playback after reinitialization", [&](const auto& state) { return loaded(state, second); });
    engine.shutdown();
    require(!engine.ready(), "Final shutdown failed");
    std::cout << "PASS idempotent shutdown, reinitialization, and playback\n";
}
} // namespace

int main() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 2;
    struct ArgumentCleanup {
        wchar_t** arguments;
        ~ArgumentCleanup() { LocalFree(arguments); }
    } cleanup{argv};
    bool runtime = false;
    bool headless = false;
    std::filesystem::path fixtures;
    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (argument == L"--runtime") runtime = true;
        else if (argument == L"--headless") headless = true;
        else if (argument == L"--fixtures" && i + 1 < argc) fixtures = std::filesystem::path(argv[++i]);
        else {
            std::cerr << "Usage: velin_engine_smoke --runtime --fixtures <directory> [--headless]\n";
            return 2;
        }
    }
    if (!runtime) {
        std::cout << "SKIP real playback requires --runtime and generated fixtures; run scripts/Test-Playback.ps1\n";
        return 77;
    }
    if (fixtures.empty()) {
        std::cerr << "--fixtures <directory> is required with --runtime\n";
        return 2;
    }
    try {
        SetProcessDPIAware();
        run(fixtures, headless);
        std::cout << "All real playback integration checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
