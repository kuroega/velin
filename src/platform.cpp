// SPDX-License-Identifier: GPL-3.0-or-later
#include "velin/platform.hpp"
#include "velin/core.hpp"
#if defined(__MINGW32__)
// WIDL's generated foundation header aliases boolean and BYTE to the same C++
// type and specializes IReference twice. This unused specialization is omitted.
#define ____FIReference_1_boolean_INTERFACE_DEFINED__
#endif
#include <windows.media.h>
#include <systemmediatransportcontrolsinterop.h>
#include <roapi.h>
#include <winstring.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <propkey.h>
#include <propvarutil.h>
#include <sddl.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>
#include <utility>

namespace velin {
namespace {
namespace Media = ABI::Windows::Media;
namespace Foundation = ABI::Windows::Foundation;
constexpr UINT platformMessage = WM_APP + 61;
constexpr UINT firstTaskbarButton = 0x7501;
constexpr ULONG_PTR launchMagic = 0x56454c49;
template<class T> class ComPtr {
public:
    ~ComPtr() { reset(); }
    T* operator->() const { return value_; }
    T* get() const { return value_; }
    T** put() { reset(); return &value_; }
    explicit operator bool() const { return value_ != nullptr; }
    void reset() { if (value_) { value_->Release(); value_ = nullptr; } }
private: T* value_ = nullptr;
};
class HString {
public:
    explicit HString(std::wstring_view value) { WindowsCreateString(value.data(), static_cast<UINT32>(value.size()), &value_); }
    ~HString() { if (value_) WindowsDeleteString(value_); }
    operator HSTRING() const { return value_; }
private: HSTRING value_{};
};
std::wstring userSid(HANDLE process) {
    HANDLE token{};
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return {};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> data(size);
    const bool okay = GetTokenInformation(token, TokenUser, data.data(), size, &size) != FALSE;
    CloseHandle(token);
    if (!okay) return {};
    LPWSTR text{};
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &text)) return {};
    std::wstring result(text);
    LocalFree(text);
    return result;
}
bool sameUser(HWND window) {
    DWORD id{};
    GetWindowThreadProcessId(window, &id);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id);
    if (!process) return false;
    const auto other = userSid(process);
    CloseHandle(process);
    return !other.empty() && other == userSid(GetCurrentProcess());
}
std::wstring systemError(LSTATUS status) {
    LPWSTR text{};
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(status), 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    std::wstring result = text ? text : L"The Windows operation failed.";
    if (text) LocalFree(text);
    return result;
}
LSTATUS registryString(const std::wstring& path, const wchar_t* name, std::wstring_view value) {
    HKEY key{};
    auto result = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (result != ERROR_SUCCESS) return result;
    result = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.data()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return result;
}
template<class Interface, class Argument> class MediaEvent final : public Interface {
public:
    explicit MediaEvent(std::function<void(Argument*)> callback) : callback_(std::move(callback)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (id == IID_IUnknown || id == __uuidof(Interface) || id == IID_IAgileObject) {
            *result = static_cast<Interface*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --references_; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE Invoke(Media::ISystemMediaTransportControls*, Argument* argument) override {
        callback_(argument); return S_OK;
    }
private:
    std::atomic<ULONG> references_{1};
    std::function<void(Argument*)> callback_;
};
using ButtonHandler = Foundation::ITypedEventHandler<Media::SystemMediaTransportControls*, Media::SystemMediaTransportControlsButtonPressedEventArgs*>;
using SeekHandler = Foundation::ITypedEventHandler<Media::SystemMediaTransportControls*, Media::PlaybackPositionChangeRequestedEventArgs*>;
struct PlatformEvents {
    std::atomic<HWND> window{};
    std::mutex mutex;
    std::deque<std::pair<PlatformCommand, double>> events;
    void enqueue(PlatformCommand command, double value = 0) {
        std::lock_guard lock(mutex);
        const auto target = window.load();
        if (!target) return;
        if (events.size() < 128) events.emplace_back(command, value);
        PostMessageW(target, platformMessage, 0, 0);
    }
};
} // namespace

std::filesystem::path executableDirectory() {
    std::wstring path(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size()) return {};
    path.resize(size);
    return std::filesystem::path(path).parent_path();
}

std::wstring quoteWindowsArgument(std::wstring_view argument) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t value : argument) {
        if (value == L'\\') { ++slashes; continue; }
        if (value == L'\"') { result.append(slashes * 2 + 1, L'\\'); result.push_back(value); }
        else { result.append(slashes, L'\\'); result.push_back(value); }
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

SingleInstanceLock::SingleInstanceLock() {
    const auto sid = userSid(GetCurrentProcess());
    if (sid.empty()) return;
    mutex_ = CreateMutexW(nullptr, FALSE, (L"Local\\Velin.Player." + sid).c_str());
    primary_ = !mutex_ || GetLastError() != ERROR_ALREADY_EXISTS;
}
SingleInstanceLock::~SingleInstanceLock() { if (mutex_) CloseHandle(mutex_); }

void resolveLaunchPaths(std::vector<std::wstring>& arguments) {
    bool positionalOnly = false;
    for (size_t i = 0; i < arguments.size(); ++i) {
        auto& value = arguments[i];
        if (!positionalOnly) {
            if (value == L"--") { positionalOnly = true; continue; }
            if (value == L"--command" || value == L"--data-dir") {
                if (i + 1 < arguments.size()) ++i;
                continue;
            }
            if (value == L"--enqueue" || value == L"--fullscreen" || value == L"--pip" ||
                value == L"--music" || value == L"--new-window" || value == L"--no-single-instance" ||
                value == L"--register" || value == L"--unregister" || value == L"--help" || value == L"--version" ||
                value.starts_with(L"--command=") || value.starts_with(L"--mpv-") || value.starts_with(L"--plugin-")) continue;
        }
        if (value.empty() || value.find(L"://") != std::wstring::npos) continue;
        std::error_code error;
        const auto absolute = std::filesystem::absolute(std::filesystem::path(value), error);
        if (!error) value = absolute.lexically_normal().wstring();
    }
}

bool forwardToExisting(const std::vector<std::wstring>& arguments, DWORD timeoutMilliseconds) {
    std::vector<wchar_t> payload;
    for (const auto& argument : arguments) {
        if (argument.find(L'\0') != std::wstring::npos) return false;
        payload.insert(payload.end(), argument.begin(), argument.end());
        payload.push_back(L'\0');
    }
    payload.push_back(L'\0');
    if (payload.size() * sizeof(wchar_t) > 1024 * 1024) return false;
    HWND window = nullptr;
    const auto deadline = GetTickCount64() + std::min<DWORD>(timeoutMilliseconds, 10000);
    do {
        HWND candidate = nullptr;
        while ((candidate = FindWindowExW(nullptr, candidate, L"Velin.Player", nullptr)) != nullptr) {
            if (sameUser(candidate)) { window = candidate; break; }
        }
        if (window) break;
        Sleep(25);
    } while (GetTickCount64() < deadline);
    if (!window) return false;
    COPYDATASTRUCT data{launchMagic, static_cast<DWORD>(payload.size() * sizeof(wchar_t)), payload.data()};
    DWORD_PTR accepted = 0;
    const auto result = SendMessageTimeoutW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, std::max<DWORD>(100, timeoutMilliseconds), &accepted);
    if (result && accepted) {
        ShowWindowAsync(window, SW_RESTORE);
        SetForegroundWindow(window);
        return true;
    }
    return false;
}

std::optional<std::vector<std::wstring>> receiveLaunchRequest(LPARAM value) {
    const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(value);
    if (!data || data->dwData != launchMagic || !data->lpData || data->cbData < sizeof(wchar_t)
        || data->cbData > 1024 * 1024 || data->cbData % sizeof(wchar_t)) return std::nullopt;
    const auto* text = static_cast<const wchar_t*>(data->lpData);
    const size_t count = data->cbData / sizeof(wchar_t);
    if (text[count - 1] != L'\0') return std::nullopt;
    std::vector<std::wstring> arguments;
    size_t start = 0;
    for (size_t index = 0; index < count; ++index) {
        if (!text[index]) {
            if (index == start && index == count - 1) break;
            arguments.emplace_back(text + start, index - start);
            start = index + 1;
            if (arguments.size() > 4096) return std::nullopt;
        }
    }
    return arguments;
}

struct PlatformIntegration::Impl {
    HWND window{};
    Callback callback;
    ComPtr<ITaskbarList3> taskbar;
    ComPtr<Media::ISystemMediaTransportControls> controls;
    ComPtr<Media::ISystemMediaTransportControls2> controls2;
    ComPtr<Media::ISystemMediaTransportControlsDisplayUpdater> display;
    ComPtr<Media::ISystemMediaTransportControlsTimelineProperties> timeline;
    EventRegistrationToken buttonToken{}, seekToken{};
    bool buttonRegistered = false, seekRegistered = false, roInitialized = false, taskbarButtons = false;
    UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarButtonCreated");
    std::shared_ptr<PlatformEvents> eventQueue = std::make_shared<PlatformEvents>();
    std::wstring error, lastTitle, lastArtist, lastAlbum;
    PlatformPlayback playback;
    HICON previousIcon{}, playIcon{}, pauseIcon{}, nextIcon{};

    ~Impl() {
        eventQueue->window = nullptr;
        if (controls) {
            if (buttonRegistered) controls->remove_ButtonPressed(buttonToken);
            controls->put_IsEnabled(FALSE);
        }
        if (controls2 && seekRegistered) controls2->remove_PlaybackPositionChangeRequested(seekToken);
        controls2.reset(); controls.reset(); display.reset(); timeline.reset();
        for (auto icon : {previousIcon, playIcon, pauseIcon, nextIcon}) if (icon) DestroyIcon(icon);
        if (roInitialized) RoUninitialize();
    }
    static HICON transportIcon(int type) {
        constexpr int edge = 32;
        BITMAPV5HEADER info{};
        info.bV5Size = sizeof(info); info.bV5Width = edge; info.bV5Height = -edge;
        info.bV5Planes = 1; info.bV5BitCount = 32; info.bV5Compression = BI_BITFIELDS;
        info.bV5RedMask = 0xff0000; info.bV5GreenMask = 0xff00; info.bV5BlueMask = 0xff; info.bV5AlphaMask = 0xff000000;
        void* bits{};
        HDC screen = GetDC(nullptr);
        HBITMAP bitmap = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&info), DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (!bitmap || !bits) return nullptr;
        auto* pixels = static_cast<DWORD*>(bits);
        std::fill_n(pixels, edge * edge, 0);
        for (int y = 6; y < 26; ++y) for (int x = 5; x < 27; ++x) {
            bool fill = false;
            const int dy = std::abs(y - 16);
            if (type == 0) fill = (x >= 10 && x <= 24 - dy) || (x >= 5 && x <= 7);
            if (type == 1) fill = x >= 10 && x <= 24 - dy;
            if (type == 2) fill = (x >= 9 && x <= 12) || (x >= 19 && x <= 22);
            if (type == 3) fill = (x <= 22 && x >= 8 + dy) || (x >= 25 && x <= 27);
            if (fill) pixels[y * edge + x] = 0xffc2afff;
        }
        HBITMAP mask = CreateBitmap(edge, edge, 1, 1, nullptr);
        ICONINFO iconInfo{TRUE, 0, 0, mask, bitmap};
        HICON icon = CreateIconIndirect(&iconInfo);
        DeleteObject(mask); DeleteObject(bitmap);
        return icon;
    }
    void refreshTaskbar() {
        if (!taskbar || !window) return;
        THUMBBUTTON buttons[3]{};
        const HICON icons[]{previousIcon, playback.paused ? playIcon : pauseIcon, nextIcon};
        const wchar_t* tips[]{L"Previous", playback.paused ? L"Play" : L"Pause", L"Next"};
        for (UINT i = 0; i < 3; ++i) {
            buttons[i].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
            buttons[i].iId = firstTaskbarButton + i;
            buttons[i].hIcon = icons[i];
            lstrcpynW(buttons[i].szTip, tips[i], ARRAYSIZE(buttons[i].szTip));
            const bool enabled = !playback.idle && (i == 1 || (i == 0 ? playback.hasPrevious : playback.hasNext));
            buttons[i].dwFlags = enabled ? THBF_ENABLED : THBF_DISABLED;
        }
        if (!taskbarButtons) taskbarButtons = SUCCEEDED(taskbar->ThumbBarAddButtons(window, 3, buttons));
        else taskbar->ThumbBarUpdateButtons(window, 3, buttons);
        if (playback.idle || playback.duration <= 0) taskbar->SetProgressState(window, TBPF_NOPROGRESS);
        else {
            taskbar->SetProgressState(window, playback.paused ? TBPF_PAUSED : TBPF_NORMAL);
            taskbar->SetProgressValue(window, static_cast<ULONGLONG>(std::max(0.0, playback.position) * 1000),
                static_cast<ULONGLONG>(std::max(1.0, playback.duration) * 1000));
        }
    }
};

PlatformIntegration::PlatformIntegration() : impl_(std::make_unique<Impl>()) {}
PlatformIntegration::~PlatformIntegration() = default;
bool PlatformIntegration::initialize(HWND window, Callback callback) {
    impl_->window = window; impl_->callback = std::move(callback);
    impl_->eventQueue->window = window;
    SetCurrentProcessExplicitAppUserModelID(L"Velin.Player");
    if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(ITaskbarList3), reinterpret_cast<void**>(impl_->taskbar.put())))) impl_->taskbar->HrInit();
    impl_->previousIcon = Impl::transportIcon(0); impl_->playIcon = Impl::transportIcon(1);
    impl_->pauseIcon = Impl::transportIcon(2); impl_->nextIcon = Impl::transportIcon(3);
    const HRESULT apartment = RoInitialize(RO_INIT_SINGLETHREADED);
    impl_->roInitialized = SUCCEEDED(apartment);
    ComPtr<ISystemMediaTransportControlsInterop> interop;
    HRESULT result = RoGetActivationFactory(HString(L"Windows.Media.SystemMediaTransportControls"),
        __uuidof(ISystemMediaTransportControlsInterop), reinterpret_cast<void**>(interop.put()));
    if (SUCCEEDED(result)) result = interop->GetForWindow(window, __uuidof(Media::ISystemMediaTransportControls),
        reinterpret_cast<void**>(impl_->controls.put()));
    if (FAILED(result)) {
        impl_->error = L"Windows media transport controls are unavailable (HRESULT " + std::to_wstring(static_cast<unsigned long>(result)) + L").";
        return false;
    }
    impl_->controls->put_IsPlayEnabled(TRUE); impl_->controls->put_IsPauseEnabled(TRUE);
    impl_->controls->put_IsStopEnabled(TRUE); impl_->controls->put_IsEnabled(TRUE);
    impl_->controls->get_DisplayUpdater(impl_->display.put());
    auto* buttons = new MediaEvent<ButtonHandler, Media::ISystemMediaTransportControlsButtonPressedEventArgs>([state = impl_->eventQueue](auto* argument) {
        Media::SystemMediaTransportControlsButton button{};
        if (FAILED(argument->get_Button(&button))) return;
        switch (button) {
        case Media::SystemMediaTransportControlsButton_Play: state->enqueue(PlatformCommand::Play); break;
        case Media::SystemMediaTransportControlsButton_Pause: state->enqueue(PlatformCommand::Pause); break;
        case Media::SystemMediaTransportControlsButton_Stop: state->enqueue(PlatformCommand::Stop); break;
        case Media::SystemMediaTransportControlsButton_Previous: state->enqueue(PlatformCommand::Previous); break;
        case Media::SystemMediaTransportControlsButton_Next: state->enqueue(PlatformCommand::Next); break;
        default: break;
        }
    });
    impl_->buttonRegistered = SUCCEEDED(impl_->controls->add_ButtonPressed(buttons, &impl_->buttonToken));
    buttons->Release();
    impl_->controls->QueryInterface(__uuidof(Media::ISystemMediaTransportControls2), reinterpret_cast<void**>(impl_->controls2.put()));
    if (impl_->controls2) {
        ComPtr<IInspectable> instance;
        if (SUCCEEDED(RoActivateInstance(HString(L"Windows.Media.SystemMediaTransportControlsTimelineProperties"), instance.put())))
            instance->QueryInterface(__uuidof(Media::ISystemMediaTransportControlsTimelineProperties), reinterpret_cast<void**>(impl_->timeline.put()));
        auto* seek = new MediaEvent<SeekHandler, Media::IPlaybackPositionChangeRequestedEventArgs>([state = impl_->eventQueue](auto* argument) {
            Foundation::TimeSpan position{};
            if (SUCCEEDED(argument->get_RequestedPlaybackPosition(&position))) state->enqueue(PlatformCommand::Seek, static_cast<double>(position.Duration) / 10000000.0);
        });
        impl_->seekRegistered = SUCCEEDED(impl_->controls2->add_PlaybackPositionChangeRequested(seek, &impl_->seekToken));
        seek->Release();
    }
    return true;
}

void PlatformIntegration::update(const PlatformPlayback& value) {
    impl_->playback = value;
    impl_->refreshTaskbar();
    if (!impl_->controls) return;
    impl_->controls->put_IsEnabled(!value.idle);
    impl_->controls->put_PlaybackStatus(value.idle ? Media::MediaPlaybackStatus_Stopped :
        value.paused ? Media::MediaPlaybackStatus_Paused : Media::MediaPlaybackStatus_Playing);
    impl_->controls->put_IsNextEnabled(value.hasNext); impl_->controls->put_IsPreviousEnabled(value.hasPrevious);
    if (impl_->display && (value.title != impl_->lastTitle || value.artist != impl_->lastArtist || value.album != impl_->lastAlbum)) {
        impl_->display->put_Type(value.audioOnly ? Media::MediaPlaybackType_Music : Media::MediaPlaybackType_Video);
        if (value.audioOnly) {
            ComPtr<Media::IMusicDisplayProperties> metadata;
            if (SUCCEEDED(impl_->display->get_MusicProperties(metadata.put()))) {
                metadata->put_Title(HString(value.title)); metadata->put_Artist(HString(value.artist));
                metadata->put_AlbumArtist(HString(value.album));
            }
        }
#ifdef _MSC_VER
        // MinGW's windows.media.h only forward-declares IVideoDisplayProperties.
        else {
            ComPtr<Media::IVideoDisplayProperties> metadata;
            if (SUCCEEDED(impl_->display->get_VideoProperties(metadata.put()))) metadata->put_Title(HString(value.title));
        }
#endif
        impl_->display->Update(); impl_->lastTitle = value.title; impl_->lastArtist = value.artist; impl_->lastAlbum = value.album;
    }
    if (impl_->controls2 && impl_->timeline && std::isfinite(value.duration) && std::isfinite(value.position)) {
        const Foundation::TimeSpan start{0}, end{static_cast<INT64>(std::clamp(value.duration, 0.0, 1.0e9) * 10000000.0)};
        const Foundation::TimeSpan position{static_cast<INT64>(std::clamp(value.position, 0.0, std::max(0.0, value.duration)) * 10000000.0)};
        impl_->timeline->put_StartTime(start); impl_->timeline->put_MinSeekTime(start);
        impl_->timeline->put_EndTime(end); impl_->timeline->put_MaxSeekTime(end); impl_->timeline->put_Position(position);
        impl_->controls2->UpdateTimelineProperties(impl_->timeline.get());
        impl_->controls2->put_PlaybackRate(std::isfinite(value.speed) ? value.speed : 1.0);
    }
}
bool PlatformIntegration::handleMessage(UINT message, WPARAM wparam, LPARAM) {
    if (message == impl_->taskbarCreated) { impl_->taskbarButtons = false; impl_->refreshTaskbar(); return true; }
    if (message == WM_COMMAND && HIWORD(wparam) == THBN_CLICKED && LOWORD(wparam) >= firstTaskbarButton && LOWORD(wparam) < firstTaskbarButton + 3) {
        const PlatformCommand actions[]{PlatformCommand::Previous, PlatformCommand::TogglePause, PlatformCommand::Next};
        if (impl_->callback) impl_->callback(actions[LOWORD(wparam) - firstTaskbarButton], 0);
        return true;
    }
    if (message != platformMessage) return false;
    std::deque<std::pair<PlatformCommand, double>> events;
    { std::lock_guard lock(impl_->eventQueue->mutex); events.swap(impl_->eventQueue->events); }
    for (const auto& [command, value] : events) if (impl_->callback) impl_->callback(command, value);
    return true;
}
bool PlatformIntegration::mediaControlsAvailable() const { return static_cast<bool>(impl_->controls); }
std::wstring PlatformIntegration::error() const { return impl_->error; }

void PlatformIntegration::setRecentFiles(const std::vector<std::string>& paths) {
    ComPtr<ICustomDestinationList> list;
    if (FAILED(CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(ICustomDestinationList), reinterpret_cast<void**>(list.put())))) return;
    list->SetAppID(L"Velin.Player");
    UINT slots{}; ComPtr<IObjectArray> removed;
    if (FAILED(list->BeginList(&slots, __uuidof(IObjectArray), reinterpret_cast<void**>(removed.put())))) return;
    std::vector<std::wstring> excluded;
    UINT removedCount{};
    if (removed) removed->GetCount(&removedCount);
    for (UINT i = 0; i < removedCount; ++i) {
        ComPtr<IShellLinkW> link;
        if (SUCCEEDED(removed->GetAt(i, __uuidof(IShellLinkW), reinterpret_cast<void**>(link.put())))) {
            std::wstring args(32768, L'\0');
            if (SUCCEEDED(link->GetArguments(args.data(), static_cast<int>(args.size())))) { args.resize(wcslen(args.c_str())); excluded.push_back(std::move(args)); }
        }
    }
    ComPtr<IObjectCollection> collection;
    if (FAILED(CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IObjectCollection), reinterpret_cast<void**>(collection.put())))) { list->AbortList(); return; }
    UINT added = 0;
    for (const auto& path : paths) {
        if (added >= std::min<UINT>(slots, 12)) break;
        const auto wide = fromUtf8(path);
        std::error_code ignored;
        if (!std::filesystem::is_regular_file(std::filesystem::path(wide), ignored)) continue;
        const auto args = quoteWindowsArgument(wide);
        if (std::find(excluded.begin(), excluded.end(), args) != excluded.end()) continue;
        ComPtr<IShellLinkW> link;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IShellLinkW), reinterpret_cast<void**>(link.put())))) continue;
        link->SetPath((executableDirectory() / L"velin.exe").c_str()); link->SetArguments(args.c_str());
        link->SetIconLocation((executableDirectory() / L"velin.exe").c_str(), 0);
        ComPtr<IPropertyStore> properties;
        if (SUCCEEDED(link->QueryInterface(__uuidof(IPropertyStore), reinterpret_cast<void**>(properties.put())))) {
            PROPVARIANT title{}; const auto label = fromUtf8(mediaTitle(path));
            InitPropVariantFromString(label.c_str(), &title); properties->SetValue(PKEY_Title, title); PropVariantClear(&title);
            properties->Commit();
        }
        if (SUCCEEDED(collection->AddObject(link.get()))) ++added;
    }
    ComPtr<IObjectArray> array;
    collection->QueryInterface(__uuidof(IObjectArray), reinterpret_cast<void**>(array.put()));
    if (added && array) list->AppendCategory(L"Recent media", array.get());
    list->CommitList();
}

bool PlatformIntegration::registerProtocol(bool enable, std::wstring* error) {
    const std::wstring root = L"Software\\Classes\\velin";
    LSTATUS status = ERROR_SUCCESS;
    if (enable) {
        const auto executable = executableDirectory() / L"velin.exe";
        status = registryString(root, nullptr, L"URL:Velin media protocol");
        if (!status) status = registryString(root, L"URL Protocol", L"");
        if (!status) status = registryString(root + L"\\DefaultIcon", nullptr, quoteWindowsArgument(executable.wstring()) + L",0");
        if (!status) status = registryString(root + L"\\shell\\open\\command", nullptr, quoteWindowsArgument(executable.wstring()) + L" \"%1\"");
    } else { status = RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str()); if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS; }
    if (error && status) *error = systemError(status);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return status == ERROR_SUCCESS;
}

bool PlatformIntegration::registerFileAssociations(bool enable, std::wstring* error) {
    const std::wstring root = L"Software\\Classes\\Velin.Media";
    constexpr const wchar_t* extensions[]{L".mkv",L".mp4",L".webm",L".avi",L".mov",L".m4v",L".mp3",L".flac",L".wav",L".ogg",L".m4a",L".opus",L".m3u",L".m3u8",L".pls"};
    LSTATUS status = ERROR_SUCCESS;
    if (enable) {
        const auto executable = executableDirectory() / L"velin.exe";
        status = registryString(root, nullptr, L"Velin media file");
        if (!status) status = registryString(root + L"\\DefaultIcon", nullptr, quoteWindowsArgument(executable.wstring()) + L",0");
        if (!status) status = registryString(root + L"\\shell\\open\\command", nullptr, quoteWindowsArgument(executable.wstring()) + L" \"%1\"");
        for (const auto* extension : extensions) {
            if (status) break;
            HKEY key{};
            status = RegCreateKeyExW(HKEY_CURRENT_USER, (L"Software\\Classes\\" + std::wstring(extension) + L"\\OpenWithProgids").c_str(),
                0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
            if (!status) { status = RegSetValueExW(key, L"Velin.Media", 0, REG_NONE, nullptr, 0); RegCloseKey(key); }
            if (!status) status = registryString(L"Software\\Velin\\Capabilities\\FileAssociations", extension, L"Velin.Media");
        }
        if (!status) status = registryString(L"Software\\Velin\\Capabilities", L"ApplicationName", L"Velin");
        if (!status) status = registryString(L"Software\\Velin\\Capabilities", L"ApplicationDescription", L"Native Windows media player");
        if (!status) status = registryString(L"Software\\RegisteredApplications", L"Velin", L"Software\\Velin\\Capabilities");
    } else {
        status = RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str()); if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
        for (const auto* extension : extensions) {
            HKEY key{};
            if (RegOpenKeyExW(HKEY_CURRENT_USER, (L"Software\\Classes\\" + std::wstring(extension) + L"\\OpenWithProgids").c_str(), 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
                RegDeleteValueW(key, L"Velin.Media"); RegCloseKey(key);
            }
        }
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Velin\\Capabilities");
        HKEY key{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
            RegDeleteValueW(key, L"Velin"); RegCloseKey(key);
        }
    }
    if (error && status) *error = systemError(status);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return status == ERROR_SUCCESS;
}
bool PlatformIntegration::showDefaultAppsSettings() {
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}
} // namespace velin
