// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace velin {
struct TimelineThumbnail {
    std::filesystem::path file;
    std::string media;
    double seconds = 0;
    unsigned width = 0, height = 0;
    std::vector<std::uint8_t> bgra; // Premultiplied BGRA, width * height * 4.
};

// Latest-request-wins worker. It posts notifyMessage on completion; the UI then
// calls takeResult and creates its own Direct2D bitmap from the returned pixels.
class ThumbnailService final {
public:
    ThumbnailService();
    ~ThumbnailService();
    bool initialize(HWND window, UINT notifyMessage, std::filesystem::path cacheDirectory,
                    std::filesystem::path ffmpegExecutable = {});
    bool available() const;
    void request(std::string media, double seconds, unsigned width = 240);
    void cancel();
    std::optional<TimelineThumbnail> takeResult();
    std::wstring error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace velin
