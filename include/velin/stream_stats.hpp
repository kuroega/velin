// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
//
// "Stats for nerds": a readable, live summary of what mpv is doing with the
// current stream (network speed, read-ahead buffer, codecs, frame drops).
// Portable: it works on property values as JSON text, the form
// MediaEngine::propertyJson returns, so it needs neither libmpv nor a UI.
#pragma once

#include <cstddef>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace velin {

struct StatsRow {
    std::string label, value;
};

struct StatsSection {
    std::string title;
    std::vector<StatsRow> rows;
};

struct StreamStats {
    std::vector<StatsSection> sections;
    // One sample per call to StreamStatsSampler::sample, oldest first.
    std::vector<double> speedHistory;   // Download speed, bytes per second.
    std::vector<double> bufferHistory;  // Seconds buffered ahead.
    bool network = false;               // The stream is read over the network.
};

// The mpv properties sample() reads.
const std::vector<std::string>& streamStatsProperties();

// "1.5 MiB", "820 KiB". Empty for negative or non-finite input.
std::string formatByteSize(double bytes);
// Bits per second: "12.3 Mbps", "640 kbps".
std::string formatBitRate(double bitsPerSecond);
// The scheme and host of a URL, never its path or query: AList links carry
// signatures there. "Local file" for anything that is not a URL.
std::string streamSourceLabel(const std::string& path);

class StreamStatsSampler {
public:
    static constexpr std::size_t historyLength = 60;
    // values maps each name in streamStatsProperties() to its JSON text;
    // missing or "null" entries are treated as unavailable. The history
    // restarts when the "path" value changes.
    StreamStats sample(const std::map<std::string, std::string>& values);
    void reset();

private:
    std::string path_;
    std::deque<double> speeds_, buffers_;
};

} // namespace velin
