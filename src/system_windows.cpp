// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
#include "velin/system.hpp"
#include "velin/core.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <dpapi.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace velin::sys {
namespace {
void ensure(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
std::string winError(const char* operation) { return std::string(operation) + " failed (Windows error " + std::to_string(GetLastError()) + ")"; }

template <std::size_t Size>
std::array<unsigned char, Size> hash(BCRYPT_ALG_HANDLE algorithm, std::string_view data, const char* name) {
    std::array<unsigned char, Size> digest{};
    ensure(data.size() <= std::numeric_limits<ULONG>::max(), std::string(name) + " input is too large");
    const auto status = BCryptHash(algorithm, nullptr, 0,
        reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()),
        digest.data(), static_cast<ULONG>(digest.size()));
    ensure(status >= 0, std::string(name) + " failed");
    return digest;
}

struct InternetHandle {
    HINTERNET value = nullptr;
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
};
struct WinHttpUrl {
    std::wstring host, resource;
    INTERNET_PORT port = 0;
    bool secure = false;
};
WinHttpUrl crackUrl(const std::string& value) {
    auto text = fromUtf8(value);
    ensure(!text.empty() && text.find(L'\0') == std::wstring::npos, "Invalid URL");
    URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    ensure(WinHttpCrackUrl(text.c_str(), static_cast<DWORD>(text.size()), 0, &parts) != 0, "Invalid HTTP URL");
    ensure(parts.nScheme == INTERNET_SCHEME_HTTP || parts.nScheme == INTERNET_SCHEME_HTTPS, "Only HTTP and HTTPS URLs are supported");
    ensure(parts.dwUserNameLength == 0 && parts.dwPasswordLength == 0, "Credentials must be supplied as headers, not in URLs");
    WinHttpUrl result;
    result.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    result.resource.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) result.resource.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (result.resource.empty()) result.resource = L"/";
    result.port = parts.nPort; result.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    return result;
}

std::wstring quoteArgument(const std::wstring& input) {
    std::wstring output = L"\"";
    unsigned slashes = 0;
    for (wchar_t c : input) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'\"') { output.append(slashes * 2 + 1, L'\\'); output += L'\"'; }
        else { output.append(slashes, L'\\'); output += c; }
        slashes = 0;
    }
    output.append(slashes * 2, L'\\'); output += L'\"'; return output;
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { close(); }
    void close() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = nullptr; }
};
} // namespace

std::array<unsigned char, 20> sha1(std::string_view data) { return hash<20>(BCRYPT_SHA1_ALG_HANDLE, data, "SHA-1"); }
std::array<unsigned char, 32> sha256(std::string_view data) { return hash<32>(BCRYPT_SHA256_ALG_HANDLE, data, "SHA-256"); }

std::array<unsigned char, 32> sha256File(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    ensure(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "Cannot initialize SHA-256");
    struct Cleanup { BCRYPT_ALG_HANDLE& a; BCRYPT_HASH_HANDLE& h; ~Cleanup() { if (h) BCryptDestroyHash(h); if (a) BCryptCloseAlgorithmProvider(a, 0); } } cleanup{algorithm, hash};
    DWORD objectLength = 0, copied = 0;
    ensure(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &copied, 0) >= 0, "Cannot query SHA-256");
    std::vector<unsigned char> object(objectLength);
    ensure(BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) >= 0, "Cannot create SHA-256 hash");
    std::ifstream input(path, std::ios::binary); ensure(static_cast<bool>(input), "Cannot read file for hashing");
    char buffer[65536];
    while (input.read(buffer, sizeof(buffer)) || input.gcount())
        ensure(BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer), static_cast<ULONG>(input.gcount()), 0) >= 0, "SHA-256 update failed");
    ensure(input.eof(), "File read failed during hashing");
    std::array<unsigned char, 32> digest{};
    ensure(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0, "SHA-256 finish failed");
    return digest;
}

void randomBytes(void* buffer, std::size_t size) {
    ensure(size <= std::numeric_limits<ULONG>::max(), "Random request is too large");
    ensure(BCryptGenRandom(nullptr, static_cast<PUCHAR>(buffer), static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0,
        "Cannot generate random bytes");
}

void secureZero(void* buffer, std::size_t size) { if (buffer && size) SecureZeroMemory(buffer, size); }

std::string protectSecret(std::string_view plain, std::string_view description) {
    ensure(plain.size() <= std::numeric_limits<DWORD>::max(), "Secret is too large");
    const auto label = fromUtf8(description);
    DATA_BLOB input{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, label.empty() ? nullptr : label.c_str(), nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
        throw std::runtime_error("Cannot encrypt the secret (Windows error " + std::to_string(GetLastError()) + ")");
    std::string result(reinterpret_cast<const char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return result;
}

std::string unprotectSecret(std::string_view protectedBytes) {
    ensure(protectedBytes.size() <= std::numeric_limits<DWORD>::max(), "Secret is too large");
    DATA_BLOB input{static_cast<DWORD>(protectedBytes.size()), reinterpret_cast<BYTE*>(const_cast<char*>(protectedBytes.data()))};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
        throw std::runtime_error("Cannot decrypt the secret (Windows error " + std::to_string(GetLastError()) + ")");
    std::string result(reinterpret_cast<const char*>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
}

void replaceFile(const std::filesystem::path& source, const std::filesystem::path& target) {
    ensure(MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
        "Cannot atomically replace file");
}

std::filesystem::path findExecutable(std::string_view name) {
    const auto wide = fromUtf8(name);
    if (wide.empty()) return {};
    std::wstring buffer(32768, L'\0');
    const DWORD length = SearchPathW(nullptr, wide.c_str(), L".exe", static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (!length || length >= buffer.size()) return {};
    buffer.resize(length);
    return buffer;
}

std::string decodeCodePage(unsigned codePage, std::string_view bytes) {
    if (bytes.empty()) return {};
    ensure(bytes.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()), "Text is too large");
    const int length = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    ensure(length > 0, "File contains invalid bytes or this encoding is unavailable on Windows");
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    ensure(MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), wide.data(), length) == length,
        "Cannot decode text file");
    return toUtf8(wide);
}

std::string preferredLocale() {
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
    GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
    return toUtf8(locale);
}

unsigned long processId() { return GetCurrentProcessId(); }

std::filesystem::path executableDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

std::filesystem::path dataDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD overrideLength = GetEnvironmentVariableW(L"VELIN_DATA_DIR", path.data(), static_cast<DWORD>(path.size()));
    if (overrideLength && overrideLength < path.size()) {
        const std::filesystem::path overridePath(std::wstring(path.data(), overrideLength));
        if (overridePath.is_absolute()) return overridePath;
    }
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    path.resize(length);
    return std::filesystem::path(path) / L"Velin";
}

HttpExchangeResponse httpExchange(const HttpExchangeRequest& request, std::stop_token stop) {
    const auto url = crackUrl(request.url);
    InternetHandle session{WinHttpOpen(L"Velin/0.2.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    ensure(session.value != nullptr, winError("WinHttpOpen"));
    WinHttpSetTimeouts(session.value, request.timeoutMilliseconds, request.timeoutMilliseconds,
        request.timeoutMilliseconds, request.timeoutMilliseconds);
    InternetHandle connection{WinHttpConnect(session.value, url.host.c_str(), url.port, 0)};
    ensure(connection.value != nullptr, winError("WinHttpConnect"));
    const auto method = fromUtf8(request.method);
    std::atomic<HINTERNET> active{WinHttpOpenRequest(connection.value, method.c_str(), url.resource.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, url.secure ? WINHTTP_FLAG_SECURE : 0)};
    ensure(active.load() != nullptr, winError("WinHttpOpenRequest"));
    struct ActiveCleanup { std::atomic<HINTERNET>& handle; ~ActiveCleanup() { if (auto value = handle.exchange(nullptr)) WinHttpCloseHandle(value); } } cleanup{active};
    std::stop_callback cancel(stop, [&active] { if (auto value = active.exchange(nullptr)) WinHttpCloseHandle(value); });
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(active.load(), WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    std::wstring headers;
    for (const auto& [key, value] : request.headers) headers += fromUtf8(key + ": " + value + "\r\n");
    ensure(request.body.size() <= static_cast<std::size_t>(std::numeric_limits<DWORD>::max()), "HTTP request body is too large");
    ensure(WinHttpSendRequest(active.load(), headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0 : static_cast<DWORD>(-1), request.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(request.body.data()),
        static_cast<DWORD>(request.body.size()), static_cast<DWORD>(request.body.size()), 0) != 0, winError("WinHttpSendRequest"));
    ensure(WinHttpReceiveResponse(active.load(), nullptr) != 0, winError("WinHttpReceiveResponse"));
    DWORD status = 0, size = sizeof(status);
    ensure(WinHttpQueryHeaders(active.load(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) != 0, "Cannot read HTTP status");
    HttpExchangeResponse result;
    result.status = status;
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
        DWORD length = 0;
        WinHttpQueryHeaders(active.load(), WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &length, WINHTTP_NO_HEADER_INDEX);
        ensure(length > 0 && length < 65536, "Invalid HTTP redirect");
        std::wstring location(length / sizeof(wchar_t), L'\0');
        ensure(WinHttpQueryHeaders(active.load(), WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
            location.data(), &length, WINHTTP_NO_HEADER_INDEX) != 0, "Cannot read HTTP redirect");
        location.resize(wcslen(location.c_str()));
        result.location = toUtf8(location);
        return result;
    }
    while (true) {
        if (stop.stop_requested()) throw std::runtime_error("Cancelled");
        char buffer[16384]; DWORD count = 0;
        ensure(WinHttpReadData(active.load(), buffer, sizeof(buffer), &count) != 0, winError("WinHttpReadData"));
        if (!count) break;
        ensure(result.body.size() + count <= request.maximumBytes, "HTTP response exceeded its size limit");
        result.body.append(buffer, count);
    }
    return result;
}

struct ChildProcess::Impl {
    Handle outputRead, errorRead, process, job;
    DWORD status = 0;
};

ChildProcess::ChildProcess(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
                           const std::filesystem::path& workingDirectory) : impl_(std::make_unique<Impl>()) {
    std::wstring command = quoteArgument(executable.wstring());
    for (const auto& argument : arguments) {
        ensure(argument.find('\0') == std::string::npos, "Process argument contains NUL");
        const auto wide = fromUtf8(argument); ensure(argument.empty() || !wide.empty(), "Process argument is not valid UTF-8");
        command += L" " + quoteArgument(wide);
    }
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle outputWrite, errorWrite, input, thread;
    ensure(CreatePipe(&impl_->outputRead.value, &outputWrite.value, &security, 65536) != 0, "Cannot create stdout pipe");
    ensure(CreatePipe(&impl_->errorRead.value, &errorWrite.value, &security, 65536) != 0, "Cannot create stderr pipe");
    SetHandleInformation(impl_->outputRead.value, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(impl_->errorRead.value, HANDLE_FLAG_INHERIT, 0);
    input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
    ensure(input.value != INVALID_HANDLE_VALUE, "Cannot create helper stdin");
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.value; startup.StartupInfo.hStdOutput = outputWrite.value; startup.StartupInfo.hStdError = errorWrite.value;
    SIZE_T attributeSize = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<unsigned char> attributes(attributeSize);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    ensure(InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeSize) != 0, "Cannot initialize helper handle list");
    struct AttributeCleanup { LPPROC_THREAD_ATTRIBUTE_LIST list; ~AttributeCleanup() { DeleteProcThreadAttributeList(list); } } cleanup{startup.lpAttributeList};
    HANDLE inherited[]{input.value, outputWrite.value, errorWrite.value};
    ensure(UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr) != 0,
        "Cannot restrict inherited helper handles");
    PROCESS_INFORMATION created{};
    ensure(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, workingDirectory.c_str(), &startup.StartupInfo, &created) != 0,
        winError("CreateProcess"));
    impl_->process.value = created.hProcess; thread.value = created.hThread;
    outputWrite.close(); errorWrite.close(); input.close();
    impl_->job.value = CreateJobObjectW(nullptr, nullptr);
    if (impl_->job.value) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(impl_->job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        AssignProcessToJobObject(impl_->job.value, impl_->process.value);
    }
    ResumeThread(thread.value);
}

ChildProcess::~ChildProcess() = default;  // Closing the job object kills the helper tree.

std::size_t ChildProcess::read(bool standardError, char* buffer, std::size_t size) {
    const HANDLE pipe = standardError ? impl_->errorRead.value : impl_->outputRead.value;
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || !available) return 0;
    DWORD received = 0;
    if (!ReadFile(pipe, buffer, std::min<DWORD>(available, static_cast<DWORD>(std::min<std::size_t>(size, 1 << 20))), &received, nullptr)) return 0;
    return received;
}

bool ChildProcess::wait(int milliseconds) {
    if (WaitForSingleObject(impl_->process.value, static_cast<DWORD>(std::max(milliseconds, 0))) != WAIT_OBJECT_0) return false;
    ensure(GetExitCodeProcess(impl_->process.value, &impl_->status) != 0, "Cannot obtain helper exit status");
    return true;
}

void ChildProcess::terminate() {
    TerminateProcess(impl_->process.value, 1);
    WaitForSingleObject(impl_->process.value, 5000);
}

long long ChildProcess::exitCode() const { return static_cast<long long>(impl_->status); }

} // namespace velin::sys
