// SPDX-License-Identifier: GPL-3.0-or-later
#include "velin/thumbnails.hpp"
#include "velin/platform.hpp"
#include "velin/core.hpp"
#include <bcrypt.h>
#include <wincodec.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

namespace velin {
namespace {
template<class T> struct ComPtr {
    T* value{};
    ~ComPtr() { if (value) value->Release(); }
    T** put() { return &value; }
    T* operator->() const { return value; }
};
std::wstring digest(std::string_view value) {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    std::array<UCHAR, 32> bytes{};
    const bool okay = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0
        && BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())), static_cast<ULONG>(value.size()), 0) >= 0
        && BCryptFinishHash(hash, bytes.data(), static_cast<ULONG>(bytes.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!okay) return {};
    std::wostringstream output;
    output << std::hex << std::setfill(L'0');
    for (auto byte : bytes) output << std::setw(2) << static_cast<unsigned>(byte);
    return output.str();
}
std::optional<TimelineThumbnail> decodePng(const std::filesystem::path& path) {
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IWICImagingFactory), reinterpret_cast<void**>(factory.put())))) return std::nullopt;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, decoder.put()))) return std::nullopt;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(decoder->GetFrame(0, frame.put())) || FAILED(factory->CreateFormatConverter(converter.put()))) return std::nullopt;
    if (FAILED(converter->Initialize(frame.value, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
        nullptr, 0, WICBitmapPaletteTypeCustom))) return std::nullopt;
    TimelineThumbnail result;
    if (FAILED(converter->GetSize(&result.width, &result.height)) || !result.width || !result.height
        || result.width > 2048 || result.height > 2048) return std::nullopt;
    result.bgra.resize(static_cast<size_t>(result.width) * result.height * 4);
    if (FAILED(converter->CopyPixels(nullptr, result.width * 4, static_cast<UINT>(result.bgra.size()), result.bgra.data()))) return std::nullopt;
    result.file = path;
    return result;
}
void pruneCache(const std::filesystem::path& directory) {
    std::error_code error;
    struct Cached { std::filesystem::path path; std::filesystem::file_time_type time; std::uintmax_t size; };
    std::vector<Cached> files;
    std::uintmax_t total = 0;
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error) || it->path().extension() != L".png") continue;
        const auto name = it->path().stem().wstring();
        if (name.size() != 64 || !std::all_of(name.begin(), name.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); })) continue;
        const auto size = it->file_size(error); const auto time = it->last_write_time(error);
        if (!error) { total += size; files.push_back({it->path(), time, size}); }
    }
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) { return left.time < right.time; });
    constexpr std::uintmax_t limit = 128ULL * 1024 * 1024;
    for (const auto& file : files) {
        if (total <= limit) break;
        if (std::filesystem::remove(file.path, error)) total -= file.size;
    }
}
} // namespace

struct ThumbnailService::Impl {
    struct Request { std::string media; double seconds; unsigned width; std::uint64_t serial; };
    std::filesystem::path cache, helper;
    HWND window{};
    UINT message{};
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> serial{0};
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<Request> pending;
    std::optional<Request> active;
    std::optional<TimelineThumbnail> result;
    std::wstring lastError;
    std::thread worker;
    bool initialized = false;

    ~Impl() { stop = true; ++serial; condition.notify_all(); if (worker.joinable()) worker.join(); }
    bool canceled(std::uint64_t number) const { return stop || serial.load() != number; }
    bool generate(const Request& request, const std::filesystem::path& destination) {
        const auto temporary = destination.parent_path() / (destination.stem().wstring() + L"."
            + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(request.serial) + L".partial.png");
        std::wostringstream timestamp;
        timestamp.imbue(std::locale::classic()); timestamp << std::fixed << std::setprecision(3) << request.seconds;
        const auto scale = L"scale=" + std::to_wstring(request.width) + L":" + std::to_wstring(request.width * 9 / 16) + L":force_original_aspect_ratio=decrease";
        std::wstring arguments = quoteWindowsArgument(helper.wstring()) + L" -nostdin -hide_banner -loglevel error -y -ss "
            + timestamp.str() + L" -i " + quoteWindowsArgument(fromUtf8(request.media))
            + L" -frames:v 1 -an -sn -vf " + quoteWindowsArgument(scale) + L" " + quoteWindowsArgument(temporary.wstring());
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(helper.c_str(), arguments.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, cache.c_str(), &startup, &process)) return false;
        CloseHandle(process.hThread);
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
            AssignProcessToJobObject(job, process.hProcess);
        }
        const auto deadline = GetTickCount64() + 15000;
        bool completed = false;
        while (WaitForSingleObject(process.hProcess, 40) == WAIT_TIMEOUT) {
            if (canceled(request.serial) || GetTickCount64() >= deadline) {
                TerminateProcess(process.hProcess, ERROR_CANCELLED);
                WaitForSingleObject(process.hProcess, 2000); break;
            }
        }
        DWORD exitCode = STILL_ACTIVE;
        if (GetExitCodeProcess(process.hProcess, &exitCode)) completed = exitCode == 0 && !canceled(request.serial);
        CloseHandle(process.hProcess); if (job) CloseHandle(job);
        std::error_code error;
        if (completed && std::filesystem::is_regular_file(temporary, error)) {
            std::filesystem::rename(temporary, destination, error);
            completed = !error || std::filesystem::is_regular_file(destination, error);
        }
        std::filesystem::remove(temporary, error);
        return completed;
    }
    void run() {
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (!stop) {
            Request request;
            { std::unique_lock lock(mutex); condition.wait(lock, [&] { return stop || pending.has_value(); });
                if (stop) break;
                request = std::move(*pending); pending.reset(); active = request; }
            struct Finish {
                Impl* state; std::uint64_t number;
                ~Finish() { std::lock_guard lock(state->mutex); if (state->active && state->active->serial == number) state->active.reset(); }
            } finish{this, request.serial};
            std::string fingerprint = request.media + "\n" + std::to_string(request.seconds) + "\n" + std::to_string(request.width);
            const auto source = std::filesystem::path(fromUtf8(request.media));
            std::error_code error;
            if (std::filesystem::is_regular_file(source, error)) {
                fingerprint += "\n" + std::to_string(std::filesystem::file_size(source, error));
                fingerprint += "\n" + std::to_string(std::filesystem::last_write_time(source, error).time_since_epoch().count());
            }
            const auto key = digest(fingerprint);
            if (key.empty() || canceled(request.serial)) continue;
            const auto image = cache / (key + L".png");
            if (!std::filesystem::is_regular_file(image, error) && !generate(request, image)) continue;
            if (canceled(request.serial)) continue;
            auto thumbnail = decodePng(image);
            if (!thumbnail) { std::filesystem::remove(image, error); continue; }
            thumbnail->media = request.media; thumbnail->seconds = request.seconds;
            { std::lock_guard lock(mutex); if (canceled(request.serial)) continue; result = std::move(thumbnail); }
            PostMessageW(window, message, 0, 0);
            pruneCache(cache);
        }
        if (SUCCEEDED(apartment)) CoUninitialize();
    }
};

ThumbnailService::ThumbnailService() : impl_(std::make_unique<Impl>()) {}
ThumbnailService::~ThumbnailService() = default;
bool ThumbnailService::initialize(HWND window, UINT message, std::filesystem::path cache, std::filesystem::path helper) {
    if (impl_->initialized) return false;
    if (helper.empty()) helper = executableDirectory() / L"ffmpeg.exe";
    std::error_code error;
    if (!helper.is_absolute() || !std::filesystem::is_regular_file(helper, error)) {
        impl_->lastError = L"Timeline previews require ffmpeg.exe beside Velin."; return false;
    }
    if (!window || message < WM_APP || cache.empty() || !cache.is_absolute()) return false;
    std::filesystem::create_directories(cache, error);
    if (error) { impl_->lastError = L"Cannot create the timeline thumbnail cache."; return false; }
    impl_->window = window; impl_->message = message; impl_->cache = std::move(cache); impl_->helper = std::move(helper);
    impl_->initialized = true; impl_->worker = std::thread([state = impl_.get()] { state->run(); });
    return true;
}
bool ThumbnailService::available() const { return impl_->initialized; }
void ThumbnailService::request(std::string media, double seconds, unsigned width) {
    if (!available() || media.empty() || media.find('\0') != std::string::npos || !std::isfinite(seconds) || seconds < 0) return;
    // Half-second buckets avoid starting a decoder for every mouse pixel.
    seconds = std::floor(seconds * 2) / 2;
    width = std::clamp(width, 96U, 640U);
    { std::lock_guard lock(impl_->mutex);
        if (impl_->pending && impl_->pending->media == media && impl_->pending->seconds == seconds && impl_->pending->width == width) return;
        if (impl_->active && impl_->active->media == media && impl_->active->seconds == seconds && impl_->active->width == width && impl_->active->serial == impl_->serial.load()) return;
        const auto number = ++impl_->serial;
        // A completed but unconsumed frame belongs to the previous request.
        impl_->result.reset();
        impl_->pending = Impl::Request{std::move(media), seconds, width, number}; }
    impl_->condition.notify_one();
}
void ThumbnailService::cancel() { ++impl_->serial; std::lock_guard lock(impl_->mutex); impl_->pending.reset(); impl_->result.reset(); }
std::optional<TimelineThumbnail> ThumbnailService::takeResult() { std::lock_guard lock(impl_->mutex); auto result = std::move(impl_->result); impl_->result.reset(); return result; }
std::wstring ThumbnailService::error() const { std::lock_guard lock(impl_->mutex); return impl_->lastError; }
} // namespace velin
