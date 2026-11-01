// SPDX-License-Identifier: GPL-3.0-or-later
#include "velin/platform.hpp"
#include "velin/core.hpp"
#include <shellapi.h>
#include <algorithm>
#include <iostream>

int main() {
    int count{};
    wchar_t** values = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!values) return 1;
    std::vector<std::wstring> arguments;
    for (int index = 1; index < count; ++index) arguments.emplace_back(values[index]);
    LocalFree(values);
    bool separate = false;
    for (size_t index = 0; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if (argument == L"--") break;
        if (argument == L"--command" || argument == L"--data-dir") { if (index + 1 < arguments.size()) ++index; continue; }
        if (argument == L"--new-window" || argument == L"--no-single-instance") separate = true;
        if (argument == L"--help" || argument == L"-h") {
            std::cout << "Velin command-line controller\n"
                "Usage: velin-cli [options] [media paths or URLs]\n"
                "  --enqueue              Append media to the active playlist\n"
                "  --new-window           Start a separate player window\n"
                "  --fullscreen           Start in fullscreen\n"
                "  --pip                  Start in picture in picture\n"
                "  --music                Start in music mode\n"
                "  --command <command>    Send a player command to an existing window\n"
                "  --                     End command-line options\n"
                "  --version              Print the application version\n"
                "  --help                 Show this help\n";
            return 0;
        }
        if (argument == L"--version") { std::cout << "Velin " << VELIN_VERSION << '\n'; return 0; }
    }
    velin::resolveLaunchPaths(arguments);
    if (!separate && velin::forwardToExisting(arguments, 250)) return 0;
    const auto executable = velin::executableDirectory() / L"velin.exe";
    std::wstring command = velin::quoteWindowsArgument(executable.wstring());
    for (const auto& argument : arguments) command += L" " + velin::quoteWindowsArgument(argument);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
        nullptr, executable.parent_path().c_str(), &startup, &process)) {
        std::cerr << "Cannot start velin.exe (Windows error " << GetLastError() << ").\n";
        return 1;
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return 0;
}
