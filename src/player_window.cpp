// SPDX-License-Identifier: GPL-3.0-or-later
#include "velin/player_window.hpp"
#include "velin/player_options.hpp"
#include "velin/preferences_dialog.hpp"
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace velin {
namespace {
// Design tokens, modelled on IINA's macOS HUD look: neutral graphite
// surfaces, translucent heads-up panels floating over the video, white
// glyphs and a single system-blue accent. Translucent tokens composite over
// whatever sits beneath them, so one value works on video and on panels.
struct Palette {
    D2D1_COLOR_F base, video, hud, hudEdge, panel;
    D2D1_COLOR_F hover, pressed, hairline, track, level, knob;
    D2D1_COLOR_F text, secondary, tertiary;
    D2D1_COLOR_F accent, accentHigh, accentLow, accentSoft, onAccent;
    D2D1_COLOR_F positive, danger;
};
D2D1_COLOR_F hex(unsigned value, float alpha = 1.f) { return D2D1::ColorF(value, alpha); }
D2D1_COLOR_F fade(D2D1_COLOR_F color, float alpha) { color.a = alpha; return color; }
const Palette darkTheme{
    hex(0x1c1c1e), hex(0x000000), hex(0x242427,.86f), hex(0xffffff,.12f), hex(0xffffff,.04f),
    hex(0xffffff,.1f), hex(0xffffff,.16f), hex(0xffffff,.08f), hex(0xffffff,.22f), hex(0xffffff,.92f), hex(0xffffff),
    hex(0xf5f5f7), hex(0xa1a1a6), hex(0x6e6e73),
    hex(0x0a84ff), hex(0x409cff), hex(0x0071e3), hex(0x0a84ff,.24f), hex(0xffffff),
    hex(0x30d158), hex(0xff453a)};
const Palette lightTheme{
    hex(0xececec), hex(0x000000), hex(0xf6f6f6,.88f), hex(0x000000,.1f), hex(0x000000,.035f),
    hex(0x000000,.06f), hex(0x000000,.1f), hex(0x000000,.08f), hex(0x000000,.14f), hex(0x1d1d1f,.85f), hex(0xffffff),
    hex(0x1d1d1f), hex(0x6e6e73), hex(0xa1a1a6),
    hex(0x007aff), hex(0x2a8cff), hex(0x0062cc), hex(0x007aff,.14f), hex(0xffffff),
    hex(0x34c759), hex(0xff3b30)};
Palette ui = darkTheme;
bool lightAppearance = false;
constexpr auto regular = DWRITE_FONT_WEIGHT_NORMAL;
constexpr auto semibold = DWRITE_FONT_WEIGHT_SEMI_BOLD;
constexpr auto centered = DWRITE_TEXT_ALIGNMENT_CENTER;
constexpr auto trailing = DWRITE_TEXT_ALIGNMENT_TRAILING;
constexpr float layerMargin = 18;  // Room around floating panels for their soft shadow.
constexpr D2D1_RECT_F expand(D2D1_RECT_F r, float d) { return {r.left-d, r.top-d, r.right+d, r.bottom+d}; }

constexpr auto rect(float x, float y, float w, float h) { return D2D1_RECT_F{x,y,x+w,y+h}; }
template<class T> void release(T*& pointer) { if (pointer) { pointer->Release(); pointer = nullptr; } }

void appendPluginMenu(HMENU root,const std::vector<PluginMenuItem>& items,UINT base) {
    std::vector<HMENU> levels{root};
    const auto count=std::min<size_t>(1000,items.size());
    for(size_t i=0;i<count;++i) {
        const auto& entry=items[i];
        const auto depth=std::min<size_t>(entry.depth,levels.size()-1);
        levels.resize(depth+1);
        const auto label=fromUtf8(entry.title+(entry.keyBinding.empty()?"":"\t"+entry.keyBinding));
        UINT flags=entry.separator?MF_SEPARATOR:MF_STRING|(entry.enabled?0:MF_GRAYED)|(entry.selected?MF_CHECKED:0);
        if(!entry.separator && i+1<count && items[i+1].depth>entry.depth) {
            HMENU child=CreatePopupMenu();
            AppendMenuW(levels.back(),flags|MF_POPUP,reinterpret_cast<UINT_PTR>(child),label.c_str());
            levels.push_back(child);
        } else AppendMenuW(levels.back(),flags,base+static_cast<UINT>(i),label.c_str());
    }
}

std::filesystem::path settingsDirectory() {
    std::wstring overridePath(32768,L'\0');
    const DWORD length=GetEnvironmentVariableW(L"VELIN_DATA_DIR",overridePath.data(),static_cast<DWORD>(overridePath.size()));
    if (length>0 && length<overridePath.size()) {
        overridePath.resize(length);
        std::filesystem::path directory(overridePath);
        if (directory.is_absolute()) return directory;
    }
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) {
        std::filesystem::path result = std::filesystem::path(path) / L"Velin";
        CoTaskMemFree(path);
        return result;
    }
    throw std::runtime_error("Cannot locate the Windows application data directory.");
}

std::wstring number(double value, int precision = 2) {
    std::wostringstream stream;
    stream << std::fixed << std::setprecision(precision) << value;
    auto result = stream.str();
    while (result.size() > 1 && result.back() == L'0') result.pop_back();
    if (result.back() == L'.') result.pop_back();
    return result;
}

std::wstring menuLabel(std::string_view value) {
    std::wstring result;
    for (wchar_t ch : fromUtf8(value)) { result.push_back(ch); if (ch == L'&') result.push_back(ch); }
    return result;
}

struct Prompt {
    HWND edit{};
    std::wstring value;
    bool done = false, accepted = false;
};

LRESULT CALLBACK promptProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* data = reinterpret_cast<Prompt*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        data = static_cast<Prompt*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
    }
    if (message == WM_COMMAND && (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)) {
        if (LOWORD(wParam) == IDOK) {
            const int length = GetWindowTextLengthW(data->edit);
            std::wstring value(static_cast<size_t>(length) + 1, L'\0');
            GetWindowTextW(data->edit, value.data(), length + 1);
            value.resize(length);
            const auto start = value.find_first_not_of(L" \r\n\t");
            if (start != std::wstring::npos) {
                data->value = value.substr(start, value.find_last_not_of(L" \r\n\t") - start + 1);
                data->accepted = true;
            }
        }
        data->done = true;
        return 0;
    }
    if (message == WM_CLOSE) { data->done = true; return 0; }
    return DefWindowProcW(window, message, wParam, lParam);
}

std::wstring promptUrl(HWND parent, HINSTANCE instance) {
    WNDCLASSW type{};
    type.lpfnWndProc = promptProc;
    type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hbrBackground = GetSysColorBrush(COLOR_3DFACE);
    type.lpszClassName = L"Velin.UrlPrompt";
    RegisterClassW(&type);
    const float scale = static_cast<float>(GetDpiForWindow(parent)) / 96.f;
    const auto px = [scale](int value) { return static_cast<int>(static_cast<float>(value) * scale); };
    RECT bounds{}; GetWindowRect(parent, &bounds);
    Prompt prompt;
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, type.lpszClassName, L"Open URL",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, bounds.left + ((bounds.right-bounds.left)-px(510))/2,
        bounds.top + ((bounds.bottom-bounds.top)-px(190))/2, px(510), px(190), parent, nullptr, instance, &prompt);
    if (!window) return {};
    auto makeControl = [&](const wchar_t* cls, const wchar_t* title, DWORD style, int x, int y, int w, int h, int id) {
        HWND control = CreateWindowExW(cls == std::wstring_view(L"EDIT") ? WS_EX_CLIENTEDGE : 0,
            cls, title, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(dialogFont(window)), TRUE);
        return control;
    };
    makeControl(L"STATIC", L"Enter an HTTP, HTTPS, or other media stream URL:", 0, 20, 18, 465, 24, 0);
    prompt.edit = makeControl(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 20, 50, 465, 28, 100);
    makeControl(L"BUTTON", L"Open", WS_TABSTOP | BS_DEFPUSHBUTTON, 300, 99, 88, 30, IDOK);
    makeControl(L"BUTTON", L"Cancel", WS_TABSTOP, 398, 99, 88, 30, IDCANCEL);
    EnableWindow(parent, FALSE);
    ShowWindow(window, SW_SHOW);
    SetFocus(prompt.edit);
    MSG message{};
    bool quit = false;
    while (!prompt.done) {
        const int result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) { quit = result == 0; break; }
        if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN) {
            SendMessageW(window, WM_COMMAND, IDOK, 0); continue;
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            SendMessageW(window, WM_COMMAND, IDCANCEL, 0); continue;
        }
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    EnableWindow(parent, TRUE);
    DestroyWindow(window);
    SetForegroundWindow(parent);
    if (quit) PostQuitMessage(static_cast<int>(message.wParam));
    return prompt.accepted ? prompt.value : std::wstring{};
}
} // namespace

PlayerWindow::PlayerWindow(HINSTANCE instance) : instance_(instance), settings_(settingsDirectory()), preferences_(settings_.load()),
    history_(settingsDirectory()),playlists_(settingsDirectory()),dataDirectory_(settingsDirectory()) {
    showPlaylist_=preferences_.showPlaylist; musicMode_=preferences_.musicMode;
}

PlayerWindow::~PlayerWindow() {
    if (ocrWorker_.joinable()) ocrWorker_.join();
    thumbnails_.cancel();
    webviews_.clear(); plugins_.reset(); online_.reset(); accessibility_.shutdown();
    engine_.shutdown();
    for (auto& [key,format]:textFormats_) { (void)key; format->Release(); }
    discardTarget();
    releaseLayers();
    release(roundStroke_);
    release(writeFactory_);
    release(factory_);
}

int PlayerWindow::run(int showCommand, const std::vector<std::string>& paths) {
    playerLabel_=std::to_string(GetCurrentProcessId());
    for(const auto& argument:paths) {
        if(argument.starts_with("--plugin-parent=")) { try { pluginParent_=static_cast<DWORD>(std::stoul(argument.substr(16))); } catch(...) {} }
        else if(argument.starts_with("--plugin-owner=")) pluginOwner_=argument.substr(15);
        else if(argument.starts_with("--plugin-secret=")) pluginSecret_=argument.substr(16);
        else if(argument.starts_with("--plugin-label=")) playerLabel_=argument.substr(15);
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSEXW type{};
    type.cbSize=sizeof(type);
    type.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    type.lpfnWndProc = windowProc;
    type.hInstance = instance_;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(101));
    if (!type.hIcon) type.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    type.hIconSm = type.hIcon;
    type.lpszClassName = L"Velin.Player";
    RegisterClassExW(&type);
    WNDCLASSW videoType{};
    videoType.style = CS_DBLCLKS;
    videoType.lpfnWndProc = videoProc;
    videoType.hInstance = instance_;
    videoType.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    videoType.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    videoType.lpszClassName = L"Velin.Video";
    RegisterClassW(&videoType);
    WNDCLASSW overlayType{};
    overlayType.lpfnWndProc = overlayProc;
    overlayType.hInstance = instance_;
    overlayType.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    overlayType.lpszClassName = L"Velin.Overlay";
    RegisterClassW(&overlayType);
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory_)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                  reinterpret_cast<IUnknown**>(&writeFactory_))))
        throw std::runtime_error("Direct2D initialization failed. Update your Windows graphics driver.");
    dpi_ = static_cast<float>(GetDpiForSystem()) / 96.f;
    RECT size{0, 0, static_cast<LONG>(static_cast<float>(musicMode_?860:preferences_.windowWidth) * dpi_),
                    static_cast<LONG>(static_cast<float>(musicMode_?560:preferences_.windowHeight) * dpi_)};
    AdjustWindowRectExForDpi(&size, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForSystem());
    MONITORINFO monitor{}; monitor.cbSize=sizeof(monitor);
    const bool restorePosition=preferences_.rememberWindowPosition && (preferences_.windowX!=-1 || preferences_.windowY!=-1);
    GetMonitorInfoW(MonitorFromPoint(restorePosition?POINT{preferences_.windowX,preferences_.windowY}:POINT{0,0},MONITOR_DEFAULTTONEAREST),&monitor);
    const int windowWidth=std::min<int>(size.right-size.left,monitor.rcWork.right-monitor.rcWork.left);
    const int windowHeight=std::min<int>(size.bottom-size.top,monitor.rcWork.bottom-monitor.rcWork.top);
    window_ = CreateWindowExW(0, type.lpszClassName, L"Velin", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        restorePosition?std::clamp(preferences_.windowX,static_cast<int>(monitor.rcWork.left),static_cast<int>(monitor.rcWork.right)-windowWidth):monitor.rcWork.left+(monitor.rcWork.right-monitor.rcWork.left-windowWidth)/2,
        restorePosition?std::clamp(preferences_.windowY,static_cast<int>(monitor.rcWork.top),static_cast<int>(monitor.rcWork.bottom)-windowHeight):monitor.rcWork.top+(monitor.rcWork.bottom-monitor.rcWork.top-windowHeight)/2,
        windowWidth, windowHeight, nullptr, nullptr, instance_, this);
    if (!window_) throw std::runtime_error("Cannot create the player window.");
    BOOL dark = TRUE;
    DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark));
    COLORREF captionColor = RGB(12,13,17);
    DwmSetWindowAttribute(window_, 35, &captionColor, sizeof(captionColor));
    video_ = CreateWindowExW(0, videoType.lpszClassName, L"Video", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0, 0, 1, 1, window_, nullptr, instance_, this);
    DragAcceptFiles(window_, TRUE);
    initializeEngine();
    platform_.initialize(window_,[this](PlatformCommand command,double value) {
        if (closing_) return;
        switch(command) {
        case PlatformCommand::Play: engine_.setProperty("pause","no"); break;
        case PlatformCommand::Pause: engine_.setProperty("pause","yes"); break;
        case PlatformCommand::TogglePause: execute(TogglePause); break;
        case PlatformCommand::Stop: execute(Stop); break;
        case PlatformCommand::Previous: execute(Previous); break;
        case PlatformCommand::Next: execute(Next); break;
        case PlatformCommand::Seek: engine_.seek(value,true); break;
        }
    });
    accessibility_.initialize(window_,[this](int id,double value) {
        if (id<=0 || static_cast<size_t>(id)>hits_.size()) return;
        const auto hit=hits_[static_cast<size_t>(id-1)];
        if (hit.action==SeekBar) engine_.seek(value,true);
        else if (hit.action==VolumeBar) engine_.setVolume(value);
        else execute(hit.action,hit.data);
    },[this](int id) { hovered_=id-1; InvalidateRect(window_,nullptr,FALSE); });
    thumbnails_.initialize(window_,thumbnailMessage,dataDirectory_/L"Thumbnails");
    online_=std::make_unique<OnlineServices>();
    ExtensionCallbacks callbacks;
    callbacks.log=[this](const std::string& plugin,const std::string& text) {
        logLines_.push_back("["+plugin+"] "+text);
        if (logLines_.size()>5000) logLines_.erase(logLines_.begin(),logLines_.begin()+500);
    };
    callbacks.ui=[this](const auto& metadata,const auto& operation,const auto& arguments) { return extensionUi(metadata,operation,arguments); };
    callbacks.menusChanged=[this] { if (plugins_) pluginMenus_=plugins_->menuItems(); InvalidateRect(window_,nullptr,FALSE); };
    plugins_=std::make_unique<PluginHost>(dataDirectory_,engine_,*online_,std::move(callbacks),std::filesystem::path(fromUtf8(preferences_.pluginsDirectory)));
    plugins_->setGlobalEntriesEnabled(pluginParent_==0);
    if (preferences_.pluginsEnabled) {
        try { plugins_->reload(); } catch (const std::exception& error) { notice(fromUtf8(error.what()),true); }
    }
    applyPreferences(preferences_);
    if (preferences_.alwaysOnTop)
        SetWindowPos(window_, HWND_TOPMOST, 0,0,0,0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(window_, showCommand);
    UpdateWindow(window_);
    SetTimer(window_, 1, 100, nullptr);
    if (!paths.empty()) {
        std::vector<std::wstring> arguments; for (const auto& path:paths) arguments.push_back(fromUtf8(path));
        acceptLaunch(arguments);
    } else if (preferences_.launchAction==LaunchAction::OpenFiles) showFileDialog(false);
    else restoreSession();
    if (preferences_.checkForUpdates && !preferences_.updateManifestUrl.empty()) execute(CheckUpdates);
    if (plugins_) plugins_->emit("iina.window-loaded");
    MSG message{};
    int mouseClickCounts[3]{1,1,1};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if ((message.message == WM_KEYDOWN || message.message == WM_KEYUP) && (message.hwnd == window_ || IsChild(window_, message.hwnd))) {
            keyDown(message.wParam,message.message==WM_KEYUP);
            continue;
        }
        if (plugins_ && (message.hwnd==window_ || message.hwnd==video_ || (isLayerWindow(message.hwnd) && IsWindowEnabled(window_)))) {
            const auto kind=message.message;
            const bool doubleClick=kind==WM_LBUTTONDBLCLK || kind==WM_RBUTTONDBLCLK || kind==WM_MBUTTONDBLCLK;
            const bool down=kind==WM_LBUTTONDOWN || kind==WM_RBUTTONDOWN || kind==WM_MBUTTONDOWN || doubleClick;
            const bool up=kind==WM_LBUTTONUP || kind==WM_RBUTTONUP || kind==WM_MBUTTONUP;
            const bool drag=kind==WM_MOUSEMOVE && (message.wParam&(MK_LBUTTON|MK_RBUTTON|MK_MBUTTON));
            if (down || up || drag) {
                const int button=(kind==WM_RBUTTONDOWN || kind==WM_RBUTTONUP || kind==WM_RBUTTONDBLCLK || (drag && (message.wParam&MK_RBUTTON)))?1:
                    (kind==WM_MBUTTONDOWN || kind==WM_MBUTTONUP || kind==WM_MBUTTONDBLCLK || (drag && (message.wParam&MK_MBUTTON)))?2:0;
                const auto key=button==1?"MBTN_RIGHT":button==2?"MBTN_MID":"MBTN_LEFT";
                if (down) mouseClickCounts[button]=doubleClick?2:1;
                POINT point{GET_X_LPARAM(message.lParam),GET_Y_LPARAM(message.lParam)};
                MapWindowPoints(message.hwnd,window_,&point,1);
                RECT client{}; GetClientRect(window_,&client);
                const double scale=GetDpiForWindow(window_)/96.0;
                Json args={{"x",point.x/scale},{"y",(client.bottom-point.y)/scale},
                    {"clickCount",mouseClickCounts[button]},{"pressure",up?0.0:1.0}};
                if (plugins_->input(down?"mouseDown":up?"mouseUp":"mouseDrag",key,args)) continue;
            }
        }
        if (message.message == WM_SYSKEYDOWN && message.wParam == VK_RETURN) { toggleFullscreen(); continue; }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK PlayerWindow::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<PlayerWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<PlayerWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK PlayerWindow::videoProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* self = static_cast<PlayerWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<PlayerWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self) {
        switch (message) {
        case WM_MOUSEMOVE: self->pointerMoved(); break;
        case WM_LBUTTONUP: self->execute(TogglePause); return 0;
        case WM_LBUTTONDBLCLK: self->toggleFullscreen(); return 0;
        case WM_RBUTTONUP: {
            POINT point{}; GetCursorPos(&point); self->showMenu(PlaybackMenu, point); return 0;
        }
        case WM_MOUSEWHEEL: return SendMessageW(self->window_, message, wParam, lParam);
        default: break;
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void PlayerWindow::createResources() {
    if (target_) return;
    RECT bounds{}; GetClientRect(window_, &bounds);
    if (FAILED(factory_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(window_, D2D1::SizeU(bounds.right, bounds.bottom)), &target_))) return;
    target_->SetDpi(dpi_ * 96.f, dpi_ * 96.f);
    if (FAILED(target_->CreateSolidColorBrush(ui.text, &brush_))) { discardTarget(); return; }
    canvas_ = target_; paint_ = brush_;
    if (!roundStroke_) {
        const auto properties = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        factory_->CreateStrokeStyle(&properties, nullptr, 0, &roundStroke_);
    }
}

void PlayerWindow::discardTarget() { release(brush_); release(target_); canvas_ = nullptr; paint_ = nullptr; }

void PlayerWindow::fill(D2D1_RECT_F bounds, D2D1_COLOR_F color, float radius) {
    paint_->SetColor(color);
    if (radius > 0) canvas_->FillRoundedRectangle(D2D1::RoundedRect(bounds, radius, radius), paint_);
    else canvas_->FillRectangle(bounds, paint_);
}

void PlayerWindow::outline(D2D1_RECT_F bounds, D2D1_COLOR_F color, float radius, float width) {
    // Inset by half the stroke so hairlines sit inside the shape's edge.
    const float inset = width/2;
    const D2D1_RECT_F inner{bounds.left+inset, bounds.top+inset, bounds.right-inset, bounds.bottom-inset};
    paint_->SetColor(color);
    if (radius > 0) canvas_->DrawRoundedRectangle(D2D1::RoundedRect(inner, radius-inset, radius-inset), paint_, width);
    else canvas_->DrawRectangle(inner, paint_, width);
}

void PlayerWindow::gradient(D2D1_RECT_F bounds, D2D1_COLOR_F from, D2D1_COLOR_F to, float radius, bool horizontal) {
    const D2D1_GRADIENT_STOP stops[]{{0.f, from}, {1.f, to}};
    ID2D1GradientStopCollection* collection = nullptr;
    ID2D1LinearGradientBrush* brush = nullptr;
    const auto properties = D2D1::LinearGradientBrushProperties(D2D1::Point2F(bounds.left, bounds.top),
        horizontal ? D2D1::Point2F(bounds.right, bounds.top) : D2D1::Point2F(bounds.left, bounds.bottom));
    if (SUCCEEDED(canvas_->CreateGradientStopCollection(stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &collection)) &&
        SUCCEEDED(canvas_->CreateLinearGradientBrush(&properties, nullptr, collection, &brush))) {
        if (radius > 0) canvas_->FillRoundedRectangle(D2D1::RoundedRect(bounds, radius, radius), brush);
        else canvas_->FillRectangle(bounds, brush);
    } else fill(bounds, to, radius);
    release(brush); release(collection);
}

void PlayerWindow::circle(float cx, float cy, float radius, D2D1_COLOR_F color) {
    paint_->SetColor(color);
    canvas_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), paint_);
}

void PlayerWindow::line(float x1, float y1, float x2, float y2, D2D1_COLOR_F color, float thickness) {
    paint_->SetColor(color);
    canvas_->DrawLine(D2D1::Point2F(x1,y1), D2D1::Point2F(x2,y2), paint_, thickness, roundStroke_);
}

IDWriteTextFormat* PlayerWindow::textFormat(float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT alignment) {
    if (textFamily_.empty()) {
        // Prefer the Windows 11 optical-size families; Segoe UI everywhere else.
        textFamily_ = displayFamily_ = L"Segoe UI";
        IDWriteFontCollection* fonts = nullptr;
        if (SUCCEEDED(writeFactory_->GetSystemFontCollection(&fonts))) {
            UINT32 index = 0; BOOL exists = FALSE;
            if (SUCCEEDED(fonts->FindFamilyName(L"Segoe UI Variable Text", &index, &exists)) && exists) textFamily_ = L"Segoe UI Variable Text";
            exists = FALSE;
            if (SUCCEEDED(fonts->FindFamilyName(L"Segoe UI Variable Display", &index, &exists)) && exists) displayFamily_ = L"Segoe UI Variable Display";
            fonts->Release();
        }
    }
    const auto key = std::make_tuple(static_cast<int>(size*100), static_cast<int>(weight), static_cast<int>(alignment));
    if (auto existing = textFormats_.find(key); existing != textFormats_.end()) return existing->second;
    IDWriteTextFormat* format = nullptr;
    if (FAILED(writeFactory_->CreateTextFormat((size >= 20 ? displayFamily_ : textFamily_).c_str(), nullptr, weight,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-US", &format))) return nullptr;
    format->SetTextAlignment(alignment);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    IDWriteInlineObject* ellipsis = nullptr;
    if (SUCCEEDED(writeFactory_->CreateEllipsisTrimmingSign(format, &ellipsis))) {
        format->SetTrimming(&trimming, ellipsis); ellipsis->Release();
    }
    textFormats_.emplace(key, format);
    return format;
}

float PlayerWindow::textWidth(std::wstring_view value, float size, DWRITE_FONT_WEIGHT weight) {
    auto* format = textFormat(size, weight, DWRITE_TEXT_ALIGNMENT_LEADING);
    IDWriteTextLayout* layout = nullptr;
    if (!format || FAILED(writeFactory_->CreateTextLayout(value.data(), static_cast<UINT32>(value.size()), format, 4096, 64, &layout))) return 0;
    DWRITE_TEXT_METRICS metrics{};
    const float width = SUCCEEDED(layout->GetMetrics(&metrics)) ? metrics.widthIncludingTrailingWhitespace : 0.f;
    layout->Release();
    return width;
}

void PlayerWindow::text(std::wstring_view value, D2D1_RECT_F bounds, float size,
                        D2D1_COLOR_F color, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT alignment) {
    auto* format = textFormat(size, weight, alignment);
    if (!format) return;
    paint_->SetColor(color);
    canvas_->DrawText(value.data(), static_cast<UINT32>(value.size()), format, bounds, paint_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void PlayerWindow::applyTheme() {
    bool light=preferences_.theme==Theme::Light;
    if (preferences_.theme==Theme::System) {
        DWORD value=0,size=sizeof(value);
        if (RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",L"AppsUseLightTheme",RRF_RT_REG_DWORD,nullptr,&value,&size)==ERROR_SUCCESS) light=value!=0;
    }
    ui=light?lightTheme:darkTheme;
    lightAppearance=light;
    BOOL dark=!light; DwmSetWindowAttribute(window_,20,&dark,sizeof(dark));
    COLORREF captionText=light?RGB(29,29,31):RGB(161,161,166); DwmSetWindowAttribute(window_,36,&captionText,sizeof(captionText));
    captionColor_=CLR_INVALID;  // draw() picks the caption colour for the current content.
    InvalidateRect(window_,nullptr,FALSE);
}

void PlayerWindow::icon(int action, D2D1_RECT_F bounds, D2D1_COLOR_F color) {
    // Glyphs are drawn on a 24-unit grid around the centre with round caps and
    // joins, so every icon shares one stroke weight and corner softness.
    const float cx = (bounds.left+bounds.right)/2, cy = (bounds.top+bounds.bottom)/2;
    const auto& state = engine_.state();
    const auto at = [&](float x, float y) { return D2D1::Point2F(cx+x, cy+y); };
    const auto stroke = [&](float x1, float y1, float x2, float y2, float width = 1.6f) { line(cx+x1,cy+y1,cx+x2,cy+y2,color,width); };
    const auto shape = [&](std::initializer_list<D2D1_POINT_2F> points, bool closed, bool filled, float width = 1.6f) {
        ID2D1PathGeometry* geometry = nullptr; ID2D1GeometrySink* sink = nullptr;
        if (points.size() < 2 || FAILED(factory_->CreatePathGeometry(&geometry))) return;
        if (SUCCEEDED(geometry->Open(&sink))) {
            auto point = points.begin();
            sink->BeginFigure(at(point->x, point->y), filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
            for (++point; point != points.end(); ++point) sink->AddLine(at(point->x, point->y));
            sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
            sink->Close(); sink->Release();
            paint_->SetColor(color);
            if (filled) canvas_->FillGeometry(geometry, paint_);
            canvas_->DrawGeometry(geometry, paint_, width, roundStroke_);
        }
        geometry->Release();
    };
    const auto arc = [&](float x1, float y1, float x2, float y2, float radius) {
        ID2D1PathGeometry* geometry = nullptr; ID2D1GeometrySink* sink = nullptr;
        if (FAILED(factory_->CreatePathGeometry(&geometry))) return;
        if (SUCCEEDED(geometry->Open(&sink))) {
            sink->BeginFigure(at(x1, y1), D2D1_FIGURE_BEGIN_HOLLOW);
            const auto segment = D2D1::ArcSegment(at(x2, y2), D2D1::SizeF(radius, radius), 0,
                D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL);
            sink->AddArc(&segment);
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close(); sink->Release();
            paint_->SetColor(color);
            canvas_->DrawGeometry(geometry, paint_, 1.6f, roundStroke_);
        }
        geometry->Release();
    };
    const auto frame = [&](float x, float y, float w, float h, float radius) {
        paint_->SetColor(color);
        canvas_->DrawRoundedRectangle(D2D1::RoundedRect(rect(cx+x, cy+y, w, h), radius, radius), paint_, 1.6f);
    };
    const auto ring = [&](float x, float y, float rx, float ry) {
        paint_->SetColor(color);
        canvas_->DrawEllipse(D2D1::Ellipse(at(x, y), rx, ry), paint_, 1.6f);
    };
    switch(action) {
    case TogglePause: {
        // The primary transport glyph is drawn larger, as in IINA's controller.
        const float s = bounds.bottom-bounds.top >= 38 ? 1.3f : 1.f;
        if (!state.idle && !state.paused) {
            fill(rect(cx-5.6f*s, cy-7.5f*s, 3.8f*s, 15*s), color, 1.4f);
            fill(rect(cx+1.8f*s, cy-7.5f*s, 3.8f*s, 15*s), color, 1.4f);
        } else shape({{-3.5f*s,-7.5f*s},{9*s,0},{-3.5f*s,7.5f*s}}, true, true, 2.f);
        break;
    }
    case RecentItem: shape({{-2.5f,-4.5f},{5,0},{-2.5f,4.5f}}, true, true, 1.4f); break;
    case Previous: stroke(-7,-7,-7,7,2.2f); shape({{7,-6.5f},{7,6.5f},{-3,0}}, true, true); break;
    case Next: stroke(7,-7,7,7,2.2f); shape({{-7,-6.5f},{-7,6.5f},{3,0}}, true, true); break;
    case ToggleMute:
        shape({{-9,-3.5f},{-5.5f,-3.5f},{-.5f,-8},{-.5f,8},{-5.5f,3.5f},{-9,3.5f}}, true, true);
        if (state.muted || state.volume <= 0) { stroke(4,-3.5f,11,3.5f); stroke(4,3.5f,11,-3.5f); }
        else {
            arc(3.5f,-3.8f,3.5f,3.8f,5.4f);
            if (state.volume >= 50) arc(6.5f,-7.5f,6.5f,7.5f,10.f);
        }
        break;
    case ToggleFullscreen:
        if (!fullscreen_) {
            shape({{-9,-4},{-9,-9},{-4,-9}}, false, false); shape({{4,-9},{9,-9},{9,-4}}, false, false);
            shape({{9,4},{9,9},{4,9}}, false, false); shape({{-4,9},{-9,9},{-9,4}}, false, false);
        } else {
            shape({{-9,-4},{-4,-4},{-4,-9}}, false, false); shape({{4,-9},{4,-4},{9,-4}}, false, false);
            shape({{9,4},{4,4},{4,9}}, false, false); shape({{-4,9},{-4,4},{-9,4}}, false, false);
        }
        break;
    case TogglePlaylist:
        for (int i=-1;i<=1;++i) {
            const float y = static_cast<float>(i)*5.5f;
            stroke(-3.5f,y,9,y); circle(cx-8,cy+y,1.3f,color);
        }
        break;
    case ToggleRepeat:
        stroke(-9,2,-9,-1); arc(-9,-1,-5,-5,4); stroke(-5,-5,8,-5); shape({{5,-8},{8,-5},{5,-2}}, false, false);
        stroke(9,-2,9,1); arc(9,1,5,5,4); stroke(5,5,-8,5); shape({{-5,2},{-8,5},{-5,8}}, false, false);
        break;
    case Screenshot:
        frame(-10,-5.5f,20,14,3.5f);
        shape({{-4,-5.5f},{-2.5f,-8.5f},{2.5f,-8.5f},{4,-5.5f}}, false, false);
        ring(0,1.5f,3.6f,3.6f);
        break;
    case SubtitleMenu:
        frame(-10.5f,-7.5f,21,15,3.5f);
        stroke(-6,-1,0,-1); stroke(3,-1,6,-1); stroke(-6,3,-3,3); stroke(0,3,6,3);
        break;
    case PreferencesAction:
        stroke(-9,-5,-5.5f,-5); ring(-3,-5,2.5f,2.5f); stroke(-.5f,-5,9,-5);
        stroke(-9,5,1.5f,5); ring(4,5,2.5f,2.5f); stroke(6.5f,5,9,5);
        break;
    case PictureInPicture:
        frame(-10,-7.5f,20,15,3);
        fill(rect(cx+.5f,cy+.5f,7,4.5f),color,1.2f);
        break;
    case AddFiles: stroke(-6,0,6,0,1.8f); stroke(0,-6,0,6,1.8f); break;
    case OpenFiles:
        shape({{-6,-9},{2,-9},{7,-4},{7,9},{-6,9}}, true, false);
        shape({{2,-9},{2,-4},{7,-4}}, false, false);
        break;
    case OpenUrl:
        ring(0,0,8.5f,8.5f); ring(0,0,3.6f,8.5f); stroke(-8.5f,0,8.5f,0);
        break;
    case OpenFolder:
        shape({{-10,-7},{-4,-7},{-2,-4.5f},{10,-4.5f},{10,7.5f},{-10,7.5f}}, true, false); break;
    default:
        for (int i=-1;i<=1;++i) circle(cx+static_cast<float>(i)*6,cy,1.6f,color);
        break;
    }
}

void PlayerWindow::button(int action, D2D1_RECT_F bounds, std::wstring_view label,
                          std::wstring hint, bool primary, int data) {
    const bool hover = hovered_ == static_cast<int>(hits_.size());
    const float height = bounds.bottom-bounds.top, radius = height/2;
    if (primary) gradient(bounds, hover ? ui.accentHigh : ui.accent, hover ? ui.accent : ui.accentLow, radius);
    else {
        fill(bounds, hover ? ui.pressed : ui.hover, radius);
        outline(bounds, ui.hairline, radius);
    }
    text(label, bounds, height < 30 ? 12.f : 13.f, primary ? ui.onAccent : ui.text, semibold, centered);
    hits_.push_back({bounds,action,data,std::move(hint)});
}

void PlayerWindow::iconButton(int action, D2D1_RECT_F bounds, std::wstring hint, bool active) {
    const bool hover = hovered_ == static_cast<int>(hits_.size());
    const float radius = (bounds.bottom-bounds.top)/2;
    if (active) fill(bounds, ui.accentSoft, radius);
    if (hover) fill(bounds, ui.hover, radius);
    icon(action, bounds, active ? ui.accent : (hover ? ui.text : fade(ui.text, .86f)));
    hits_.push_back({bounds,action,0,std::move(hint)});
}

void PlayerWindow::slider(int action, D2D1_RECT_F track, double fraction, std::wstring hint) {
    // IINA-style bar: a thin track that thickens under the pointer, a bright
    // level and a white knob.
    const bool hot = hovered_ == static_cast<int>(hits_.size()) || dragAction_ == action;
    const float cy = (track.top+track.bottom)/2, thickness = hot ? 5.f : 3.f;
    const auto bar = rect(track.left, cy-thickness/2, track.right-track.left, thickness);
    const float level = (bar.right-bar.left)*static_cast<float>(std::clamp(fraction, 0., 1.));
    fill(bar, ui.track, thickness/2);
    if (level > 0) fill(rect(bar.left, bar.top, std::max(level, thickness), thickness), ui.level, thickness/2);
    const auto& state = engine_.state();
    if (action == SeekBar && preferences_.showChapterMarkers && state.duration > 0) for (const auto& chapter : state.chapters) {
        if (chapter.time > 0 && chapter.time < state.duration) {
            const float x = bar.left+(bar.right-bar.left)*static_cast<float>(chapter.time/state.duration);
            fill(rect(x-.75f, bar.top-1, 1.5f, thickness+2), fade(ui.hud, 1.f));
        }
    }
    if (action != SeekBar || state.duration > 0) {
        const float knob = hot ? 6.5f : 5.f;
        circle(bar.left+level, cy+.5f, knob+1, D2D1::ColorF(0, 0, 0, .28f));
        circle(bar.left+level, cy, knob, ui.knob);
    }
    hits_.push_back({rect(track.left-6, cy-10, track.right-track.left+12, 20), action, 0, std::move(hint)});
}

void PlayerWindow::hud(D2D1_RECT_F bounds, float radius, bool shadow) {
    if (shadow) for (int ring = 1; ring <= 4; ++ring) {
        // Soft falloff built from widening rings; the panel itself stays translucent.
        const float spread = static_cast<float>(ring)*2;
        outline(expand(bounds, spread), D2D1::ColorF(0, 0, 0, .11f-.025f*static_cast<float>(ring)), radius+spread, 2);
    }
    fill(bounds, ui.hud, radius);
    outline(bounds, ui.hudEdge, radius);
}

float PlayerWindow::sidebarWidth() const { const float side=std::clamp(static_cast<float>(preferences_.sidebarWidth),240.f,640.f);return showPlaylist_ && width_>=side+sidebarThreshold?side:0.f; }
bool PlayerWindow::chromeShown() const { return !uiHidden_ || engine_.state().idle; }
bool PlayerWindow::sidebarShown() const {
    return sidebarWidth()>0 && chromeShown() && !pip_ && (!fullscreen_ || engine_.state().idle);
}
D2D1_RECT_F PlayerWindow::sidebarRect() const { return {width_-sidebarWidth(),0,width_,height_}; }
D2D1_RECT_F PlayerWindow::videoRect() const { return {0,0,width_-(sidebarShown()?sidebarWidth():0.f),height_}; }
D2D1_RECT_F PlayerWindow::controlRect() const {
    // The controller floats centred over the video, clear of the sidebar.
    const float right=width_-(sidebarShown()?sidebarWidth():0.f);
    if (pip_) {
        const float w=std::max(120.f,std::min(right-16,300.f));
        return rect((right-w)/2,height_-54,w,44);
    }
    const float w=std::max(120.f,std::min({std::max(right-48,300.f),640.f,right-16}));
    return rect((right-w)/2,height_-(fullscreen_?40.f:20.f)-84,w,84);
}

bool PlayerWindow::isLayerWindow(HWND window) const {
    return window && std::any_of(layers_.begin(),layers_.end(),[window](const Layer& layer){return layer.window==window;});
}

bool PlayerWindow::pointerOverChrome() const {
    POINT point{};
    if (!GetCursorPos(&point)) return false;
    const HWND under=WindowFromPoint(point);
    return std::any_of(layers_.begin(),layers_.end(),[under](const Layer& layer){return layer.shown && layer.window==under;});
}

void PlayerWindow::pointerMoved() {
    // Windows also sends WM_MOUSEMOVE when windows change under a resting
    // pointer (every overlay redraw does), so only real movement counts.
    POINT point{};
    if (!GetCursorPos(&point) || (point.x==lastPointer_.x && point.y==lastPointer_.y)) return;
    lastPointer_=point;
    interactionUntil_=GetTickCount64()+static_cast<ULONGLONG>(preferences_.controlsHideSeconds*1000);
    if (!controlsVisible_) { controlsVisible_=true; layoutVideo(); InvalidateRect(window_,nullptr,FALSE); }
}

bool PlayerWindow::beginLayer(LayerId id, D2D1_RECT_F bounds) {
    auto& layer=layers_[id];
    if (!layer.window) {
        // Notices and tooltips never take input; the other layers forward theirs.
        const DWORD style=WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|(id==NoticeLayer || id==TooltipLayer?WS_EX_TRANSPARENT:0);
        layer.window=CreateWindowExW(style,L"Velin.Overlay",L"",WS_POPUP,0,0,1,1,window_,nullptr,instance_,this);
        if (!layer.window) return false;
    }
    const int left=static_cast<int>(std::floor(bounds.left*dpi_)), top=static_cast<int>(std::floor(bounds.top*dpi_));
    const int width=std::max(1,static_cast<int>(std::ceil(bounds.right*dpi_))-left);
    const int height=std::max(1,static_cast<int>(std::ceil(bounds.bottom*dpi_))-top);
    if (!layer.dc) {
        HDC screen=GetDC(nullptr); layer.dc=CreateCompatibleDC(screen); ReleaseDC(nullptr,screen);
        if (!layer.dc) return false;
    }
    if (!layer.bitmap || width!=layer.width || height!=layer.height) {
        if (layer.bitmap) { SelectObject(layer.dc,layer.previous); DeleteObject(layer.bitmap); layer.bitmap=nullptr; }
        BITMAPINFO info{};
        info.bmiHeader.biSize=sizeof(info.bmiHeader); info.bmiHeader.biWidth=width; info.bmiHeader.biHeight=-height;
        info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
        void* bits=nullptr;
        layer.bitmap=CreateDIBSection(layer.dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
        if (!layer.bitmap) return false;
        layer.previous=SelectObject(layer.dc,layer.bitmap);
        layer.width=width; layer.height=height;
    }
    if (layer.target && layer.dpi!=dpi_) { release(layer.brush); release(layer.target); }
    if (!layer.target) {
        const auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96.f*dpi_,96.f*dpi_);
        if (FAILED(factory_->CreateDCRenderTarget(&properties,&layer.target))) return false;
        if (FAILED(layer.target->CreateSolidColorBrush(ui.text,&layer.brush))) { release(layer.target); return false; }
        // ClearType needs an opaque backdrop; translucent layers use greyscale text.
        layer.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        layer.dpi=dpi_;
    }
    const RECT area{0,0,width,height};
    if (FAILED(layer.target->BindDC(layer.dc,&area))) return false;
    layer.left=left; layer.top=top;
    canvas_=layer.target; paint_=layer.brush;
    layer.target->BeginDraw();
    layer.target->Clear(D2D1::ColorF(0,0,0,0));
    // Draw in client coordinates; the transform maps them into this layer.
    layer.target->SetTransform(D2D1::Matrix3x2F::Translation(-static_cast<float>(left)/dpi_,-static_cast<float>(top)/dpi_));
    return true;
}

void PlayerWindow::endLayer(LayerId id) {
    auto& layer=layers_[id];
    const HRESULT result=layer.target->EndDraw();
    canvas_=target_; paint_=brush_;
    if (FAILED(result)) {
        if (result==D2DERR_RECREATE_TARGET) { release(layer.brush); release(layer.target); }
        return;
    }
    POINT origin{layer.left,layer.top}; ClientToScreen(window_,&origin);
    SIZE size{layer.width,layer.height};
    POINT source{0,0};
    BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    UpdateLayeredWindow(layer.window,nullptr,&origin,&size,layer.dc,&source,0,&blend,ULW_ALPHA);
    if (!layer.shown) { ShowWindow(layer.window,SW_SHOWNOACTIVATE); layer.shown=true; }
}

void PlayerWindow::hideLayer(LayerId id) {
    auto& layer=layers_[id];
    if (layer.shown) { ShowWindow(layer.window,SW_HIDE); layer.shown=false; }
}

void PlayerWindow::moveLayers() {
    for (auto& layer:layers_) {
        if (!layer.shown) continue;
        POINT origin{layer.left,layer.top}; ClientToScreen(window_,&origin);
        SetWindowPos(layer.window,nullptr,origin.x,origin.y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
    }
}

void PlayerWindow::releaseLayers() {
    for (auto& layer:layers_) {
        release(layer.brush); release(layer.target);
        if (layer.bitmap) { SelectObject(layer.dc,layer.previous); DeleteObject(layer.bitmap); layer.bitmap=nullptr; }
        if (layer.dc) { DeleteDC(layer.dc); layer.dc=nullptr; }
        if (layer.window && IsWindow(layer.window)) DestroyWindow(layer.window);
        layer.window=nullptr; layer.shown=false;
    }
}

LRESULT CALLBACK PlayerWindow::overlayProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* self = static_cast<PlayerWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<PlayerWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!self) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_CLOSE: return 0;  // Overlays live and die with the player window.
    default: break;
    }
    // While a modal dialog has disabled the player, its overlays stay inert too.
    if (!IsWindowEnabled(self->window_)) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN:
        if (GetForegroundWindow() != self->window_) SetForegroundWindow(self->window_);
        [[fallthrough]];
    case WM_MOUSEMOVE: case WM_LBUTTONUP: case WM_RBUTTONUP: {
        // Overlay pixels sit exactly over the client area, so the player's own
        // hit testing handles the input once it is in client coordinates.
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        MapWindowPoints(window, self->window_, &point, 1);
        self->pointerWindow_ = window;
        const LRESULT result = self->handleMessage(message, wParam,
            MAKELPARAM(static_cast<WORD>(static_cast<SHORT>(point.x)), static_cast<WORD>(static_cast<SHORT>(point.y))));
        self->pointerWindow_ = nullptr;
        return result;
    }
    case WM_MOUSELEAVE: return self->handleMessage(WM_MOUSELEAVE, 0, 0);
    default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void PlayerWindow::drawTopBar() {
    const auto& state=engine_.state();
    const float right=width_-(sidebarShown()?sidebarWidth():0.f);
    fill(rect(0,0,right,topBarHeight),ui.hud);
    line(0,topBarHeight-.5f,right,topBarHeight-.5f,ui.hudEdge);
    const std::array<std::tuple<int,const wchar_t*,float>,6> menus{{{FileMenu,L"File",44.f},{PlaybackMenu,L"Playback",70.f},
        {VideoMenu,L"Video",52.f},{AudioMenu,L"Audio",52.f},{SubtitleMenu,L"Subtitles",72.f},{ViewMenu,L"View",48.f}}};
    const float limit=right-84;
    float x=8;
    for (const auto& [action,label,w] : menus) {
        if (x+w > limit) break;
        auto bounds=rect(x,6,w,26);
        const bool hover=hovered_ == static_cast<int>(hits_.size());
        if (hover) fill(bounds,ui.hover,6);
        text(label,bounds,12.5f,hover?ui.text:ui.secondary,regular,centered);
        hits_.push_back({bounds,action,0,L"Open menu"}); x += w;
    }
    // Windowed mode shows the title in the native caption; fullscreen needs it here.
    if (fullscreen_ && !state.idle && limit-x > 80)
        text(fromUtf8(state.title.empty()?mediaTitle(state.path):state.title),rect(x+16,0,limit-x-32,topBarHeight),12.5f,ui.secondary,semibold,centered);
    iconButton(PreferencesAction,rect(right-74,4,30,30),L"Preferences");
    iconButton(TogglePlaylist,rect(right-38,4,30,30),L"Playlist / chapters  (P)",showPlaylist_);
}

void PlayerWindow::drawControls() {
    const auto& state=engine_.state();
    const auto box=controlBounds_;
    const float cx=(box.left+box.right)/2, w=box.right-box.left;
    hud(box,pip_?12.f:14.f);
    if (pip_) {
        const float cy=(box.top+box.bottom)/2;
        iconButton(TogglePause,rect(cx-18,cy-18,36,36),L"Play / pause  (Space)");
        iconButton(Previous,rect(cx-56,cy-15,30,30),L"Previous item");
        iconButton(Next,rect(cx+26,cy-15,30,30),L"Next item");
        iconButton(PictureInPicture,rect(box.right-38,cy-15,30,30),L"Leave picture in picture  (Esc)");
        iconButton(ToggleMute,rect(box.left+8,cy-15,30,30),L"Mute  (M)",state.muted);
        return;
    }
    // Row one: volume on the left, transport in the centre, tools on the right.
    const float row=box.top+30;
    iconButton(ToggleMute,rect(box.left+12,row-16,32,32),L"Mute  (M)",state.muted);
    if (w>=480) {
        volumeTrack_=rect(box.left+50,row-1.5f,72,3);
        slider(VolumeBar,volumeTrack_,state.volume/100,L"Volume  (Up / Down)");
    }
    iconButton(Previous,rect(cx-66,row-16,32,32),L"Previous item  (Shift+Left)");
    iconButton(TogglePause,rect(cx-20,row-20,40,40),state.paused?L"Play  (Space)":L"Pause  (Space)");
    iconButton(Next,rect(cx+34,row-16,32,32),L"Next item  (Shift+Right)");
    float x=box.right-44;
    iconButton(ToggleFullscreen,rect(x,row-16,32,32),L"Fullscreen  (F)",fullscreen_); x-=34;
    if (w>=520) { iconButton(Screenshot,rect(x,row-16,32,32),L"Save screenshot  (S)"); x-=34; }
    iconButton(SubtitleMenu,rect(x,row-16,32,32),L"Subtitles  (J to cycle)"); x-=34;
    if (w>=380) { iconButton(ToggleRepeat,rect(x,row-16,32,32),L"Repeat current file  (R)",repeating_); x-=34; }
    if (w>=600) button(SpeedMenu,rect(x-12,row-12,44,24),number(state.speed)+L"\u00d7",L"Playback speed  ([ / ])");
    // Row two: elapsed time, the timeline and the duration.
    const float seekY=box.bottom-20;
    const double position=dragAction_==SeekBar?dragPosition_:state.position;
    text(fromUtf8(formatTime(position)),rect(box.left+14,seekY-10,56,20),11,ui.secondary);
    text(fromUtf8(formatTime(state.duration)),rect(box.right-70,seekY-10,56,20),11,ui.secondary,regular,trailing);
    seekTrack_=rect(box.left+74,seekY-1.5f,w-148,3);
    slider(SeekBar,seekTrack_,state.duration>0?position/state.duration:0.,L"Seek in media");
}

void PlayerWindow::drawSidebar() {
    const auto& state=engine_.state();
    const auto panel=sidebarRect();
    const float x=panel.left, w=panel.right-panel.left;
    fill(panel,ui.base);
    fill(panel,ui.hud);
    line(x+.5f,0,x+.5f,height_,ui.hudEdge);
    // Text tabs with an accent underline, as in IINA's playlist sidebar.
    auto tab=[&](int action,int index,const wchar_t* label,bool active) {
        const float half=(w-24)/2;
        auto bounds=rect(x+12+half*static_cast<float>(index),0,half,topBarHeight);
        const bool hover=hovered_==static_cast<int>(hits_.size());
        text(label,bounds,11,active?ui.text:(hover?ui.secondary:ui.tertiary),semibold,centered);
        if (active) fill(rect(bounds.left+half/2-14,topBarHeight-3.5f,28,2.5f),ui.accent,1.25f);
        hits_.push_back({bounds,action,0,L"Switch sidebar"});
    };
    tab(PlaylistTab,0,L"PLAYLIST",!chapters_); tab(ChaptersTab,1,L"CHAPTERS",chapters_);
    line(x,topBarHeight-.5f,panel.right,topBarHeight-.5f,ui.hairline);
    const float listTop=topBarHeight+6, listBottom=height_-46, pitch=48;
    const int count=static_cast<int>(chapters_?state.chapters.size():state.playlist.size());
    const int visible=std::max(1,static_cast<int>((listBottom-listTop)/pitch));
    listOffset_=std::clamp(listOffset_,0,std::max(0,count-visible));
    if (count==0) {
        const float y=listTop+std::max(8.f,(listBottom-listTop-120)/2);
        icon(TogglePlaylist,rect(x+w/2-22,y,44,44),ui.tertiary);
        text(chapters_?L"No chapters in this file":L"A good place for what's next.",rect(x+16,y+52,w-32,22),13,ui.secondary,regular,centered);
        if (y+100<listBottom) text(chapters_?L"Embedded chapters appear here.":L"Add a few files to make a playlist.",rect(x+16,y+76,w-32,20),11.5f,ui.tertiary,regular,centered);
    } else {
        const double seconds=static_cast<double>(GetTickCount64())/1000.0;
        for (int row=0;row<visible && row+listOffset_<count;++row) {
            const int index=row+listOffset_;
            const float y=listTop+static_cast<float>(row)*pitch;
            auto bounds=rect(x+6,y,w-12,44);
            bool current=false;
            std::wstring title, detail;
            if (chapters_) {
                const auto& chapter=state.chapters[static_cast<size_t>(index)];
                title=fromUtf8(chapter.title.empty()?"Chapter "+std::to_string(index+1):chapter.title);
                detail=fromUtf8(formatTime(chapter.time));
                current=state.position>=chapter.time && (index+1==count || state.position<state.chapters[static_cast<size_t>(index+1)].time);
            } else {
                const auto& item=state.playlist[static_cast<size_t>(index)];
                title=fromUtf8(item.title.empty()?mediaTitle(item.filename):item.title);
                detail=item.current ? (state.paused?L"Paused":L"Now playing") : L"Ready to play";
                current=item.current;
            }
            const bool hover=hovered_==static_cast<int>(hits_.size());
            if (current) fill(bounds,ui.accentSoft,8);
            if (hover) fill(bounds,ui.hover,8);
            const float rowCenter=y+22;
            if (current && !chapters_) {
                // Live level bars mark the playing item; they settle when paused.
                for (int bar=0;bar<3;++bar) {
                    const float level=state.paused ? (bar==1?.55f:.35f)
                        : static_cast<float>(.3+.7*(.5+.5*std::sin(seconds*(5.2+bar*1.3)+bar*1.7)));
                    const float h=4+9*level;
                    fill(rect(x+17.5f+static_cast<float>(bar)*4.f,rowCenter+6.5f-h,2.4f,h),ui.accent,1.2f);
                }
            } else text(std::to_wstring(index+1),rect(x+10,y,30,44),11,current?ui.accent:ui.tertiary,current?semibold:regular,centered);
            text(title,rect(x+44,y+4,w-60,20),13,current||hover?ui.text:ui.secondary,current?semibold:regular);
            text(detail,rect(x+44,y+23,w-60,16),11,current?ui.accent:ui.tertiary);
            hits_.push_back({bounds,chapters_?ChapterItem:PlayItem,index,chapters_?L"Seek to chapter":L"Play this item; right-click for playlist actions"});
        }
        if (count>visible) {
            const float trackHeight=static_cast<float>(visible)*pitch-4;
            const float thumb=std::max(24.f,trackHeight*static_cast<float>(visible)/static_cast<float>(count));
            const float thumbTop=listTop+(trackHeight-thumb)*static_cast<float>(listOffset_)/static_cast<float>(std::max(1,count-visible));
            fill(rect(panel.right-5,thumbTop,3,thumb),ui.track,1.5f);
        }
    }
    // Bottom toolbar.
    line(x,height_-40.5f,panel.right,height_-40.5f,ui.hairline);
    iconButton(AddFiles,rect(x+8,height_-36,32,32),L"Add files to playlist");
    text(std::to_wstring(count)+(chapters_?L" chapters":L" items"),rect(x+46,height_-40,w/2-46,40),11,ui.tertiary);
    if (count>visible) text(L"Scroll for more",rect(x+w/2,height_-40,w/2-14,40),11,ui.tertiary,regular,trailing);
}

void PlayerWindow::drawWelcome(D2D1_RECT_F area) {
    // Modelled on IINA's welcome window: identity and actions on the left,
    // recent media on the right.
    const float w=area.right-area.left, h=area.bottom-area.top;
    const bool wide=w>=680 && h>=300;
    const bool compact=h<360;
    const auto& recent=preferences_.recentFiles;
    const float column=wide?std::min(340.f,w*.44f):w;
    const float cx=area.left+column/2;
    const float rowsWidth=std::min(264.f,column-40);
    const float blockHeight=(compact?0.f:104.f)+60+3*38+28;
    float y=area.top+std::max(12.f,(h-blockHeight-(wide||recent.empty()||compact?0.f:150.f))/2);
    if (!compact) {
        // The original Velin "Scan" mark, drawn from the 256-unit geometry of
        // resources/velin.svg: scanlines whose lengths trace a play shape.
        const float scale=84.f/256, originX=cx-42;
        const auto lavender=hex(0x9d8cff), sky=hex(0x2fa7ff);
        for (int i=0;i<9;++i) {
            const float lineY=128+static_cast<float>(i-4)*19, right=42+170*(1-std::abs(lineY-128)/90);
            const float t=static_cast<float>(i)/8;
            const D2D1_COLOR_F tint{lavender.r+(sky.r-lavender.r)*t,lavender.g+(sky.g-lavender.g)*t,lavender.b+(sky.b-lavender.b)*t,1};
            gradient(rect(originX+36*scale,y+(lineY-6)*scale,(right-30)*scale,12*scale),hex(0x4b47e8),tint,6*scale,true);
        }
        y+=104;
    }
    text(L"Velin",rect(area.left+16,y,column-32,34),26,ui.text,semibold,centered);
    text(fromUtf8(std::string("Version ")+VELIN_VERSION),rect(area.left+16,y+34,column-32,20),12,ui.tertiary,regular,centered);
    y+=68;
    struct Entry { int action; const wchar_t* label; const wchar_t* shortcut; const wchar_t* hint; };
    const std::array<Entry,3> entries{{
        {OpenFiles,L"Open Files\u2026",L"Ctrl+O",L"Open media files  (Ctrl+O)"},
        {OpenUrl,L"Open URL\u2026",L"Ctrl+L",L"Open a network stream  (Ctrl+L)"},
        {OpenFolder,L"Open Folder\u2026",L"Ctrl+Shift+O",L"Open a folder  (Ctrl+Shift+O)"}}};
    for (const auto& entry : entries) {
        const auto bounds=rect(cx-rowsWidth/2,y,rowsWidth,34);
        const bool hover=hovered_==static_cast<int>(hits_.size());
        if (hover) fill(bounds,ui.hover,8);
        icon(entry.action,rect(bounds.left+4,bounds.top+3,28,28),ui.accent);
        text(entry.label,rect(bounds.left+38,bounds.top,rowsWidth-120,34),13,ui.text);
        text(entry.shortcut,rect(bounds.right-104,bounds.top,94,34),11.5f,ui.tertiary,regular,trailing);
        hits_.push_back({bounds,entry.action,0,entry.hint});
        y+=38;
    }
    text(L"or drop files or a folder anywhere",rect(area.left+16,y+4,column-32,20),11.5f,ui.tertiary,regular,centered);
    // Runtime status sits quietly at the foot of the identity column.
    const std::wstring status=engine_.ready()?L"libmpv connected":L"Playback runtime missing \u00b7 See File > About for setup";
    const float statusWidth=std::min(column-48,textWidth(status,11)+14);
    const float statusLeft=cx-statusWidth/2;
    if (area.bottom-30>y+30) {
        circle(statusLeft+3,area.bottom-20,3,engine_.ready()?ui.positive:ui.danger);
        text(status,rect(statusLeft+12,area.bottom-30,statusWidth-12,20),11,engine_.ready()?ui.tertiary:ui.danger);
    }
    const auto recentRow=[&](size_t index,D2D1_RECT_F bounds) {
        const bool hover=hovered_==static_cast<int>(hits_.size());
        const float rowCenter=(bounds.top+bounds.bottom)/2;
        if (hover) fill(bounds,ui.hover,8);
        circle(bounds.left+22,rowCenter,13,ui.accentSoft);
        icon(RecentItem,rect(bounds.left+8,rowCenter-14,28,28),ui.accent);
        const auto& path=recent[index];
        std::wstring location=fromUtf8(path);
        if (path.find("://")==std::string::npos) location=std::filesystem::path(location).parent_path().wstring();
        text(fromUtf8(mediaTitle(path)),rect(bounds.left+44,bounds.top+5,bounds.right-bounds.left-56,20),13,ui.text);
        text(location,rect(bounds.left+44,bounds.top+24,bounds.right-bounds.left-56,16),11,ui.tertiary);
        hits_.push_back({bounds,RecentItem,static_cast<int>(index),L"Open recent media"});
    };
    if (wide) {
        const auto panel=rect(area.left+column,area.top+20,w-column-20,h-40);
        fill(panel,ui.panel,12);
        outline(panel,ui.hairline,12);
        text(L"Recent",rect(panel.left+18,panel.top+10,160,28),13,ui.secondary,semibold);
        if (recent.empty()) {
            const float my=(panel.top+panel.bottom)/2;
            text(L"No recent media",rect(panel.left+16,my-22,panel.right-panel.left-32,22),13,ui.secondary,regular,centered);
            text(L"Files you open will appear here.",rect(panel.left+16,my+2,panel.right-panel.left-32,20),11.5f,ui.tertiary,regular,centered);
        }
        for (size_t i=0;i<recent.size();++i) {
            const auto bounds=rect(panel.left+8,panel.top+44+static_cast<float>(i)*50,panel.right-panel.left-16,46);
            if (bounds.bottom>panel.bottom-8) break;
            recentRow(i,bounds);
        }
    } else if (!compact && !recent.empty()) {
        float rowTop=y+40;
        const float listWidth=std::min(420.f,w-48);
        if (rowTop+70<area.bottom-36) text(L"Recent",rect(cx-listWidth/2+8,rowTop,listWidth,20),11.5f,ui.tertiary,semibold);
        rowTop+=24;
        for (size_t i=0;i<std::min<size_t>(3,recent.size());++i) {
            const auto bounds=rect(cx-listWidth/2,rowTop+static_cast<float>(i)*50,listWidth,46);
            if (bounds.bottom>area.bottom-36) break;
            recentRow(i,bounds);
        }
    }
}

void PlayerWindow::drawNowPlaying(D2D1_RECT_F area) {
    // Audio has no picture, so present it like IINA's music mode: a large art
    // tile with the title and stream details beneath.
    const auto& state=engine_.state();
    const float w=area.right-area.left, h=area.bottom-area.top;
    const float art=std::clamp(std::min(w,h)-150,64.f,200.f);
    const float cx=(area.left+area.right)/2;
    const float top=area.top+std::max(12.f,(h-art-76)/2);
    const auto tile=rect(cx-art/2,top,art,art);
    gradient(tile,lightAppearance?hex(0xdcdce0):hex(0x3a3a3e),lightAppearance?hex(0xc8c8ce):hex(0x232326),art*.1f);
    outline(tile,ui.hairline,art*.1f);
    // A pair of beamed notes.
    const float s=art/120;
    const auto ink=fade(ui.text,.5f);
    circle(cx-14*s,top+art*.62f,9*s,ink); circle(cx+18*s,top+art*.56f,9*s,ink);
    line(cx-6*s,top+art*.62f,cx-6*s,top+art*.28f,ink,3.5f*s);
    line(cx+26*s,top+art*.56f,cx+26*s,top+art*.22f,ink,3.5f*s);
    line(cx-6*s,top+art*.28f,cx+26*s,top+art*.22f,ink,6*s);
    text(fromUtf8(state.title.empty()?mediaTitle(state.path):state.title),rect(area.left+24,top+art+16,w-48,30),20,ui.text,semibold,centered);
    std::wstring detail=L"AUDIO  \u00b7  "+fromUtf8(state.audioCodec);
    if (auto artist=state.metadata.find("artist");artist!=state.metadata.end() && !artist->second.empty()) detail=fromUtf8(artist->second)+L"  \u00b7  "+detail;
    text(detail,rect(area.left+24,top+art+48,w-48,22),11.5f,ui.secondary,regular,centered);
}

void PlayerWindow::drawNotice(D2D1_RECT_F bounds) {
    // On-screen display in the top-left corner, as IINA shows its OSD.
    hud(bounds,10);
    float left=bounds.left+16;
    if (noticeError_) { circle(left+3,(bounds.top+bounds.bottom)/2,3.5f,ui.danger); left+=16; }
    text(notice_,rect(left,bounds.top,bounds.right-left-14,bounds.bottom-bounds.top),14,ui.text,semibold);
}

void PlayerWindow::drawTooltip(D2D1_RECT_F bounds, std::wstring_view tip) {
    hud(bounds,6);
    text(tip,bounds,12,ui.text,regular,centered);
}

void PlayerWindow::draw() {
    createResources();
    if (!target_ || !brush_) return;
    const auto& state = engine_.state();
    hits_.clear();
    const bool live = IsWindowVisible(window_) && !IsIconic(window_);
    const bool chrome = live && chromeShown();
    const float contentRight = width_-(sidebarShown()?sidebarWidth():0.f);
    // Overlays render first so their hit targets take precedence over
    // anything drawn on the canvas beneath them.
    const bool controls = chrome && controlsVisible_ && !state.idle;
    if (controls) controlBounds_ = controlRect();
    if (controls && beginLayer(ControlLayer,expand(controlBounds_,layerMargin))) { drawControls(); endLayer(ControlLayer); }
    else hideLayer(ControlLayer);
    const bool topBar = chrome && !pip_ && (state.idle || controlsVisible_);
    if (topBar && beginLayer(TopBarLayer,rect(0,0,contentRight,topBarHeight))) { drawTopBar(); endLayer(TopBarLayer); }
    else hideLayer(TopBarLayer);
    // The canvas: black behind video, the docked sidebar, the welcome screen
    // when idle, and the now-playing view for audio.
    canvas_ = target_; paint_ = brush_;
    target_->BeginDraw();
    const bool picture = !state.idle && state.width > 0;
    target_->Clear(picture ? ui.video : ui.base);
    const COLORREF caption = lightAppearance ? RGB(236,236,236) : picture ? RGB(0,0,0) : RGB(28,28,30);
    if (caption != captionColor_) { DwmSetWindowAttribute(window_,35,&caption,sizeof(caption)); captionColor_ = caption; }
    if (chrome && sidebarShown()) drawSidebar();
    if (chrome && state.idle) drawWelcome(rect(0,topBarHeight,contentRight,height_-topBarHeight));
    else if (chrome && state.loaded && state.width == 0)
        drawNowPlaying({0,topBarHeight,contentRight,controls?controlBounds_.top-8:height_});
    const HRESULT result = target_->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) discardTarget();
    // Notices and tooltips float above everything and never take input.
    if (chrome && GetTickCount64() < noticeUntil_ && !notice_.empty()) {
        const float top = (topBar?topBarHeight:0.f)+14;
        const float w = std::min(std::max(120.f,contentRight-32), textWidth(notice_,14,semibold)+(noticeError_?48.f:32.f));
        if (beginLayer(NoticeLayer,expand(rect(16,top,w,40),layerMargin))) { drawNotice(rect(16,top,w,40)); endLayer(NoticeLayer); }
    } else hideLayer(NoticeLayer);
    std::wstring tip;
    D2D1_RECT_F anchor{};
    if (chrome && dragAction_ == VolumeBar) { tip = L"Volume  "+std::to_wstring(static_cast<int>(state.volume))+L"%"; anchor = volumeTrack_; }
    else if (chrome && dragAction_ == None && hovered_ >= 0 && hovered_ < static_cast<int>(hits_.size())) {
        const auto& hit = hits_[static_cast<size_t>(hovered_)];
        const bool described = hit.action != SeekBar && hit.action != PlayItem && hit.action != ChapterItem &&
            hit.action != RecentItem && hit.action != PlaylistTab && hit.action != ChaptersTab &&
            hit.action != OpenFiles && hit.action != OpenUrl && hit.action != OpenFolder &&
            (hit.action < FileMenu || hit.action > ViewMenu);
        if (hit.action == VolumeBar) tip = L"Volume  "+std::to_wstring(static_cast<int>(state.volume))+L"%  \u00b7  Up / Down";
        else if (described) tip = hit.hint;
        anchor = hit.rect;
    }
    if (!tip.empty()) {
        const float w = textWidth(tip,12)+20, h = 26;
        const float left = std::clamp((anchor.left+anchor.right-w)/2,6.f,std::max(6.f,width_-w-6));
        const float top = anchor.top-h-10 >= 4 ? anchor.top-h-10 : anchor.bottom+10;
        if (beginLayer(TooltipLayer,expand(rect(left,top,w,h),10))) { drawTooltip(rect(left,top,w,h),tip); endLayer(TooltipLayer); }
    } else hideLayer(TooltipLayer);
    updateAccessibility();
}

void PlayerWindow::layoutVideo() {
    if (!video_) return;
    const auto bounds=videoRect();
    RECT current{}; GetWindowRect(video_,&current); MapWindowPoints(HWND_DESKTOP,window_,reinterpret_cast<POINT*>(&current),2);
    const RECT desired{static_cast<LONG>(bounds.left*dpi_),static_cast<LONG>(bounds.top*dpi_),static_cast<LONG>(bounds.right*dpi_),static_cast<LONG>(bounds.bottom*dpi_)};
    if (!EqualRect(&current,&desired)) SetWindowPos(video_,nullptr,static_cast<int>(bounds.left*dpi_),static_cast<int>(bounds.top*dpi_),
        std::max(1,static_cast<int>((bounds.right-bounds.left)*dpi_)),
        std::max(1,static_cast<int>((bounds.bottom-bounds.top)*dpi_)),SWP_NOZORDER | SWP_NOACTIVATE);
    const auto& state=engine_.state();
    const bool visible=engine_.ready() && !state.idle && (!state.loaded || state.width>0);
    if ((IsWindowVisible(video_)!=FALSE)!=visible) ShowWindow(video_,visible?SW_SHOWNA:SW_HIDE);
    updateWebViews();
}

int PlayerWindow::hitAt(float x,float y) const {
    for (size_t i=0;i<hits_.size();++i) {
        const auto& bounds=hits_[i].rect;
        if (x>=bounds.left && x<bounds.right && y>=bounds.top && y<bounds.bottom) return static_cast<int>(i);
    }
    return -1;
}

void PlayerWindow::updatePointer(float x,float, bool commit) {
    if (dragAction_==SeekBar && engine_.state().duration>0) {
        const float span=std::max(1.f,seekTrack_.right-seekTrack_.left);
        dragPosition_=std::clamp(static_cast<double>((x-seekTrack_.left)/span),0.,1.)*engine_.state().duration;
        if (commit) engine_.seek(dragPosition_,true);
    } else if (dragAction_==VolumeBar) {
        const float span=std::max(1.f,volumeTrack_.right-volumeTrack_.left);
        engine_.setVolume(std::clamp(static_cast<double>((x-volumeTrack_.left)/span),0.,1.)*100);
    }
    InvalidateRect(window_,nullptr,FALSE);
}

void PlayerWindow::notice(const std::wstring& message,bool error) {
    notice_=message; noticeError_=error; noticeUntil_=GetTickCount64()+(error?12000:4500);
    InvalidateRect(window_,nullptr,FALSE);
}

void PlayerWindow::savePreferences() {
    if (engine_.ready()) {
        const auto& state=engine_.state();
        preferences_.volume=state.volume;
        preferences_.speed=state.speed;
        preferences_.muted=state.muted;
    }
    if (!fullscreen_ && !compact_ && !pip_ && !musicMode_ && !IsZoomed(window_) && !IsIconic(window_)) {
        preferences_.windowWidth=static_cast<int>(width_);
        preferences_.windowHeight=static_cast<int>(height_);
    }
    preferences_.showPlaylist=showPlaylist_;
    if (preferences_.rememberWindowPosition && !pip_ && !fullscreen_) { RECT bounds{}; GetWindowRect(window_,&bounds); preferences_.windowX=bounds.left; preferences_.windowY=bounds.top; }
    if (!settings_.save(preferences_)) notice(L"Could not save preferences to your application data folder.",true);
}

void PlayerWindow::onEngineEvent() {
    engine_.poll();
    const auto& state=engine_.state();
    if (state.loaded && pendingSeek_>=0 && state.path==pendingResumePath_) {
        engine_.seek(pendingSeek_,true); engine_.setProperty("pause",pendingPause_?"yes":"no");
        pendingSeek_=-1; pendingResumePath_.clear();
    }
    if (state.loaded && state.path==pendingMediaPath_ && !pendingSubtitles_.empty()) {
        for (const auto& path:pendingSubtitles_) engine_.addSubtitle(path);
        pendingSubtitles_.clear();
        pendingMediaPath_.clear();
    }
    if (!state.error.empty() && state.error!=lastError_) {
        lastError_=state.error;
        pendingSubtitles_.clear(); pendingMediaPath_.clear();
        notice(fromUtf8(state.error),true);
    }
    if (state.loaded && !state.path.empty() && !state.idle && state.path!=lastPath_) {
        lastPath_=state.path;
        if (preferences_.recordRecentFiles) settings_.rememberFile(preferences_,state.path);
        persistPlayback(true); platform_.setRecentFiles(preferences_.recentFiles);
        const bool audioOnly=std::none_of(state.tracks.begin(),state.tracks.end(),[](const auto& track){return track.type=="video" && !track.albumArt;});
        if (preferences_.autoMusicMode && musicMode_!=audioOnly) toggleMusicMode();
        if (preferences_.fullscreenOnOpen && !fullscreen_ && state.width>0) toggleFullscreen();
        savePreferences();
    }
    const std::wstring title=state.idle ? L"Velin" : fromUtf8(state.title.empty()?mediaTitle(state.path):state.title)+L" \u2014 Velin";
    if (title!=lastTitle_) { SetWindowTextW(window_,title.c_str()); lastTitle_=title; }
    updatePlatform();
    layoutVideo();
    lastIdle_=state.idle;
    if (GetTickCount64()-lastAnimation_>=80 || state.paused || state.idle) { InvalidateRect(window_,nullptr,FALSE); lastAnimation_=GetTickCount64(); }
}

LRESULT PlayerWindow::handleMessage(UINT message,WPARAM wParam,LPARAM lParam) {
    if (auto result=accessibility_.handleMessage(message,wParam,lParam)) return *result;
    if (platform_.handleMessage(message,wParam,lParam)) return 0;
    switch(message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; BeginPaint(window_,&paint); draw(); EndPaint(window_,&paint); return 0;
    }
    case WM_SIZE:
        if (wParam==SIZE_MINIMIZED && preferences_.pauseWhenMinimized && !engine_.state().idle && !engine_.state().paused) { pausedByWindow_=true; engine_.setProperty("pause","yes"); }
        if (wParam!=SIZE_MINIMIZED) {
            if (pausedByWindow_ && preferences_.pauseWhenMinimized) { engine_.setProperty("pause","no"); pausedByWindow_=false; }
            RECT bounds{}; GetClientRect(window_,&bounds);
            width_=static_cast<float>(bounds.right)/dpi_; height_=static_cast<float>(bounds.bottom)/dpi_;
            if (target_) {
                if (FAILED(target_->Resize(D2D1::SizeU(bounds.right,bounds.bottom)))) discardTarget();
            }
            hovered_=-1;
            layoutVideo(); InvalidateRect(window_,nullptr,FALSE);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto* limits=reinterpret_cast<MINMAXINFO*>(lParam);
        limits->ptMinTrackSize={static_cast<LONG>((pip_?240.f:minWindowWidth)*dpi_),static_cast<LONG>((pip_?135.f:minWindowHeight)*dpi_)};
        return 0;
    }
    case WM_SIZING:
        if (constrainWindowSizing(wParam,*reinterpret_cast<RECT*>(lParam))) return TRUE;
        break;
    case WM_DPICHANGED: {
        dpi_=static_cast<float>(HIWORD(wParam))/96.f;
        if (target_) target_->SetDpi(dpi_*96,dpi_*96);
        const auto* bounds=reinterpret_cast<RECT*>(lParam);
        SetWindowPos(window_,nullptr,bounds->left,bounds->top,bounds->right-bounds->left,bounds->bottom-bounds->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case engineMessage: onEngineEvent(); return 0;
    case thumbnailMessage:
        if (auto result=thumbnails_.takeResult();result && hovered_>=0 && static_cast<size_t>(hovered_)<hits_.size() && hits_[static_cast<size_t>(hovered_)].action==SeekBar)
            preview_.show(window_,*result,thumbnailAnchor_);
        return 0;
    case WM_APP+11: {
        if (ocrWorker_.joinable()) ocrWorker_.join();
        std::optional<TextRecognitionResult> result;
        {std::lock_guard lock(ocrMutex_);result=std::move(ocrResult_);ocrResult_.reset();}
        if(!result)return 0;
        if (!result->error.empty()) notice(result->error,true);
        else showTextPrompt(window_,L"Recognized text",L"Text from the current frame",result->text,true);
        return 0;
    }
    case WM_TIMER:
        if (!closing_) {
            if(deferredClose_ && (!plugins_ || !plugins_->busy())){deferredClose_=false;PostMessageW(window_,WM_CLOSE,0,0);return 0;}
            pollServices();
            const auto& state=engine_.state();
            // A hover can outlive its overlay (it may hide under the pointer), so
            // drop it once the pointer is no longer over any part of the player.
            POINT cursor{}; GetCursorPos(&cursor); const HWND under=WindowFromPoint(cursor);
            if (hovered_>=0 && dragAction_==None && under!=window_ && !IsChild(window_,under) && !isLayerWindow(under)) { hovered_=-1; preview_.hide(); InvalidateRect(window_,nullptr,FALSE); }
            // Controls float over the picture, so they step aside during playback
            // until the pointer moves, as in IINA.
            const bool visible=!preferences_.fullscreenAutoHide || state.idle || state.paused || state.width==0 || GetTickCount64()<interactionUntil_ || dragAction_!=None || pointerOverChrome();
            if (controlsVisible_!=visible) {
                controlsVisible_=visible;
                if (!visible) { hovered_=-1; preview_.hide(); thumbnailTime_=-1; }
                layoutVideo(); InvalidateRect(window_,nullptr,FALSE);
            }
            if (!IsIconic(window_) && ((!state.idle && !state.paused && (controlsVisible_ || sidebarShown())) || (noticeUntil_>0 && GetTickCount64()>=noticeUntil_))) {
                InvalidateRect(window_,nullptr,FALSE);
                if (GetTickCount64()>=noticeUntil_) noticeUntil_=0;
            }
            if (state.loaded && GetTickCount64()-lastPersist_>=10000) { persistPlayback(); updatePlatform(); }
        }
        return 0;
    case WM_MOUSEMOVE: {
        pointerMoved();
        const float x=static_cast<float>(GET_X_LPARAM(lParam))/dpi_,y=static_cast<float>(GET_Y_LPARAM(lParam))/dpi_;
        if (dragAction_==SeekBar || dragAction_==VolumeBar) updatePointer(x,y,false);
        else {
            const int hit=hitAt(x,y);
            if (hovered_!=hit) { hovered_=hit; InvalidateRect(window_,nullptr,FALSE); }
            if (hit>=0 && hits_[static_cast<size_t>(hit)].action==SeekBar && preferences_.showTimelineThumbnails && engine_.state().duration>0 && thumbnails_.available()) {
                const double time=std::clamp(static_cast<double>((x-seekTrack_.left)/std::max(1.f,seekTrack_.right-seekTrack_.left)),0.,1.)*engine_.state().duration;
                thumbnailAnchor_={GET_X_LPARAM(lParam),static_cast<LONG>((controlBounds_.top+4)*dpi_)}; ClientToScreen(window_,&thumbnailAnchor_);
                if (std::abs(time-thumbnailTime_)>=.5) { thumbnailTime_=time; thumbnails_.request(engine_.state().path,time,static_cast<unsigned>(preferences_.timelineThumbnailWidth)); }
            } else { preview_.hide(); thumbnailTime_=-1; }
            TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,pointerWindow_?pointerWindow_:window_,0}; TrackMouseEvent(&tracking);
            SetCursor(LoadCursorW(nullptr,hit>=0?IDC_HAND:IDC_ARROW));
        }
        return 0;
    }
    case WM_MOUSELEAVE: hovered_=-1; preview_.hide(); thumbnailTime_=-1; InvalidateRect(window_,nullptr,FALSE); return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(window_);
        const float x=static_cast<float>(GET_X_LPARAM(lParam))/dpi_,y=static_cast<float>(GET_Y_LPARAM(lParam))/dpi_;
        const int hit=hitAt(x,y);
        if (hit>=0) {
            if (hits_[static_cast<size_t>(hit)].action==PlayItem) { movingItem_=hits_[static_cast<size_t>(hit)].data; dragStart_={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)}; }
            const int action=hits_[static_cast<size_t>(hit)].action;
            if (action==SeekBar || action==VolumeBar) {
                dragAction_=action; SetCapture(window_); updatePointer(x,y,false);
            }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const float x=static_cast<float>(GET_X_LPARAM(lParam))/dpi_,y=static_cast<float>(GET_Y_LPARAM(lParam))/dpi_;
        if (movingItem_>=0 && (std::abs(GET_X_LPARAM(lParam)-dragStart_.x)>GetSystemMetrics(SM_CXDRAG) || std::abs(GET_Y_LPARAM(lParam)-dragStart_.y)>GetSystemMetrics(SM_CYDRAG))) {
            const int hit=hitAt(x,y); if (hit>=0 && hits_[static_cast<size_t>(hit)].action==PlayItem) engine_.movePlaylist(movingItem_,hits_[static_cast<size_t>(hit)].data);
            movingItem_=-1; return 0;
        }
        movingItem_=-1;
        if (dragAction_==SeekBar || dragAction_==VolumeBar) {
            updatePointer(x,y,true); dragAction_=None; ReleaseCapture(); savePreferences();
        } else {
            const int hit=hitAt(x,y);
            if (hit>=0) { const auto item=hits_[static_cast<size_t>(hit)]; execute(item.action,item.data); }
        }
        return 0;
    }
    case WM_CAPTURECHANGED: dragAction_=None; return 0;
    case WM_MOUSEWHEEL: {
        POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)}; ScreenToClient(window_,&point);
        const int delta=GET_WHEEL_DELTA_WPARAM(wParam);
        if (sidebarShown() && static_cast<float>(point.x)/dpi_>=sidebarRect().left && static_cast<float>(point.y)/dpi_<sidebarRect().bottom) {
            listOffset_+=delta>0?-1:1; InvalidateRect(window_,nullptr,FALSE);
        } else engine_.setVolume(engine_.state().volume+(delta>0?preferences_.volumeStep:-preferences_.volumeStep));
        return 0;
    }
    case WM_CONTEXTMENU: {
        POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
        if (point.x==-1 && point.y==-1) GetCursorPos(&point);
        POINT client=point; ScreenToClient(window_,&client);
        const int hit=hitAt(static_cast<float>(client.x)/dpi_,static_cast<float>(client.y)/dpi_);
        if (hit>=0 && hits_[static_cast<size_t>(hit)].action==PlayItem) {
            const int data=hits_[static_cast<size_t>(hit)].data;
            HMENU menu=CreatePopupMenu();
            AppendMenuW(menu,MF_STRING,PlayItem,L"Play this item");
            AppendMenuW(menu,MF_STRING,RemoveItem,L"Remove from playlist");
            AppendMenuW(menu,MF_STRING,MoveItemUp,L"Move up"); AppendMenuW(menu,MF_STRING,MoveItemDown,L"Move down");
            AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
            AppendMenuW(menu,MF_STRING,ClearPlaylist,L"Clear upcoming items");
            std::vector<PluginMenuItem> custom;
            if(plugins_ && data>=0 && static_cast<size_t>(data)<engine_.state().playlist.size())custom=plugins_->playlistMenuItems({engine_.state().playlist[static_cast<size_t>(data)].filename});
            if(!custom.empty())AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
            appendPluginMenu(menu,custom,11000);
            const UINT selected=TrackPopupMenuEx(menu,TPM_RETURNCMD | TPM_RIGHTBUTTON,point.x,point.y,window_,nullptr);
            DestroyMenu(menu);
            if(selected>=11000 && selected-11000<custom.size()){const auto& item=custom[selected-11000];plugins_->invokeMenu(item.plugin,item.identifier);}
            else if (selected) execute(static_cast<int>(selected),data);
        } else showMenu(PlaybackMenu,point);
        return 0;
    }
    case WM_DROPFILES: {
        const auto drop=reinterpret_cast<HDROP>(wParam);
        std::vector<std::string> paths;
        const UINT count=DragQueryFileW(drop,0xffffffff,nullptr,0);
        for (UINT i=0;i<count;++i) {
            const UINT length=DragQueryFileW(drop,i,nullptr,0);
            std::wstring path(static_cast<size_t>(length)+1,L'\0');
            DragQueryFileW(drop,i,path.data(),length+1); path.resize(length);
            paths.push_back(toUtf8(path));
        }
        DragFinish(drop);
        openPaths(paths,(GetKeyState(VK_SHIFT)&0x8000)!=0); return 0;
    }
    case WM_APPCOMMAND:
        switch(GET_APPCOMMAND_LPARAM(lParam)) {
        case APPCOMMAND_MEDIA_PLAY_PAUSE: execute(TogglePause); return TRUE;
        case APPCOMMAND_MEDIA_NEXTTRACK: execute(Next); return TRUE;
        case APPCOMMAND_MEDIA_PREVIOUSTRACK: execute(Previous); return TRUE;
        case APPCOMMAND_MEDIA_STOP: execute(Stop); return TRUE;
        default: break;
        }
        break;
    case WM_COMMAND: execute(LOWORD(wParam)); return 0;
    case WM_COPYDATA:
        if (receivePluginMessage(wParam,lParam)) return TRUE;
        if (const auto arguments=receiveLaunchRequest(lParam)) { acceptLaunch(*arguments); return TRUE; }
        return FALSE;
    case WM_SETTINGCHANGE: if (preferences_.theme==Theme::System) applyTheme(); return 0;
    case WM_MOVE: moveLayers(); updateWebViews(); updateAccessibility(); return 0;
    case WM_ACTIVATEAPP:
        if (preferences_.pauseWhenInactive && !engine_.state().idle) {
            if (!wParam && !engine_.state().paused) { pausedByWindow_=true; engine_.setProperty("pause","yes"); }
            else if (wParam && pausedByWindow_) { pausedByWindow_=false; engine_.setProperty("pause","no"); }
        }
        break;
    case WM_POWERBROADCAST:
        if (wParam==PBT_APMSUSPEND && preferences_.pauseOnSleep && !engine_.state().idle) engine_.setProperty("pause","yes");
        return TRUE;
    case WM_SYSKEYDOWN:
        if (wParam==VK_RETURN) { toggleFullscreen(); return 0; }
        if (wParam=='F' || wParam=='P' || wParam=='V' || wParam=='A' || wParam=='S') {
            POINT point{static_cast<LONG>(8*dpi_),static_cast<LONG>(topBarHeight*dpi_)}; ClientToScreen(window_,&point);
            showMenu(wParam=='F'?FileMenu:wParam=='P'?PlaybackMenu:wParam=='V'?VideoMenu:wParam=='A'?AudioMenu:SubtitleMenu,point);
            return 0;
        }
        break;
    case WM_CLOSE:
        if(plugins_ && plugins_->busy()){
            deferredClose_=true;
            EnumThreadWindows(GetCurrentThreadId(),[](HWND child,LPARAM owner)->BOOL{if(GetWindow(child,GW_OWNER)==reinterpret_cast<HWND>(owner))PostMessageW(child,WM_CLOSE,0,0);return TRUE;},reinterpret_cast<LPARAM>(window_));
            return 0;
        }
        if (!closing_) {
            closing_=true; KillTimer(window_,1);
            persistPlayback(); preview_.hide(); thumbnails_.cancel();
            if (online_) online_->cancelAll();
            webviews_.clear(); plugins_.reset(); accessibility_.shutdown();
            savePreferences();
            engine_.shutdown(); // The embedded HWND must outlive the playback engine.
            DestroyWindow(window_);
        }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: break;
    }
    return DefWindowProcW(window_,message,wParam,lParam);
}

void PlayerWindow::showFileDialog(bool append,bool subtitles,bool folder) {
    IFileOpenDialog* dialog=nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))) {
        notice(L"Windows could not open the file picker.",true); return;
    }
    DWORD options=0; dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | (folder?FOS_PICKFOLDERS:FOS_ALLOWMULTISELECT));
    const COMDLG_FILTERSPEC mediaFilters[]={{L"Media files",L"*.mkv;*.mp4;*.mov;*.webm;*.avi;*.m4v;*.mpg;*.mpeg;*.ts;*.m2ts;*.vob;*.flv;*.wmv;*.mp3;*.flac;*.m4a;*.aac;*.ogg;*.opus;*.wav;*.aiff;*.m3u;*.m3u8;*.pls"},{L"All files",L"*.*"}};
    const COMDLG_FILTERSPEC subtitleFilters[]={{L"Subtitle files",L"*.srt;*.ass;*.ssa;*.vtt;*.sub;*.idx;*.sup;*.ttml;*.lrc"},{L"All files",L"*.*"}};
    if (!folder) dialog->SetFileTypes(2,subtitles?subtitleFilters:mediaFilters);
    dialog->SetTitle(folder?L"Open a media folder":subtitles?L"Add subtitles":append?L"Add files to playlist":L"Open media");
    std::vector<std::string> paths;
    if (SUCCEEDED(dialog->Show(window_))) {
        IShellItemArray* items=nullptr;
        if (SUCCEEDED(dialog->GetResults(&items))) {
            DWORD count=0; items->GetCount(&count);
            for (DWORD i=0;i<count;++i) {
                IShellItem* item=nullptr;
                if (SUCCEEDED(items->GetItemAt(i,&item))) {
                    PWSTR path=nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))) {
                        paths.push_back(toUtf8(path)); CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            items->Release();
        }
    }
    dialog->Release();
    SetFocus(window_);
    if (subtitles) {
        for (const auto& path:paths) engine_.addSubtitle(path);
        if (!paths.empty()) notice(L"Subtitle track added");
    } else if (!paths.empty()) openPaths(paths,append);
}

void PlayerWindow::openPaths(const std::vector<std::string>& paths,bool append) {
    if (!engine_.ready()) { notice(fromUtf8(engine_.error()),true); return; }
    std::vector<std::string> media,subtitles;
    for (const auto& path:expandMediaPaths(paths)) (isSubtitleFile(path)?subtitles:media).push_back(path);
    if (!append && preferences_.autoAddRelatedFiles && paths.size()==1 && media.size()==1)
        media=relatedMediaPaths(media.front());
    if (media.empty()) {
        if (subtitles.empty()) { notice(L"No supported media files were found in this folder.",true); return; }
        if (engine_.state().idle) { notice(L"Open a video before adding subtitles.",true); return; }
    } else {
        if (!append && preferences_.rememberPosition && !engine_.state().idle)
            engine_.command({"write-watch-later-config"});
        lastError_.clear();
        if (!engine_.open(media,append)) { notice(fromUtf8(engine_.error()),true); return; }
        if (!append) engine_.setProperty("pause",preferences_.pauseOnOpen?"yes":"no");
        if (!append) listOffset_=0;
        if (!append) { lastPath_.clear(); abLoopPhase_=0; }
        notice(append?L"Added to your playlist":L"Opening media\u2026");
    }
    if (!media.empty() && !append) { pendingSubtitles_=std::move(subtitles); pendingMediaPath_=media.front(); }
    else for (const auto& path:subtitles) engine_.addSubtitle(path);
    SetFocus(window_);
}

void PlayerWindow::toggleFullscreen() {
    fullscreen_=!fullscreen_;
    // Fullscreen opens on the bare picture; leaving it shows the controls
    // until the usual hide delay passes.
    controlsVisible_=!fullscreen_; hovered_=-1; preview_.hide();
    interactionUntil_=fullscreen_?0:GetTickCount64()+static_cast<ULONGLONG>(preferences_.controlsHideSeconds*1000);
    GetCursorPos(&lastPointer_);
    if (fullscreen_) {
        placement_.length=sizeof(placement_); GetWindowPlacement(window_,&placement_);
        MONITORINFO monitor{}; monitor.cbSize=sizeof(monitor);
        GetMonitorInfoW(MonitorFromWindow(window_,MONITOR_DEFAULTTONEAREST),&monitor);
        SetWindowLongPtrW(window_,GWL_STYLE,GetWindowLongPtrW(window_,GWL_STYLE)&~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW));
        SetWindowPos(window_,HWND_TOP,monitor.rcMonitor.left,monitor.rcMonitor.top,
            monitor.rcMonitor.right-monitor.rcMonitor.left,monitor.rcMonitor.bottom-monitor.rcMonitor.top,
            SWP_FRAMECHANGED);
    } else {
        SetWindowLongPtrW(window_,GWL_STYLE,GetWindowLongPtrW(window_,GWL_STYLE)|WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(window_,&placement_);
        SetWindowPos(window_,preferences_.alwaysOnTop?HWND_TOPMOST:HWND_NOTOPMOST,0,0,0,0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
    }
    layoutVideo(); InvalidateRect(window_,nullptr,FALSE);
}

std::pair<double,double> PlayerWindow::videoDisplaySize() const {
    // mpv's output dimensions include pixel aspect, rotation and active filters.
    const auto params=Json::parse(engine_.propertyJson("video-out-params"),nullptr,false);
    if (params.is_object() && params.contains("dw") && params["dw"].is_number() &&
        params.contains("dh") && params["dh"].is_number()) {
        const double width=params["dw"].get<double>(),height=params["dh"].get<double>();
        if (std::isfinite(width) && std::isfinite(height) && width>0 && height>0) return {width,height};
    }
    const auto& state=engine_.state();
    return {static_cast<double>(state.width),static_cast<double>(state.height)};
}

bool PlayerWindow::constrainWindowSizing(WPARAM edge,RECT& bounds) const {
    if (!preferences_.lockWindowAspect || fullscreen_ || pip_ || musicMode_ || uiHidden_) return false;
    const auto [videoWidth,videoHeight]=videoDisplaySize();
    if (videoWidth<=0 || videoHeight<=0) return false;
    const double ratio=videoWidth/videoHeight;
    RECT frame{};
    AdjustWindowRectExForDpi(&frame,static_cast<DWORD>(GetWindowLongPtrW(window_,GWL_STYLE)),FALSE,
                             static_cast<DWORD>(GetWindowLongPtrW(window_,GWL_EXSTYLE)),GetDpiForWindow(window_));
    const double frameWidth=frame.right-frame.left,frameHeight=frame.bottom-frame.top;
    // The video fills the client area beside the sidebar; the remaining chrome floats above it.
    const double side=std::clamp(static_cast<double>(preferences_.sidebarWidth),240.,640.);
    const double minWidth=static_cast<double>(minWindowWidth)*dpi_,minHeight=static_cast<double>(minWindowHeight)*dpi_;
    double width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
    const bool vertical=edge==WMSZ_TOP || edge==WMSZ_BOTTOM;
    // The sidebar disappears below its layout threshold, so resolve its width
    // from the candidate window rather than the previous window size.
    const auto sidebarFor=[&](double candidate) { return showPlaylist_ && (candidate-frameWidth)/dpi_>=side+sidebarThreshold ? side*dpi_ : 0.; };
    double sidebar=sidebarFor(width);
    if (vertical) {
        height=std::max(height,minHeight);
        width=(height-frameHeight)*ratio+frameWidth+sidebar;
        sidebar=sidebarFor(width);
        width=(height-frameHeight)*ratio+frameWidth+sidebar;
    } else {
        width=std::max(width,minWidth);
        height=(width-frameWidth-sidebar)/ratio+frameHeight;
    }
    if (height<minHeight) {
        height=minHeight;
        width=(height-frameHeight)*ratio+frameWidth+sidebar;
    }
    if (width<minWidth) {
        width=minWidth;
        sidebar=sidebarFor(width);
        height=(width-frameWidth-sidebar)/ratio+frameHeight;
    }
    const LONG w=static_cast<LONG>(std::lround(width)),h=static_cast<LONG>(std::lround(height));
    if (edge==WMSZ_LEFT || edge==WMSZ_TOPLEFT || edge==WMSZ_BOTTOMLEFT) bounds.left=bounds.right-w;
    else bounds.right=bounds.left+w;
    if (edge==WMSZ_TOP || edge==WMSZ_TOPLEFT || edge==WMSZ_TOPRIGHT) bounds.top=bounds.bottom-h;
    else bounds.bottom=bounds.top+h;
    return true;
}

void PlayerWindow::resizeForVideo(double scale,bool fitScreen) {
    const auto [videoWidth,videoHeight]=videoDisplaySize();
    if (videoWidth<=0 || videoHeight<=0 || fullscreen_ || pip_ || musicMode_) {
        notice(L"Open a video in the normal window to change its size.",true); return;
    }
    if (IsZoomed(window_)) ShowWindow(window_,SW_RESTORE);
    RECT frame{};
    AdjustWindowRectExForDpi(&frame,static_cast<DWORD>(GetWindowLongPtrW(window_,GWL_STYLE)),FALSE,
                             static_cast<DWORD>(GetWindowLongPtrW(window_,GWL_EXSTYLE)),GetDpiForWindow(window_));
    const double frameWidth=frame.right-frame.left,frameHeight=frame.bottom-frame.top;
    // The video fills the client area beside the sidebar; the remaining chrome floats above it.
    const double side=std::clamp(static_cast<double>(preferences_.sidebarWidth),240.,640.);
    const double sidePx=showPlaylist_ ? side*dpi_ : 0.;
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(window_,MONITOR_DEFAULTTONEAREST),&monitor);
    const double availableWidth=monitor.rcWork.right-monitor.rcWork.left;
    const double availableHeight=monitor.rcWork.bottom-monitor.rcWork.top;
    if (fitScreen) scale=std::min((availableWidth-frameWidth-sidePx)/videoWidth,(availableHeight-frameHeight)/videoHeight);
    const double minScale=std::max((static_cast<double>(minWindowWidth)*dpi_-frameWidth)/videoWidth,
                                   (static_cast<double>(minWindowHeight)*dpi_-frameHeight)/videoHeight);
    scale=std::max(scale,minScale);
    const double sidebar=showPlaylist_ && videoWidth*scale/dpi_>=sidebarThreshold ? sidePx : 0.;
    int width=static_cast<int>(std::lround(videoWidth*scale+frameWidth+sidebar));
    int height=static_cast<int>(std::lround(videoHeight*scale+frameHeight));
    RECT old{}; GetWindowRect(window_,&old);
    const int left=std::clamp(static_cast<int>((old.left+old.right-width)/2),static_cast<int>(monitor.rcWork.left),std::max(static_cast<int>(monitor.rcWork.left),static_cast<int>(monitor.rcWork.right)-width));
    const int top=std::clamp(static_cast<int>((old.top+old.bottom-height)/2),static_cast<int>(monitor.rcWork.top),std::max(static_cast<int>(monitor.rcWork.top),static_cast<int>(monitor.rcWork.bottom)-height));
    SetWindowPos(window_,nullptr,left,top,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
}

void PlayerWindow::execute(int action,int data) {
    const auto& state=engine_.state();
    if (action>=WindowSizeBase && action<WindowSizeBase+4) {
        constexpr double scales[]={.5,1.,2.,1.};
        resizeForVideo(scales[action-WindowSizeBase],action==WindowSizeBase+3); return;
    }
    if (action==ToggleWindowAspectLock) {
        preferences_.lockWindowAspect=!preferences_.lockWindowAspect; savePreferences(); return;
    }
    if (action>=100 && action<1000 && executeFeature(action,data)) return;
    if(action==ShowInfo){showMediaInspector(window_,engine_);return;}
    if(action==VideoFilters || action==AudioFilters){showFilterManager(window_,engine_,action==AudioFilters);return;}
    if(action>=AudioDeviceBase && action<AudioDeviceBase+1000){const auto index=static_cast<size_t>(action-AudioDeviceBase);if(index<state.audioDevices.size()){preferences_.audioDevice=state.audioDevices[index].name;engine_.setProperty("audio-device",preferences_.audioDevice);}return;}
    if(action>=VideoTrackBase && action<VideoTrackBase+1000){const auto index=static_cast<size_t>(action-VideoTrackBase);engine_.setProperty("vid",index==999?"no":index<state.tracks.size()?std::to_string(state.tracks[index].id):"auto");return;}
    if(action>=SecondarySubtitleBase && action<SecondarySubtitleBase+1000){const auto index=static_cast<size_t>(action-SecondarySubtitleBase);engine_.selectSecondarySubtitle(index==999?-1:index<state.tracks.size()?state.tracks[index].id:-1);return;}
    if (action>=PluginMenuBase && action<PluginMenuBase+1000) {
        const auto index=static_cast<size_t>(action-PluginMenuBase);
        if (plugins_ && index<pluginMenus_.size()) plugins_->invokeMenu(pluginMenus_[index].plugin,pluginMenus_[index].identifier);
        return;
    }
    if (action>=SpeedBase && action<SpeedBase+11) {
        constexpr double speeds[]={.25,.5,.75,1,1.25,1.5,1.75,2,2.5,3,4};
        engine_.setSpeed(speeds[action-SpeedBase]); return;
    }
    if (action>=AudioBase && action<SubtitleBase) {
        const auto index=static_cast<size_t>(action-AudioBase);
        if (index<state.tracks.size()) engine_.setProperty("aid",std::to_string(state.tracks[index].id));
        return;
    }
    if (action>=SubtitleBase && action<ChapterBase) {
        if (action==SubtitleBase+999) engine_.setProperty("sid","no");
        else {
            const auto index=static_cast<size_t>(action-SubtitleBase);
            if (index<state.tracks.size()) engine_.setProperty("sid",std::to_string(state.tracks[index].id));
        }
        return;
    }
    if (action>=ChapterBase && action<RecentBase) { execute(ChapterItem,action-ChapterBase); return; }
    if (action>=RecentBase && action<AspectBase) { execute(RecentItem,action-RecentBase); return; }
    if (action>=AspectBase && action<AspectBase+5) {
        constexpr const char* values[]={"-1","1.7777777778","1.3333333333","1","2.35"};
        engine_.setProperty("video-aspect-override",values[action-AspectBase]); return;
    }
    if (action>=CropAspectBase && action<CropAspectBase+4) {
        constexpr const char* ratios[]={"16:9","4:3","1:1","2.35:1"};
        const auto geometry=centeredCropGeometry(state.width,state.height,ratios[action-CropAspectBase]);
        if (!geometry) notice(L"Open a video before cropping.",true);
        else if (engine_.setProperty("video-crop",*geometry)) { preferences_.mpvOptions["video-crop"]=*geometry; notice(L"Crop requested"); }
        else notice(L"Could not request crop.",true);
        return;
    }
    switch(action) {
    case FileMenu: case PlaybackMenu: case VideoMenu: case AudioMenu: case SubtitleMenu: case ViewMenu: case SpeedMenu: {
        POINT point{}; GetCursorPos(&point);
        if (hovered_>=0 && hovered_<static_cast<int>(hits_.size())) {
            const auto bounds=hits_[static_cast<size_t>(hovered_)].rect;
            point={static_cast<LONG>(bounds.left*dpi_),static_cast<LONG>(bounds.bottom*dpi_)};
            ClientToScreen(window_,&point);
        }
        showMenu(action,point); break;
    }
    case OpenFiles: showFileDialog(false); break;
    case AddFiles: showFileDialog(true); break;
    case OpenFolder: showFileDialog(false,false,true); break;
    case AddSubtitle:
        if (state.idle) notice(L"Open a video before adding subtitles.",true);
        else showFileDialog(false,true);
        break;
    case OpenUrl: {
        const auto value=promptUrl(window_,instance_);
        if (!value.empty()) openPaths({toUtf8(value)});
        break;
    }
    case TogglePause:
        if (state.idle) showFileDialog(false);
        else engine_.togglePause();
        break;
    case Stop:
        if (!state.idle && preferences_.rememberPosition) engine_.command({"write-watch-later-config"});
        engine_.stop(); pendingSubtitles_.clear(); break;
    case Previous: if (!state.playlist.empty()) engine_.command({"playlist-prev","weak"}); break;
    case Next: if (!state.playlist.empty()) engine_.command({"playlist-next","weak"}); break;
    case ToggleMute: engine_.toggleMute(); break;
    case ToggleFullscreen: toggleFullscreen(); break;
    case TogglePlaylist: showPlaylist_=!showPlaylist_; hovered_=-1; layoutVideo(); break;
    case PlaylistTab: chapters_=false; listOffset_=0; hovered_=-1; break;
    case ChaptersTab: chapters_=true; listOffset_=0; hovered_=-1; break;
    case ToggleRepeat:
        repeating_=!repeating_; engine_.setProperty("loop-file",repeating_?"inf":"no");
        notice(repeating_?L"Repeat current file on":L"Repeat current file off"); break;
    case TogglePlaylistLoop:
        playlistLoop_=!playlistLoop_; engine_.setProperty("loop-playlist",playlistLoop_?"inf":"no");
        notice(playlistLoop_?L"Repeat playlist on":L"Repeat playlist off"); break;
    case Screenshot:
        if (!state.idle) {
            engine_.command({"screenshot",preferences_.screenshotIncludeSubtitles?"subtitles":"video"});
            if(preferences_.screenshotCopyToClipboard)execute(CopyScreenshot);
            const auto directory=preferences_.screenshotDirectory.empty()?dataDirectory_/L"Screenshots":std::filesystem::path(fromUtf8(preferences_.screenshotDirectory));
            notice(L"Saving screenshot to "+directory.wstring());
        }
        break;
    case PlayItem: engine_.selectPlaylist(data); break;
    case RemoveItem: engine_.command({"playlist-remove",std::to_string(data)}); break;
    case ClearPlaylist: engine_.command({"playlist-clear"}); break;
    case ChapterItem:
        if (data>=0 && static_cast<size_t>(data)<state.chapters.size()) engine_.seek(state.chapters[static_cast<size_t>(data)].time,true);
        break;
    case RecentItem:
        if (data>=0 && static_cast<size_t>(data)<preferences_.recentFiles.size()) {
            const auto path=preferences_.recentFiles[static_cast<size_t>(data)]; openPaths({path});
        }
        break;
    case ToggleTopmost:
        preferences_.alwaysOnTop=!preferences_.alwaysOnTop;
        SetWindowPos(window_,preferences_.alwaysOnTop?HWND_TOPMOST:HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        savePreferences(); notice(preferences_.alwaysOnTop?L"Always on top enabled":L"Always on top disabled"); break;
    case ToggleResume:
        preferences_.rememberPosition=!preferences_.rememberPosition;
        engine_.setProperty("options/resume-playback",preferences_.rememberPosition?"yes":"no");
        engine_.setProperty("options/save-position-on-quit",preferences_.rememberPosition?"yes":"no");
        savePreferences(); break;
    case ClearHistory:
        preferences_.recentFiles.clear();
        if (settings_.save(preferences_)) notice(L"Recent files cleared");
        else notice(L"Could not save recent-file preferences.",true);
        break;
    case SeekBack: if (!state.idle) engine_.seek(-5); break;
    case SeekForward: if (!state.idle) engine_.seek(5); break;
    case VolumeUp: engine_.setVolume(state.volume+5); break;
    case VolumeDown: engine_.setVolume(state.volume-5); break;
    case PreviousChapter: if (!state.chapters.empty()) engine_.command({"add","chapter","-1"}); break;
    case NextChapter: if (!state.chapters.empty()) engine_.command({"add","chapter","1"}); break;
    case FrameStep: if (!state.idle) engine_.command({"frame-step"}); break;
    case CycleSubtitle: if (!state.idle) engine_.command({"cycle","sid"}); break;
    case CycleAudio: if (!state.idle) engine_.command({"cycle","aid"}); break;
    case SubtitleDelayDown: engine_.command({"add","sub-delay","-0.1"}); notice(L"Subtitle delay \u2212 0.1 s"); break;
    case SubtitleDelayUp: engine_.command({"add","sub-delay","0.1"}); notice(L"Subtitle delay + 0.1 s"); break;
    case AudioDelayDown: engine_.command({"add","audio-delay","-0.1"}); notice(L"Audio delay \u2212 0.1 s"); break;
    case AudioDelayUp: engine_.command({"add","audio-delay","0.1"}); notice(L"Audio delay + 0.1 s"); break;
    case ToggleSubtitles: engine_.command({"cycle","sub-visibility"}); break;
    case ToggleSecondarySubtitles: engine_.command({"cycle","secondary-sub-visibility"}); break;
    case Rotate: engine_.command({"cycle-values","video-rotate","0","90","180","270"}); break;
    case ResetVideo:
        engine_.setProperty("video-aspect-override","-1"); engine_.setProperty("video-rotate","0");
        engine_.setProperty("video-zoom","0"); break;
    case LoopAB:
        if (!state.idle) {
            engine_.command({"ab-loop"}); abLoopPhase_=(abLoopPhase_+1)%3;
            notice(abLoopPhase_==1?L"A\u2013B loop: start point set":abLoopPhase_==2?L"A\u2013B loop: end point set":L"A\u2013B loop cleared");
        }
        break;
    case Compact: {
        if (fullscreen_) toggleFullscreen();
        compact_=!compact_;
        RECT bounds{0,0,static_cast<LONG>((compact_?760:preferences_.windowWidth)*dpi_),
                          static_cast<LONG>((compact_?470:preferences_.windowHeight)*dpi_)};
        AdjustWindowRectExForDpi(&bounds,static_cast<DWORD>(GetWindowLongPtrW(window_,GWL_STYLE)),FALSE,0,GetDpiForWindow(window_));
        SetWindowPos(window_,nullptr,0,0,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOMOVE | SWP_NOZORDER);
        break;
    }
    case OpenSettings: {
        const auto directory=settingsDirectory();
        ShellExecuteW(window_,L"open",directory.c_str(),nullptr,nullptr,SW_SHOWNORMAL); break;
    }
    case ShowInfo: {
        std::wstring value=L"Title: "+fromUtf8(state.title)+L"\n\nSource: "+fromUtf8(state.path)+L"\n\nVideo: "+fromUtf8(state.videoCodec);
        if (state.width>0) value+=L"\nResolution: "+std::to_wstring(state.width)+L" \u00d7 "+std::to_wstring(state.height);
        value+=L"\nAudio: "+fromUtf8(state.audioCodec)+L"\nDuration: "+fromUtf8(formatTime(state.duration));
        value+=L"\nTracks: "+std::to_wstring(state.tracks.size())+L"\nChapters: "+std::to_wstring(state.chapters.size());
        MessageBoxW(window_,value.c_str(),L"Media information",MB_OK | MB_ICONINFORMATION); break;
    }
    case ShortcutHelp:
        MessageBoxW(window_,L"Ctrl+O  Open files\nCtrl+Shift+O  Open folder\nCtrl+L  Open URL\nCtrl+Shift+S  Add subtitles\n\n"
            L"Space  Play / pause\nLeft / Right  Seek 5 seconds\nUp / Down  Adjust volume\nShift+Left / Right  Previous / next file\n"
            L"Page Up / Down  Previous / next chapter\n.  Frame step\n[ / ]  Change speed\nBackspace  Normal speed\n\n"
            L"F or Alt+Enter  Fullscreen\nEscape  Leave fullscreen\nP  Playlist / chapters\nCtrl+M  Compact window\nT  Always on top\n"
            L"M  Mute\nJ  Cycle subtitles\nK  Cycle audio\nV  Subtitle visibility\nZ / X  Subtitle delay\nR  Repeat file\nL  A-B loop\n"
            L"S  Screenshot\nCtrl+I  Media information\n?  Keyboard shortcuts\n\nTab / Shift+Tab  Move through controls\nEnter  Activate focused control\n"
            L"Alt+F / P / V / A / S  Open menus\nDrop files / folders to play; Shift+drop to append.",
            L"Keyboard shortcuts",MB_OK | MB_ICONINFORMATION); break;
    case About: {
        std::wstring value=L"Velin 0.1.0\nA native Windows media player inspired by IINA.\n\nC++20 \u00b7 Win32 \u00b7 Direct2D \u00b7 libmpv\n"
            L"Licensed under GNU GPL v3 or later.\n\nIINA: https://github.com/iina/iina\nmpv: https://mpv.io\n\n"
            L"Playback runtime: libmpv-2.dll must sit beside velin.exe.\nRun scripts/Get-Mpv.ps1 before building to install it.\n\n"
            L"Settings, playback resume, and screenshots:\n%LOCALAPPDATA%\\Velin";
        if (!engine_.ready()) value+=L"\n\n"+fromUtf8(engine_.error());
        MessageBoxW(window_,value.c_str(),L"About Velin",MB_OK | MB_ICONINFORMATION); break;
    }
    case Quit: SendMessageW(window_,WM_CLOSE,0,0); return;
    default: break;
    }
    if (window_ && !closing_) InvalidateRect(window_,nullptr,FALSE);
}

void PlayerWindow::showMenu(int action,POINT point) {
    HMENU menu=CreatePopupMenu();
    const auto& state=engine_.state();
    auto item=[&](int id,const std::wstring& label,bool checked=false,bool enabled=true) {
        static const std::map<int,std::string> commands{{OpenFiles,"open-files"},{OpenFolder,"open-folder"},{OpenUrl,"open-url"},
            {TogglePause,"play-pause"},{Stop,"stop"},{Next,"next"},{Previous,"previous"},{ToggleFullscreen,"fullscreen"},
            {PictureInPicture,"pip"},{MusicMode,"music-mode"},{TogglePlaylist,"playlist"},{PreferencesAction,"preferences"},
            {HistoryAction,"history"},{Screenshot,"screenshot"},{ToggleMute,"mute"},{CropVideo,"crop"},{ShowInfo,"inspector"},
            {OnlineSubtitles,"online-subtitles"},{PluginManager,"plugins"},{AddSubtitle,"add-subtitles"},
            {SavePlaylist,"save-playlist"},{ShufflePlaylist,"shuffle"},{ZoomIn,"zoom-in"},{ZoomOut,"zoom-out"}};
        auto display=label;
        if (const auto command=commands.find(id);command!=commands.end()) {
            const auto tab=display.find(L'\t');
            const auto defaultKey=tab==std::wstring::npos?std::string{}:toUtf8(display.substr(tab+1));
            if(tab!=std::wstring::npos)display.resize(tab);
            const auto keys=shortcutKeys(defaultKey,command->second,preferences_.keyBindings);
            for(std::size_t i=0;i<keys.size();++i)display+=(i==0?L"\t":L", ")+fromUtf8(keys[i]);
        }
        AppendMenuW(menu,MF_STRING | (checked?MF_CHECKED:0) | (enabled?MF_ENABLED:MF_GRAYED),static_cast<UINT_PTR>(id),display.c_str());
    };
    auto separator=[&]() { AppendMenuW(menu,MF_SEPARATOR,0,nullptr); };
    auto speedMenu=[&]() {
        constexpr double speeds[]={.25,.5,.75,1,1.25,1.5,1.75,2,2.5,3,4};
        for (int i=0;i<11;++i) item(SpeedBase+i,number(speeds[i])+L"\u00d7"+(i==3?L"  Normal":L""),std::abs(state.speed-speeds[i])<.01);
    };
    if (action==FileMenu) {
        item(NewWindow,L"New player window");item(OpenClipboard,L"Open clipboard URL");item(OpenDisc,L"Open DVD / Blu-ray folder...");
        item(PreferencesAction,L"Preferences..."); item(HistoryAction,L"Playback history...");
        item(LoadPlaylist,L"Open playlist..."); item(SavePlaylist,L"Save playlist...");
        item(PluginManager,L"Plugins..."); separator();
        if (!pluginMenus_.empty()) {
            HMENU extensions=CreatePopupMenu();
            appendPluginMenu(extensions,pluginMenus_,PluginMenuBase);
            AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(extensions),L"Plugin commands");
        }
        item(OpenFiles,L"Open files...\tCtrl+O"); item(OpenFolder,L"Open folder...\tCtrl+Shift+O");
        item(OpenUrl,L"Open URL...\tCtrl+L"); item(AddFiles,L"Add to playlist..."); separator();
        item(RevealFile,L"Show current file in Explorer",false,!state.idle);item(TrashFile,L"Move current file to Recycle Bin...",false,!state.idle);
        HMENU recent=CreatePopupMenu();
        for (size_t i=0;i<preferences_.recentFiles.size();++i)
            AppendMenuW(recent,MF_STRING,RecentBase+i,menuLabel(mediaTitle(preferences_.recentFiles[i])).c_str());
        if (preferences_.recentFiles.empty()) AppendMenuW(recent,MF_STRING | MF_GRAYED,0,L"No recent files");
        else { AppendMenuW(recent,MF_SEPARATOR,0,nullptr); AppendMenuW(recent,MF_STRING,ClearHistory,L"Clear recent files"); }
        AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(recent),L"Recent files");
        separator(); item(ToggleResume,L"Remember playback position",preferences_.rememberPosition);
        item(OpenSettings,L"Open application data folder"); separator();
        item(ShortcutHelp,L"Keyboard shortcuts\t?"); item(About,L"About Velin"); separator(); item(Quit,L"Exit\tAlt+F4");
    } else if (action==PlaybackMenu) {
        item(TogglePause,state.paused?L"Play\tSpace":L"Pause\tSpace",false,!state.idle);
        item(Stop,L"Stop",false,!state.idle); separator();
        item(SeekBack,L"Seek backward 5 seconds\tLeft",false,!state.idle);
        item(SeekForward,L"Seek forward 5 seconds\tRight",false,!state.idle);
        item(FrameBack,L"Previous frame",false,!state.idle);
        item(FrameStep,L"Next frame\t.",false,!state.idle);
        item(GoToTime,L"Go to time...",false,!state.idle);
        item(JumpToBeginning,L"Jump to beginning",false,!state.idle); separator();
        item(Previous,L"Previous file\tShift+Left",false,!state.playlist.empty());
        item(Next,L"Next file\tShift+Right",false,!state.playlist.empty());
        HMENU chapters=CreatePopupMenu();
        for (size_t i=0;i<std::min<size_t>(state.chapters.size(),999);++i) {
            const auto& chapter=state.chapters[i];
            std::wstring label=fromUtf8(formatTime(chapter.time))+L"   "+menuLabel(chapter.title.empty()?"Chapter "+std::to_string(i+1):chapter.title);
            AppendMenuW(chapters,MF_STRING,ChapterBase+i,label.c_str());
        }
        if (state.chapters.empty()) AppendMenuW(chapters,MF_STRING | MF_GRAYED,0,L"No chapters");
        AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(chapters),L"Chapters"); separator();
        HMENU speeds=CreatePopupMenu(); HMENU saved=menu; menu=speeds; speedMenu(); menu=saved;
        AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(speeds),L"Playback speed");
        item(ToggleRepeat,L"Repeat current file\tR",repeating_); item(TogglePlaylistLoop,L"Repeat playlist",playlistLoop_);
        item(LoopAB,L"Set / clear A-B loop\tL",false,!state.idle);
    } else if (action==VideoMenu) {
        item(ToggleDeinterlace,L"Deinterlace",preferences_.deinterlace);item(ToggleDeband,L"Reduce color banding",preferences_.deband);
        item(FlipVideo,L"Flip vertically");item(MirrorVideo,L"Mirror horizontally");
        item(ZoomIn,L"Zoom in");item(ZoomOut,L"Zoom out");item(ResetPan,L"Reset zoom and position");
        HMENU tracks=CreatePopupMenu();
        AppendMenuW(tracks,MF_STRING,VideoTrackBase+999,L"Off");
        for(size_t i=0;i<std::min<size_t>(999,state.tracks.size());++i)if(state.tracks[i].type=="video"){
            const auto& track=state.tracks[i];const auto label=track.title.empty()?L"Video "+std::to_wstring(track.id):menuLabel(track.title);
            AppendMenuW(tracks,MF_STRING|(track.selected?MF_CHECKED:0),VideoTrackBase+i,label.c_str());
        }
        AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(tracks),L"Video track");
        item(VideoSettings,L"Video adjustments...");
        item(VideoFilters,L"Manage video filters...");
        HMENU crops=CreatePopupMenu();
        const wchar_t* cropLabels[]={L"16:9",L"4:3",L"1:1",L"2.35:1"};
        for (int i=0;i<4;++i) AppendMenuW(crops,MF_STRING,CropAspectBase+i,cropLabels[i]);
        AppendMenuW(crops,MF_SEPARATOR,0,nullptr);
        AppendMenuW(crops,MF_STRING,CustomCropAspect,L"Custom aspect ratio...");
        AppendMenuW(crops,MF_STRING,CropVideo,L"Crop interactively...");
        AppendMenuW(crops,MF_STRING,ClearCrop,L"Remove crop");
        AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(crops),L"Crop");
        item(PictureInPicture,L"Picture in picture",pip_); item(RecognizeText,L"Recognize text in frame...");
        item(ToggleFullscreen,L"Fullscreen\tF",fullscreen_); item(ToggleTopmost,L"Always on top\tT",preferences_.alwaysOnTop); separator();
        item(Rotate,L"Rotate 90 degrees",false,!state.idle);
        HMENU aspects=CreatePopupMenu();
        const wchar_t* labels[]={L"Original",L"16:9",L"4:3",L"1:1",L"2.35:1"};
        for (int i=0;i<5;++i) AppendMenuW(aspects,MF_STRING,AspectBase+i,labels[i]);
        AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(aspects),L"Aspect ratio");
        HMENU sizes=CreatePopupMenu();
        const wchar_t* sizeLabels[]={L"Half size",L"Actual size",L"Double size",L"Fit to screen"};
        const bool canResize=state.width>0 && state.height>0 && !fullscreen_ && !pip_ && !musicMode_;
        for (int i=0;i<4;++i) AppendMenuW(sizes,MF_STRING|(canResize?0:MF_GRAYED),WindowSizeBase+i,sizeLabels[i]);
        AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(sizes),L"Window size");
        item(ToggleWindowAspectLock,L"Lock video aspect while resizing",preferences_.lockWindowAspect);
        item(ResetVideo,L"Reset video adjustments"); separator(); item(Screenshot,L"Save screenshot\tS",false,!state.idle);
        item(ShowInfo,L"Media information\tCtrl+I",false,!state.idle);
    } else if (action==AudioMenu || action==SubtitleMenu) {
        const bool subtitles=action==SubtitleMenu;
        if(subtitles){
            HMENU secondary=CreatePopupMenu();AppendMenuW(secondary,MF_STRING|(state.secondarySubtitle<=0?MF_CHECKED:0),SecondarySubtitleBase+999,L"Off");
            for(size_t i=0;i<std::min<size_t>(999,state.tracks.size());++i)if(state.tracks[i].type=="sub"){
                const auto& track=state.tracks[i];const auto label=track.title.empty()?L"Subtitle "+std::to_wstring(track.id):menuLabel(track.title);
                AppendMenuW(secondary,MF_STRING|(state.secondarySubtitle==track.id?MF_CHECKED:0),SecondarySubtitleBase+i,label.c_str());
            }
            AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(secondary),L"Secondary subtitles");
        }else{
            HMENU devices=CreatePopupMenu();
            for(size_t i=0;i<std::min<size_t>(999,state.audioDevices.size());++i){const auto& device=state.audioDevices[i];AppendMenuW(devices,MF_STRING|(state.audioDevice==device.name?MF_CHECKED:0),AudioDeviceBase+i,menuLabel(device.description.empty()?device.name:device.description).c_str());}
            AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(devices),L"Audio output device");
        }
        item(subtitles?SubtitleSettings:AudioSettings,subtitles?L"Subtitle settings...":L"Audio settings and equalizer...");
        if (!subtitles) item(AudioFilters,L"Manage audio filters...");
        if (subtitles) item(OnlineSubtitles,L"Search online subtitles...");
        else { item(ExternalAudio,L"Add external audio..."); item(MusicMode,L"Music mode",musicMode_); }
        if (subtitles) {
            item(AddSubtitle,L"Add subtitle file...\tCtrl+Shift+S",false,!state.idle);
            item(ToggleSubtitles,L"Toggle subtitle visibility\tV",false,!state.idle);
            item(ToggleSecondarySubtitles,L"Toggle secondary subtitle visibility",false,!state.idle); separator();
            const bool selected=std::any_of(state.tracks.begin(),state.tracks.end(),[](const Track& track) { return track.type=="sub" && track.selected; });
            item(SubtitleBase+999,L"Off",!selected);
        } else {
            item(ToggleMute,L"Mute\tM",state.muted); item(VolumeUp,L"Volume up\tUp"); item(VolumeDown,L"Volume down\tDown"); separator();
        }
        int count=0;
        for (size_t i=0;i<std::min<size_t>(state.tracks.size(),999);++i) {
            const auto& track=state.tracks[i];
            if (track.type!=(subtitles?"sub":"audio")) continue;
            auto label=track.title.empty()?(subtitles?L"Subtitle ":L"Audio ")+std::to_wstring(track.id):menuLabel(track.title);
            if (!track.language.empty()) label+=L"  ["+menuLabel(track.language)+L"]";
            if (!track.codec.empty()) label+=L"  "+menuLabel(track.codec);
            if (track.external) label+=L"  (external)";
            item((subtitles?SubtitleBase:AudioBase)+static_cast<int>(i),label,track.selected); ++count;
        }
        if (!count) item(None,subtitles?L"No subtitle tracks":L"No audio tracks",false,false);
        separator();
        item(subtitles?SubtitleDelayDown:AudioDelayDown,L"Delay \u2212 0.1 seconds");
        item(subtitles?SubtitleDelayUp:AudioDelayUp,L"Delay + 0.1 seconds");
    } else if (action==ViewMenu) {
        item(CheckUpdates,L"Check for updates..."); item(RegisterAssociations,L"Set up file associations...");
        item(PictureInPicture,L"Picture in picture",pip_); item(MusicMode,L"Music mode",musicMode_);
        item(ShufflePlaylist,L"Shuffle playlist"); item(UnshufflePlaylist,L"Restore playlist order");
        item(MediaLog,L"Playback and plugin log..."); item(AdvancedSettings,L"Advanced settings...");
        item(TogglePlaylist,L"Playlist / chapters\tP",showPlaylist_);
        item(Compact,L"Compact window\tCtrl+M",compact_); item(ToggleFullscreen,L"Fullscreen\tF",fullscreen_);
        item(ToggleTopmost,L"Always on top\tT",preferences_.alwaysOnTop); separator();
        item(ShowInfo,L"Media information\tCtrl+I",false,!state.idle); item(ShortcutHelp,L"Keyboard shortcuts\t?");
    } else if (action==SpeedMenu) speedMenu();
    const UINT selected=TrackPopupMenuEx(menu,TPM_RETURNCMD | TPM_RIGHTBUTTON,point.x,point.y,window_,nullptr);
    DestroyMenu(menu);
    if (selected) execute(static_cast<int>(selected));
}

void PlayerWindow::keyDown(WPARAM key, bool released) {
    const bool control=(GetKeyState(VK_CONTROL)&0x8000)!=0, shift=(GetKeyState(VK_SHIFT)&0x8000)!=0;
    std::string keyName;
    if (key>='A' && key<='Z') keyName=static_cast<char>(key);
    else if (key>='0' && key<='9') keyName=static_cast<char>(key);
    else {
        static const std::map<WPARAM,std::string> names{{VK_SPACE,"Space"},{VK_RETURN,"Enter"},{VK_ESCAPE,"Escape"},
            {VK_LEFT,"Left"},{VK_RIGHT,"Right"},{VK_UP,"Up"},{VK_DOWN,"Down"},{VK_TAB,"Tab"},
            {VK_BACK,"Backspace"},{VK_DELETE,"Delete"},{VK_HOME,"Home"},{VK_END,"End"},{VK_PRIOR,"PageUp"},{VK_NEXT,"PageDown"},
            {VK_OEM_PERIOD,"."},{VK_OEM_COMMA,","},{VK_OEM_4,"["},{VK_OEM_6,"]"},{VK_OEM_1,";"},
            {VK_OEM_2,"/"},{VK_OEM_3,"`"},{VK_OEM_5,"\\"},{VK_OEM_7,"'"},{VK_OEM_MINUS,"-"},{VK_OEM_PLUS,"="}};
        if (auto found=names.find(key);found!=names.end()) keyName=found->second;
        else if (key>=VK_F1 && key<=VK_F24) keyName="F"+std::to_string(key-VK_F1+1);
    }
    if (!keyName.empty()) {
        std::string binding=(control?"Ctrl+":"");
        if (GetKeyState(VK_MENU)&0x8000) binding+="Alt+";
        if (shift) binding+="Shift+";
        binding=normalizeKeyBinding(binding+keyName);
        if (plugins_ && plugins_->input(released?"keyUp":"keyDown",binding)) return;
        if (released) return;
        if (auto found=preferences_.keyBindings.find(binding);found!=preferences_.keyBindings.end()) { dispatchCommand(found->second); return; }
    }
    if (released) return;
    if (control) {
        switch(key) {
        case 'O': execute(shift?OpenFolder:OpenFiles); return;
        case 'L': execute(OpenUrl); return;
        case 'S': if (shift) execute(AddSubtitle); return;
        case 'I': execute(ShowInfo); return;
        case 'M': execute(Compact); return;
        default: return;
        }
    }
    switch(key) {
    case VK_TAB:
        if (!hits_.empty()) {
            hovered_=shift?(hovered_<=0?static_cast<int>(hits_.size())-1:hovered_-1):(hovered_+1)%static_cast<int>(hits_.size());
            InvalidateRect(window_,nullptr,FALSE);
            accessibility_.notifyFocus(hovered_+1);
        }
        break;
    case VK_RETURN:
        if (hovered_>=0 && hovered_<static_cast<int>(hits_.size())) { const auto hit=hits_[static_cast<size_t>(hovered_)]; execute(hit.action,hit.data); }
        break;
    case VK_SPACE: execute(TogglePause); break;
    case VK_LEFT: execute(shift?Previous:SeekBack); break;
    case VK_RIGHT: execute(shift?Next:SeekForward); break;
    case VK_UP: execute(VolumeUp); break;
    case VK_DOWN: execute(VolumeDown); break;
    case VK_PRIOR: execute(PreviousChapter); break;
    case VK_NEXT: execute(NextChapter); break;
    case VK_HOME: if (!engine_.state().idle) engine_.seek(0,true); break;
    case VK_END: if (!engine_.state().idle) engine_.seek(engine_.state().duration,true); break;
    case VK_ESCAPE: uiHidden_=false;if (pip_) togglePip(); else if (fullscreen_) toggleFullscreen();layoutVideo();InvalidateRect(window_,nullptr,FALSE); break;
    case 'F': execute(ToggleFullscreen); break;
    case 'P': execute(TogglePlaylist); break;
    case 'M': execute(ToggleMute); break;
    case 'T': execute(ToggleTopmost); break;
    case 'J': execute(CycleSubtitle); break;
    case 'K': execute(CycleAudio); break;
    case 'V': execute(ToggleSubtitles); break;
    case 'S': execute(Screenshot); break;
    case 'R': execute(ToggleRepeat); break;
    case 'L': execute(LoopAB); break;
    case 'Z': execute(SubtitleDelayDown); break;
    case 'X': execute(SubtitleDelayUp); break;
    case VK_OEM_PERIOD: execute(FrameStep); break;
    case VK_OEM_4: engine_.setSpeed(engine_.state().speed-.25); break;
    case VK_OEM_6: engine_.setSpeed(engine_.state().speed+.25); break;
    case VK_BACK: engine_.setSpeed(1); break;
    case VK_OEM_2: if (shift) execute(ShortcutHelp); break;
    case VK_F10: execute(FileMenu); break;
    default: break;
    }
}

} // namespace velin
