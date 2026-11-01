// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <filesystem>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace velin {
enum class PlatformCommand { Play, Pause, TogglePause, Stop, Previous, Next, Seek };
struct PlatformPlayback {
    std::wstring title, artist, album;
    double position = 0, duration = 0, speed = 1;
    bool idle = true, paused = false, audioOnly = false, hasPrevious = false, hasNext = false;
};

// Initialize and update on the UI thread. Event callbacks are marshaled back to
// that thread by handleMessage; call it from the owning window procedure.
class PlatformIntegration final {
public:
    using Callback = std::function<void(PlatformCommand, double)>;
    PlatformIntegration();
    ~PlatformIntegration();
    bool initialize(HWND window, Callback callback);
    void update(const PlatformPlayback& playback);
    bool handleMessage(UINT message, WPARAM wparam, LPARAM lparam);
    void setRecentFiles(const std::vector<std::string>& paths);
    bool mediaControlsAvailable() const;
    std::wstring error() const;
    static bool registerFileAssociations(bool enable, std::wstring* error = nullptr);
    static bool registerProtocol(bool enable, std::wstring* error = nullptr);
    static bool showDefaultAppsSettings();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class SingleInstanceLock final {
public:
    SingleInstanceLock();
    ~SingleInstanceLock();
    bool primary() const { return primary_; }
    SingleInstanceLock(const SingleInstanceLock&) = delete;
    SingleInstanceLock& operator=(const SingleInstanceLock&) = delete;
private:
    HANDLE mutex_ = nullptr;
    bool primary_ = true;
};

// Resolve positional local media against the sender's working directory before
// forwarding or launching a process with a different working directory.
void resolveLaunchPaths(std::vector<std::wstring>& arguments);

// WM_COPYDATA transport. Only windows owned by the same user are targeted.
bool forwardToExisting(const std::vector<std::wstring>& arguments, DWORD timeoutMilliseconds = 2000);
std::optional<std::vector<std::wstring>> receiveLaunchRequest(LPARAM copyData);
std::wstring quoteWindowsArgument(std::wstring_view argument);
std::filesystem::path executableDirectory();
struct TextRecognitionResult { std::wstring text, error; };
// Blocking Windows OCR operation intended for a worker, with a 15-second bound.
// Uses installed Windows profile languages and never uploads image pixels.
TextRecognitionResult recognizeTextBgra(unsigned width, unsigned height, const std::vector<std::uint8_t>& bgra);
} // namespace velin
