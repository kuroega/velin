// SPDX-License-Identifier: GPL-3.0-or-later
#include "velin/thumbnails.hpp"
#include "velin/platform.hpp"
#include "velin/core.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

namespace {
constexpr UINT notification = WM_APP + 17;
void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
void pump() { MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message); }
velin::TimelineThumbnail awaitFrame(velin::ThumbnailService& service) {
    const auto deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline) {
        pump();
        if (auto frame = service.takeResult()) return std::move(*frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("Thumbnail completion timed out");
}
void awaitNotification() {
    MSG message{};
    const auto deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline) {
        if (PeekMessageW(&message, nullptr, notification, notification, PM_REMOVE)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("Thumbnail notification timed out");
}
}
int main(int argc, char** argv) {
    if (argc < 2 || !std::filesystem::is_regular_file(argv[1])) return 77;
    const auto root = std::filesystem::current_path() / ("thumbnail-test-" + std::to_string(GetCurrentProcessId()));
    HWND window = nullptr;
    try {
        std::filesystem::create_directories(root);
        const auto helper = std::filesystem::absolute(argv[1]);
        const auto media = root / "fixture.mkv";
        auto command = velin::quoteWindowsArgument(helper.wstring()) + L" -nostdin -hide_banner -loglevel error -y -f lavfi -i color=c=red:s=320x180:r=2:d=4 -c:v ffv1 " + velin::quoteWindowsArgument(media.wstring());
        // FFmpeg's stderr goes to a log so a failed fixture says why.
        const auto log = root / "ffmpeg.log";
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE logFile = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(logFile != INVALID_HANDLE_VALUE, "Create FFmpeg log");
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
        startup.dwFlags = STARTF_USESTDHANDLES; startup.hStdOutput = logFile; startup.hStdError = logFile;
        const bool created = CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &process);
        CloseHandle(logFile);
        check(created, "Create actual FFmpeg fixture");
        CloseHandle(process.hThread);
        const auto wait = WaitForSingleObject(process.hProcess, 15000);
        if (wait != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 2000); }
        DWORD exitCode = 1; GetExitCodeProcess(process.hProcess, &exitCode); CloseHandle(process.hProcess);
        if (wait != WAIT_OBJECT_0 || exitCode != 0) {
            char code[16]{}; std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(exitCode));
            std::ifstream output(log, std::ios::binary);
            const std::string detail{std::istreambuf_iterator<char>(output), std::istreambuf_iterator<char>()};
            throw std::runtime_error(std::string("Generate lossless video fixture (exit ") + code + (wait != WAIT_OBJECT_0 ? ", timed out" : "") + "): " + detail);
        }
        window = CreateWindowExW(0, L"STATIC", L"Thumbnail test", 0, 0, 0, 1, 1, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        check(window != nullptr, "Create notification window");
        {
            velin::ThumbnailService service;
            check(service.initialize(window, notification, root / "cache", helper), "Initialize thumbnail worker");
            service.request(velin::toUtf8(media.wstring()), 0.2, 160);
            auto frame = awaitFrame(service);
            check(frame.seconds == 0 && frame.width == 160 && frame.height == 90, "Decode dimensions and half-second bucket");
            check(frame.bgra.size() == 160 * 90 * 4 && frame.bgra[2] > 240 && frame.bgra[0] < 10 && frame.bgra[3] == 255, "WIC decodes actual red frame into BGRA");
            const auto cached = frame.file;
            const auto stamp = std::filesystem::last_write_time(cached);
            {
                // This executable cannot generate PNGs: a successful result proves cache decoding.
                wchar_t system[MAX_PATH]{}; GetSystemDirectoryW(system, MAX_PATH);
                velin::ThumbnailService reader;
                check(reader.initialize(window, notification, root / "cache", std::filesystem::path(system) / "cmd.exe"), "Initialize cache-only reader");
                reader.request(velin::toUtf8(media.wstring()), 0, 160);
                auto hit = awaitFrame(reader);
                check(hit.bgra == frame.bgra && hit.file == cached && std::filesystem::last_write_time(cached) == stamp, "Persistent cache hit without decoder process");
            }
            pump();
            service.request(velin::toUtf8(media.wstring()), 0, 160);
            awaitNotification(); // Leave a completed frame unconsumed.
            service.request(velin::toUtf8(media.wstring()), 1.7, 320);
            check(!service.takeResult(), "New request clears an unconsumed stale frame");
            service.request(velin::toUtf8(media.wstring()), 2.3, 240);
            frame = awaitFrame(service);
            check(frame.seconds == 2 && frame.width == 240, "Latest request wins during decode and resize");
            service.request(velin::toUtf8(media.wstring()), 3, 320);
            service.cancel();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            check(!service.takeResult(), "Cancellation suppresses pending and active results");
        }
        DestroyWindow(window); window = nullptr;
        std::filesystem::remove_all(root);
        std::cout << "PASS actual thumbnail decode, pixels, persistent cache, resize, latest request and cancellation\n";
        return 0;
    } catch (const std::exception& error) {
        if (window) DestroyWindow(window);
        std::cerr << "FAIL " << error.what() << "\n";
        return 1;
    }
}
