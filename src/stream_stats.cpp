// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
#include "velin/stream_stats.hpp"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace velin {
namespace {

using Json = nlohmann::json;

std::string printf1(const char* format, double value) {
    std::array<char, 64> buffer{};
    std::snprintf(buffer.data(), buffer.size(), format, value);
    return buffer.data();
}

const Json& missing() {
    static const Json value;
    return value;
}

class Values {
public:
    explicit Values(const std::map<std::string, std::string>& text) {
        for (const auto& [name, json] : text) {
            Json parsed = Json::parse(json, nullptr, false);
            if (!parsed.is_discarded() && !parsed.is_null()) values_.emplace(name, std::move(parsed));
        }
    }
    const Json& get(const std::string& name) const {
        const auto found = values_.find(name);
        return found == values_.end() ? missing() : found->second;
    }

private:
    std::map<std::string, Json> values_;
};

const Json& member(const Json& object, const char* name) {
    if (!object.is_object()) return missing();
    const auto found = object.find(name);
    return found == object.end() ? missing() : *found;
}

bool number(const Json& value, double& out) {
    if (!value.is_number()) return false;
    out = value.get<double>();
    return std::isfinite(out);
}

double numberOr(const Json& value, double fallback) {
    double out = 0;
    return number(value, out) ? out : fallback;
}

std::string text(const Json& value) {
    return value.is_string() ? value.get<std::string>() : std::string{};
}

bool flag(const Json& value) { return value.is_boolean() && value.get<bool>(); }

// mpv reports byte-size options as integers, older builds as "150MiB".
double byteSizeOption(const Json& value) {
    double out = 0;
    if (number(value, out)) return out;
    const auto raw = text(value);
    if (raw.empty()) return -1;
    char* end = nullptr;
    out = std::strtod(raw.c_str(), &end);
    if (end == raw.c_str() || !std::isfinite(out)) return -1;
    std::string suffix(end);
    for (auto& c : suffix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const std::map<std::string, double> scale{{"", 1}, {"b", 1}, {"kib", 1024.0}, {"mib", 1024.0 * 1024},
        {"gib", 1024.0 * 1024 * 1024}, {"kb", 1000.0}, {"mb", 1e6}, {"gb", 1e9}};
    const auto found = scale.find(suffix);
    return found == scale.end() ? -1 : out * found->second;
}

std::string fps(double value) { return printf1(value >= 100 ? "%.1f" : "%.3f", value); }

// "hevc (HEVC (High Efficiency Video Coding))" reads better as just "hevc"
// with the profile from the description when it adds something.
std::string shortCodec(const std::string& codec) {
    const auto open = codec.find(" (");
    return open == std::string::npos ? codec : codec.substr(0, open);
}

std::string channels(const Json& audio) {
    const auto layout = text(member(audio, "hr-channels"));
    if (!layout.empty()) return layout;
    const double count = numberOr(member(audio, "channel-count"), 0);
    return count > 0 ? printf1("%.0f ch", count) : std::string{};
}

void add(std::vector<StatsRow>& rows, std::string label, std::string value) {
    if (!value.empty()) rows.push_back({std::move(label), std::move(value)});
}

std::string join(const std::vector<std::string>& parts, const char* separator = " · ") {
    std::string result;
    for (const auto& part : parts) {
        if (part.empty()) continue;
        if (!result.empty()) result += separator;
        result += part;
    }
    return result;
}

// "mpv v0.40.0-dirty" or "0.40.0" -> "mpv 0.40.0-dirty".
std::string engineVersion(std::string version) {
    if (version.empty()) return {};
    if (version.rfind("mpv ", 0) == 0) version.erase(0, 4);
    if (version.size() > 1 && version[0] == 'v' && std::isdigit(static_cast<unsigned char>(version[1]))) version.erase(0, 1);
    return "mpv " + version;
}

// FFmpeg release tags start with "n": "n7.1" -> "7.1".
std::string ffmpegVersion(std::string version) {
    if (version.size() > 1 && version[0] == 'n' && std::isdigit(static_cast<unsigned char>(version[1]))) version.erase(0, 1);
    return version;
}

// mpv's GPU context or API names, as people know them.
std::string graphicsApi(const std::string& context, const std::string& api) {
    static const std::map<std::string, std::string> names{
        {"d3d11", "Direct3D 11"}, {"moltenvk", "Vulkan via MoltenVK"}, {"macvk", "Vulkan via MoltenVK"},
        {"winvk", "Vulkan"}, {"vulkan", "Vulkan"}, {"waylandvk", "Vulkan"}, {"x11vk", "Vulkan"}, {"androidvk", "Vulkan"},
        {"displayvk", "Vulkan"}, {"angle", "OpenGL ES via ANGLE"}, {"win", "OpenGL"}, {"dxinterop", "OpenGL"},
        {"opengl", "OpenGL"}, {"cocoa", "OpenGL"}};
    const auto& name = context.empty() || context == "auto" ? api : context;
    if (name.empty() || name == "auto") return {};
    const auto found = names.find(name);
    return found == names.end() ? name : found->second;
}

} // namespace

namespace {
constexpr double maxShownBufferTarget = 24 * 60 * 60;
} // namespace

const std::vector<std::string>& streamStatsProperties() {
    static const std::vector<std::string> names{
        "path", "file-format", "file-size", "duration", "demuxer-via-network", "demuxer-cache-state", "cache-speed",
        "cache-buffering-state", "paused-for-cache", "demuxer-max-bytes", "cache-secs",
        "video-codec", "video-params", "video-out-params", "hwdec-current", "current-vo",
        "container-fps", "estimated-vf-fps", "display-fps", "video-bitrate",
        "audio-codec-name", "audio-params", "audio-bitrate", "current-ao",
        "frame-drop-count", "decoder-frame-drop-count", "vo-delayed-frame-count", "avsync", "speed",
        "mpv-version", "ffmpeg-version", "current-gpu-context", "gpu-api"};
    return names;
}

std::string formatByteSize(double bytes) {
    if (!std::isfinite(bytes) || bytes < 0) return {};
    static constexpr std::array<const char*, 5> units{"B", "KiB", "MiB", "GiB", "TiB"};
    std::size_t unit = 0;
    while (bytes >= 1024 && unit + 1 < units.size()) { bytes /= 1024; ++unit; }
    if (unit == 0) return printf1("%.0f B", bytes);
    return printf1(bytes >= 100 ? "%.0f" : bytes >= 10 ? "%.1f" : "%.2f", bytes) + " " + units[unit];
}

std::string formatBitRate(double bitsPerSecond) {
    if (!std::isfinite(bitsPerSecond) || bitsPerSecond < 0) return {};
    if (bitsPerSecond >= 1e9) return printf1("%.2f Gbps", bitsPerSecond / 1e9);
    if (bitsPerSecond >= 1e6) return printf1(bitsPerSecond >= 1e8 ? "%.0f Mbps" : "%.1f Mbps", bitsPerSecond / 1e6);
    if (bitsPerSecond >= 1e3) return printf1("%.0f kbps", bitsPerSecond / 1e3);
    return printf1("%.0f bps", bitsPerSecond);
}

std::string streamSourceLabel(const std::string& path) {
    const auto scheme = path.find("://");
    if (scheme == std::string::npos || scheme == 0) return path.empty() ? std::string{} : "Local file";
    std::string protocol = path.substr(0, scheme);
    if (!std::all_of(protocol.begin(), protocol.end(), [](unsigned char c) { return std::isalnum(c) || c == '+' || c == '-' || c == '.'; }))
        return "Local file";
    for (auto& c : protocol) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (protocol == "file") return "Local file";
    auto host = path.substr(scheme + 3);
    host = host.substr(0, host.find_first_of("/?#"));
    // Drop any user:password@ before the host.
    if (const auto at = host.rfind('@'); at != std::string::npos) host = host.substr(at + 1);
    return host.empty() ? protocol : host + " (" + protocol + ")";
}

void StreamStatsSampler::reset() {
    path_.clear();
    speeds_.clear();
    buffers_.clear();
}

StreamStats StreamStatsSampler::sample(const std::map<std::string, std::string>& raw) {
    const Values values(raw);
    StreamStats stats;

    const auto path = text(values.get("path"));
    if (path != path_) { speeds_.clear(); buffers_.clear(); path_ = path; }

    const Json& cache = values.get("demuxer-cache-state");
    stats.network = flag(values.get("demuxer-via-network"));

    // Engine: what plays the stream. Velin always plays through libmpv.
    {
        StatsSection section{"Engine", {}};
        add(section.rows, "Player", engineVersion(text(values.get("mpv-version"))));
        add(section.rows, "FFmpeg", ffmpegVersion(text(values.get("ffmpeg-version"))));
        add(section.rows, "Renderer", join({text(values.get("current-vo")),
            graphicsApi(text(values.get("current-gpu-context")), text(values.get("gpu-api")))}));
        if (values.get("video-params").is_object()) {
            const auto hwdec = text(values.get("hwdec-current"));
            add(section.rows, "Decoding", hwdec.empty() || hwdec == "no" ? "software (CPU)" : "hardware (" + hwdec + ")");
        }
        add(section.rows, "Audio out", text(values.get("current-ao")));
        if (!section.rows.empty()) stats.sections.push_back(std::move(section));
    }

    // Source
    {
        StatsSection section{"Source", {}};
        add(section.rows, "Source", streamSourceLabel(path));
        add(section.rows, "Container", text(values.get("file-format")));
        double size = 0;
        if (number(values.get("file-size"), size) && size > 0) add(section.rows, "File size", formatByteSize(size));
        if (!section.rows.empty()) stats.sections.push_back(std::move(section));
    }

    // Network and buffer
    double speed = -1;
    if (!number(values.get("cache-speed"), speed)) number(member(cache, "raw-input-rate"), speed);
    double ahead = -1;
    number(member(cache, "cache-duration"), ahead);
    {
        StatsSection section{"Network & buffer", {}};
        if (speed >= 0) add(section.rows, "Download", formatBitRate(speed * 8) + "  (" + formatByteSize(speed) + "/s)");
        if (ahead >= 0) {
            std::string value = printf1("%.1f s", ahead);
            const double target = numberOr(values.get("cache-secs"), -1);
            // mpv's default is 1000 hours, and the byte limit binds long before
            // it; only a target someone actually set says anything.
            if (target > 0 && target <= maxShownBufferTarget && stats.network) value += printf1(" of %.0f s", target);
            add(section.rows, "Buffer ahead", value);
        }
        double forward = 0;
        if (number(member(cache, "fw-bytes"), forward)) {
            std::string value = formatByteSize(forward);
            const double limit = byteSizeOption(values.get("demuxer-max-bytes"));
            if (limit > 0) value += " of " + formatByteSize(limit);
            add(section.rows, "Buffered", value);
        }
        double total = 0;
        if (number(member(cache, "total-bytes"), total) && total > forward) add(section.rows, "In memory", formatByteSize(total));
        std::string state;
        if (flag(values.get("paused-for-cache"))) {
            const double percent = numberOr(values.get("cache-buffering-state"), -1);
            state = percent >= 0 ? printf1("Buffering %.0f%%", percent) : "Buffering";
        } else if (flag(member(cache, "underrun"))) state = "Stalled: waiting for data";
        else if (flag(member(cache, "eof"))) state = "Fully buffered";
        else if (cache.is_object()) state = flag(member(cache, "idle")) ? "Idle (buffer full)" : "Reading";
        add(section.rows, "State", state);
        if (!section.rows.empty()) stats.sections.push_back(std::move(section));
    }
    if (speed >= 0 || ahead >= 0) {
        speeds_.push_back(std::max(0.0, speed));
        buffers_.push_back(std::max(0.0, ahead));
        while (speeds_.size() > historyLength) speeds_.pop_front();
        while (buffers_.size() > historyLength) buffers_.pop_front();
    }
    stats.speedHistory.assign(speeds_.begin(), speeds_.end());
    stats.bufferHistory.assign(buffers_.begin(), buffers_.end());

    // Video
    const Json& video = values.get("video-params");
    if (video.is_object()) {
        StatsSection section{"Video", {}};
        add(section.rows, "Codec", shortCodec(text(values.get("video-codec"))));
        const double w = numberOr(member(video, "w"), 0), h = numberOr(member(video, "h"), 0);
        if (w > 0 && h > 0) {
            std::string value = printf1("%.0f", w) + "×" + printf1("%.0f", h);
            const Json& out = values.get("video-out-params");
            const double dw = numberOr(member(out, "dw"), 0), dh = numberOr(member(out, "dh"), 0);
            if (dw > 0 && dh > 0 && (dw != w || dh != h)) value += "  →  " + printf1("%.0f", dw) + "×" + printf1("%.0f", dh);
            add(section.rows, "Resolution", value);
        }
        std::vector<std::string> rates;
        double rate = 0;
        if (number(values.get("container-fps"), rate) && rate > 0) rates.push_back(fps(rate) + " fps");
        if (number(values.get("estimated-vf-fps"), rate) && rate > 0) rates.push_back(fps(rate) + " measured");
        add(section.rows, "Frame rate", join(rates));
        if (number(values.get("display-fps"), rate) && rate > 0) add(section.rows, "Display", printf1("%.2f Hz", rate));
        // mpv measures video-bitrate over the last keyframe interval only, so a
        // dark or still scene can read a few kbps; the file average (all
        // streams) puts it in proportion.
        double bits = 0, size = 0, length = 0;
        const bool current = number(values.get("video-bitrate"), bits) && bits > 0;
        const bool average = number(values.get("file-size"), size) && size > 0 && number(values.get("duration"), length) && length > 0;
        add(section.rows, "Bitrate", join({current ? formatBitRate(bits) + " now" : "",
                                           average ? formatBitRate(size * 8 / length) + " file avg" : ""}));
        auto format = text(member(video, "pixelformat"));
        if (const auto hw = text(member(video, "hw-pixelformat")); !hw.empty()) format += " (" + hw + ")";
        add(section.rows, "Pixel format", format);
        add(section.rows, "Color", join({text(member(video, "colormatrix")), text(member(video, "primaries")),
                                         text(member(video, "gamma")), text(member(video, "colorlevels"))}, " / "));
        stats.sections.push_back(std::move(section));
    }

    // Audio
    const Json& audio = values.get("audio-params");
    const auto audioCodec = text(values.get("audio-codec-name"));
    if (audio.is_object() || !audioCodec.empty()) {
        StatsSection section{"Audio", {}};
        add(section.rows, "Codec", audioCodec);
        double rate = 0;
        add(section.rows, "Format", join({number(member(audio, "samplerate"), rate) && rate > 0 ? printf1("%.1f kHz", rate / 1000) : "",
                                          channels(audio), text(member(audio, "format"))}));
        double bits = 0;
        if (number(values.get("audio-bitrate"), bits) && bits > 0) add(section.rows, "Bitrate", formatBitRate(bits));
        if (!section.rows.empty()) stats.sections.push_back(std::move(section));
    }

    // Playback
    {
        StatsSection section{"Playback", {}};
        double dropped = 0, decoder = 0, delayed = 0;
        const bool haveDropped = number(values.get("frame-drop-count"), dropped);
        const bool haveDecoder = number(values.get("decoder-frame-drop-count"), decoder);
        if (haveDropped || haveDecoder)
            add(section.rows, "Dropped frames", printf1("%.0f output", dropped) + printf1(" · %.0f decoder", decoder));
        if (number(values.get("vo-delayed-frame-count"), delayed) && delayed > 0) add(section.rows, "Delayed frames", printf1("%.0f", delayed));
        double sync = 0;
        if (number(values.get("avsync"), sync)) add(section.rows, "A/V sync", printf1("%+.3f s", sync));
        double speedFactor = 1;
        if (number(values.get("speed"), speedFactor) && std::abs(speedFactor - 1) > 1e-6) add(section.rows, "Speed", printf1("%.2f×", speedFactor));
        if (!section.rows.empty()) stats.sections.push_back(std::move(section));
    }
    return stats;
}

} // namespace velin
