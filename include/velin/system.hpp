// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
#pragma once

// The small set of operating-system services the portable libraries need.
// system_common.cpp holds the portable parts; system_windows.cpp and
// system_apple.mm implement the rest. Nothing here includes OS headers.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace velin::sys {

// ---- Portable helpers (system_common.cpp) ---------------------------------

struct HttpUrl {
    std::string host;      // Without IPv6 brackets.
    std::string resource;  // Path and query; "/" when empty. The fragment is dropped.
    std::uint16_t port = 0;
    bool secure = false;
};
// Accepts absolute http and https URLs. Rejects credentials in the URL.
// Throws std::runtime_error on malformed input.
HttpUrl parseHttpUrl(std::string_view url);
// "scheme://host:port" with IPv6 hosts bracketed.
std::string httpOrigin(const HttpUrl& url);

std::string base64Encode(const void* data, std::size_t size);
// Strict RFC 4648 decoding with padding. Returns false on any invalid input.
bool base64Decode(std::string_view text, std::string& output);

// ---- Platform services ----------------------------------------------------

std::array<unsigned char, 20> sha1(std::string_view data);
std::array<unsigned char, 32> sha256(std::string_view data);
std::array<unsigned char, 32> sha256File(const std::filesystem::path& path);  // Throws.
void randomBytes(void* buffer, std::size_t size);                            // Throws.
void secureZero(void* buffer, std::size_t size);

// Encrypts data so only the current user on this device can read it back
// (DPAPI on Windows, a Keychain-held key on Apple platforms). Throws.
std::string protectSecret(std::string_view plain, std::string_view description);
std::string unprotectSecret(std::string_view protectedBytes);

// Atomically replaces target with source (both on the same volume). Throws.
void replaceFile(const std::filesystem::path& source, const std::filesystem::path& target);
// Searches PATH for an executable. Returns an empty path when not found.
std::filesystem::path findExecutable(std::string_view name);

// Decodes text in a Windows code page number (1252, 932, 936, ...) to UTF-8.
// Throws when the bytes are invalid or the encoding is unavailable.
std::string decodeCodePage(unsigned codePage, std::string_view bytes);
// The user's preferred locale as a BCP 47 tag, such as "en-US".
std::string preferredLocale();

unsigned long processId();
std::filesystem::path executableDirectory();
// VELIN_DATA_DIR when it is absolute, else the per-user application data folder.
std::filesystem::path dataDirectory();

// One HTTP exchange without following redirects. Throws on transport errors.
struct HttpExchangeRequest {
    std::string method = "GET";
    std::string url;
    std::map<std::string, std::string> headers;  // Already validated by the caller.
    std::string body;
    std::size_t maximumBytes = 32 * 1024 * 1024;
    int timeoutMilliseconds = 15000;
};
struct HttpExchangeResponse {
    unsigned status = 0;
    std::string location;  // Set for redirect responses.
    std::string body;      // Empty for redirect responses.
};
HttpExchangeResponse httpExchange(const HttpExchangeRequest& request, std::stop_token stop);

// A helper process with stdin from the null device and piped stdout/stderr.
// Destroying a running process terminates it. Not available on iOS, where
// the constructor throws.
class ChildProcess {
public:
    ChildProcess(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
                 const std::filesystem::path& workingDirectory);
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    // Non-blocking. Returns the bytes read, 0 when nothing is available.
    std::size_t read(bool standardError, char* buffer, std::size_t size);
    // Waits up to the given time for exit or new output. True once exited.
    bool wait(int milliseconds);
    void terminate();
    // Valid after wait() returned true.
    long long exitCode() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace velin::sys
