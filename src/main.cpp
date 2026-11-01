// SPDX-License-Identifier: GPL-3.0-or-later
#include "velin/player_window.hpp"
#include <shellapi.h>
#include <exception>
#include <algorithm>

namespace {
void printMessage(const std::wstring& message) {
    AttachConsole(ATTACH_PARENT_PROCESS);
    HANDLE output=GetStdHandle(STD_OUTPUT_HANDLE); DWORD written=0;
    if (output && output!=INVALID_HANDLE_VALUE) {
        const auto text=velin::toUtf8(message);
        WriteFile(output,text.data(),static_cast<DWORD>(text.size()),&written,nullptr);
    } else MessageBoxW(nullptr,message.c_str(),L"Velin",MB_OK|MB_ICONINFORMATION);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int exitCode = 1;
    try {
        int count = 0;
        LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        std::vector<std::string> paths;
        std::vector<std::wstring> raw;
        for (int i = 1; arguments && i < count; ++i) {
            raw.emplace_back(arguments[i]);
            paths.push_back(velin::toUtf8(arguments[i]));
        }
        if (arguments) LocalFree(arguments);
        bool showVersion=false, showHelp=false, independent=false;
        for (size_t i=0;i<raw.size();++i) {
            if (raw[i]==L"--") break;
            if (raw[i]==L"--command" || raw[i]==L"--data-dir") { if (i+1<raw.size()) ++i; continue; }
            if (raw[i]==L"--version") showVersion=true;
            else if (raw[i]==L"--help") showHelp=true;
            else if (raw[i]==L"--new-window" || raw[i]==L"--no-single-instance") independent=true;
        }
        if (showVersion) { printMessage(L"Velin 0.2.0\n"); if(SUCCEEDED(result))CoUninitialize(); return 0; }
        if (showHelp) {
            printMessage(L"Velin media player\nUsage: velin.exe [options] [files or URLs]\n\n"
                L"--enqueue       Append to the playlist\n--new-window    Open an independent player\n--fullscreen    Start fullscreen\n"
                L"--pip           Start in picture in picture\n--music         Start in music mode\n"
                L"--command TEXT  Run a Velin or mpv command\n--data-dir DIR  Use an absolute data directory\n"
                L"--register      Register media associations and the velin URL protocol\n--unregister    Remove registrations\n"
                L"--mpv-NAME=VALUE  Set an mpv property\n--version       Print the version\n");
            if(SUCCEEDED(result))CoUninitialize(); return 0;
        }
        for (size_t i=0;i<raw.size();++i) {
            if (raw[i]==L"--") break;
            if (raw[i]==L"--command") { if (i+1<raw.size()) ++i; continue; }
            if (raw[i]==L"--data-dir" && i+1<raw.size()) {
                const std::filesystem::path directory(raw[++i]);
                if (!directory.is_absolute()) throw std::runtime_error("--data-dir requires an absolute directory.");
                SetEnvironmentVariableW(L"VELIN_DATA_DIR",directory.c_str());
            }
        }
        velin::resolveLaunchPaths(raw);
        velin::SingleInstanceLock instanceLock;
        if (!independent && !instanceLock.primary() && velin::forwardToExisting(raw)) {
            if(SUCCEEDED(result))CoUninitialize(); return 0;
        }
        velin::PlayerWindow player(instance);
        exitCode = player.run(showCommand, paths);
    } catch (const std::exception& error) {
        MessageBoxW(nullptr, velin::fromUtf8(error.what()).c_str(), L"Velin", MB_OK | MB_ICONERROR);
    }
    if (SUCCEEDED(result)) CoUninitialize();
    return exitCode;
}
