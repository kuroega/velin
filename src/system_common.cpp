// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
#include "velin/system.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <stdexcept>

namespace velin::sys {
namespace {
void ensure(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
} // namespace

HttpUrl parseHttpUrl(std::string_view url) {
    ensure(!url.empty() && url.size() < 65536 && url.find('\0') == std::string_view::npos, "Invalid URL");
    for (const unsigned char c : url) ensure(c >= 0x20 && c != 0x7f, "Invalid URL");
    const auto schemeEnd = url.find("://");
    ensure(schemeEnd != std::string_view::npos, "Invalid HTTP URL");
    const auto scheme = lower(url.substr(0, schemeEnd));
    ensure(scheme == "http" || scheme == "https", "Only HTTP and HTTPS URLs are supported");
    HttpUrl result;
    result.secure = scheme == "https";
    auto rest = url.substr(schemeEnd + 3);
    const auto authorityEnd = std::min(rest.find_first_of("/?#"), rest.size());
    const auto authority = rest.substr(0, authorityEnd);
    rest.remove_prefix(authorityEnd);
    ensure(authority.find('@') == std::string_view::npos, "Credentials must be supplied as headers, not in URLs");
    std::string_view port;
    if (authority.starts_with('[')) {
        const auto close = authority.find(']');
        ensure(close != std::string_view::npos && close > 1, "Invalid HTTP URL");
        result.host = std::string(authority.substr(1, close - 1));
        const auto after = authority.substr(close + 1);
        ensure(after.empty() || after.starts_with(':'), "Invalid HTTP URL");
        if (!after.empty()) port = after.substr(1);
    } else {
        const auto colon = authority.find(':');
        result.host = std::string(authority.substr(0, colon));
        if (colon != std::string_view::npos) port = authority.substr(colon + 1);
        ensure(result.host.find(':') == std::string::npos, "Invalid HTTP URL");
    }
    ensure(!result.host.empty(), "Invalid HTTP URL");
    result.port = result.secure ? 443 : 80;
    if (!port.empty()) {
        unsigned value = 0;
        const auto parsed = std::from_chars(port.data(), port.data() + port.size(), value);
        ensure(parsed.ec == std::errc{} && parsed.ptr == port.data() + port.size() && value > 0 && value <= 65535, "Invalid HTTP URL port");
        result.port = static_cast<std::uint16_t>(value);
    }
    result.resource = std::string(rest.substr(0, rest.find('#')));
    if (result.resource.empty() || result.resource.front() == '?') result.resource.insert(0, "/");
    return result;
}

std::string httpOrigin(const HttpUrl& url) {
    const bool ipv6 = url.host.find(':') != std::string::npos;
    return std::string(url.secure ? "https://" : "http://") + (ipv6 ? "[" + url.host + "]" : url.host) + ":" + std::to_string(url.port);
}

std::string base64Encode(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::string result;
    result.reserve((size + 2) / 3 * 4);
    for (std::size_t i = 0; i < size; i += 3) {
        const unsigned value = (bytes[i] << 16) | (i + 1 < size ? bytes[i + 1] << 8 : 0) | (i + 2 < size ? bytes[i + 2] : 0);
        result += kAlphabet[(value >> 18) & 63];
        result += kAlphabet[(value >> 12) & 63];
        result += i + 1 < size ? kAlphabet[(value >> 6) & 63] : '=';
        result += i + 2 < size ? kAlphabet[value & 63] : '=';
    }
    return result;
}

bool base64Decode(std::string_view text, std::string& output) {
    output.clear();
    if (text.size() % 4 != 0) return false;
    auto digit = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        int values[4];
        for (int j = 0; j < 4; ++j) {
            const char c = text[i + j];
            if (c == '=' && last && j >= 2 && (j == 3 || text[i + 3] == '=')) { values[j] = 0; continue; }
            values[j] = digit(c);
            if (values[j] < 0) { output.clear(); return false; }
        }
        const unsigned value = (values[0] << 18) | (values[1] << 12) | (values[2] << 6) | values[3];
        output += static_cast<char>(value >> 16);
        if (text[i + 2] != '=') output += static_cast<char>((value >> 8) & 255);
        if (text[i + 3] != '=') output += static_cast<char>(value & 255);
    }
    return true;
}

} // namespace velin::sys
