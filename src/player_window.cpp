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
auto background = D2D1::ColorF(0x101116);
auto panel = D2D1::ColorF(0x171920);
auto border = D2D1::ColorF(0x292c37);
auto foreground = D2D1::ColorF(0xf1f0f7);
auto secondary = D2D1::ColorF(0x999cab);
auto muted = D2D1::ColorF(0x626777);
auto accent = D2D1::ColorF(0xc2afff);
auto green = D2D1::ColorF(0x85cfa7);
bool lightPalette=false;
auto shade(unsigned value) {
    auto color=D2D1::ColorF(value);
    if (lightPalette) { color.r=1.f-color.r*.32f; color.g=1.f-color.g*.32f; color.b=1.f-color.b*.28f; }
    return color;
}
const auto errorColor = D2D1::ColorF(0xffb4ae);
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
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
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
    COLORREF captionColor = RGB(16,17,22);
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
        if (plugins_ && (message.hwnd==window_ || message.hwnd==video_)) {
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
        case WM_MOUSEMOVE:
            self->interactionUntil_=GetTickCount64()+2500;
            if (!self->controlsVisible_) { self->controlsVisible_=true; self->layoutVideo(); InvalidateRect(self->window_,nullptr,FALSE); }
            break;
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
    if (FAILED(target_->CreateSolidColorBrush(foreground, &brush_))) discardTarget();
}

void PlayerWindow::discardTarget() { release(brush_); release(target_); }

void PlayerWindow::fill(D2D1_RECT_F bounds, D2D1_COLOR_F color, float radius) {
    brush_->SetColor(color);
    if (radius > 0) target_->FillRoundedRectangle(D2D1::RoundedRect(bounds, radius, radius), brush_);
    else target_->FillRectangle(bounds, brush_);
}

void PlayerWindow::line(float x1, float y1, float x2, float y2, D2D1_COLOR_F color, float thickness) {
    brush_->SetColor(color);
    target_->DrawLine(D2D1::Point2F(x1,y1), D2D1::Point2F(x2,y2), brush_, thickness);
}

void PlayerWindow::text(std::wstring_view value, D2D1_RECT_F bounds, float size,
                        D2D1_COLOR_F color, bool bold, DWRITE_TEXT_ALIGNMENT alignment) {
    const auto key=std::make_tuple(static_cast<int>(size*100),bold,static_cast<int>(alignment));
    IDWriteTextFormat* format=nullptr;
    auto existing=textFormats_.find(key);
    if (existing!=textFormats_.end()) format=existing->second;
    else {
    if (FAILED(writeFactory_->CreateTextFormat(L"Segoe UI", nullptr,
        bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-US", &format))) return;
    format->SetTextAlignment(alignment);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    IDWriteInlineObject* ellipsis = nullptr;
    if (SUCCEEDED(writeFactory_->CreateEllipsisTrimmingSign(format, &ellipsis))) {
        format->SetTrimming(&trimming, ellipsis); ellipsis->Release();
    }
    textFormats_.emplace(key,format);
    }
    brush_->SetColor(color);
    target_->DrawText(value.data(), static_cast<UINT32>(value.size()), format, bounds, brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void PlayerWindow::applyTheme() {
    bool light=preferences_.theme==Theme::Light;
    if (preferences_.theme==Theme::System) {
        DWORD value=0,size=sizeof(value);
        if (RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",L"AppsUseLightTheme",RRF_RT_REG_DWORD,nullptr,&value,&size)==ERROR_SUCCESS) light=value!=0;
    }
    lightPalette=light;
    background=D2D1::ColorF(light?0xf5f6fa:0x101116); panel=D2D1::ColorF(light?0xffffff:0x171920);
    border=D2D1::ColorF(light?0xd8dbe5:0x292c37); foreground=D2D1::ColorF(light?0x202430:0xf1f0f7);
    secondary=D2D1::ColorF(light?0x5b6374:0x999cab); muted=D2D1::ColorF(light?0x737e91:0x626777);
    accent=D2D1::ColorF(light?0x7042b4:0xc2afff); green=D2D1::ColorF(light?0x326e4b:0x85cfa7);
    BOOL dark=!light; DwmSetWindowAttribute(window_,20,&dark,sizeof(dark));
    COLORREF caption=light?RGB(245,246,250):RGB(16,17,22); DwmSetWindowAttribute(window_,35,&caption,sizeof(caption));
    InvalidateRect(window_,nullptr,FALSE);
}

void PlayerWindow::icon(int action, D2D1_RECT_F bounds, D2D1_COLOR_F color) {
    const float cx = (bounds.left+bounds.right)/2, cy = (bounds.top+bounds.bottom)/2;
    auto stroke = [&](float x1,float y1,float x2,float y2) { line(cx+x1,cy+y1,cx+x2,cy+y2,color,1.7f); };
    auto triangle = [&](float x, float y, float size, bool right) {
        ID2D1PathGeometry* geometry = nullptr; ID2D1GeometrySink* sink = nullptr;
        if (FAILED(factory_->CreatePathGeometry(&geometry))) return;
        if (SUCCEEDED(geometry->Open(&sink))) {
            sink->BeginFigure(D2D1::Point2F(cx+x-size/2,cy+y-size), D2D1_FIGURE_BEGIN_FILLED);
            if (right) {
                sink->AddLine(D2D1::Point2F(cx+x+size,cy+y));
                sink->AddLine(D2D1::Point2F(cx+x-size/2,cy+y+size));
            } else {
                sink->AddLine(D2D1::Point2F(cx+x-size/2,cy+y+size));
                sink->AddLine(D2D1::Point2F(cx+x-size*2,cy+y));
            }
            sink->EndFigure(D2D1_FIGURE_END_CLOSED); sink->Close();
            brush_->SetColor(color); target_->FillGeometry(geometry,brush_); sink->Release();
        }
        geometry->Release();
    };
    switch(action) {
    case TogglePause:
        if (!engine_.state().idle && !engine_.state().paused) {
            fill(rect(cx-6,cy-8,4,16),color,1); fill(rect(cx+2,cy-8,4,16),color,1);
        } else triangle(0,0,8,true);
        break;
    case Previous: triangle(1,0,6,false); stroke(-10,-7,-10,7); break;
    case Next: triangle(-3,0,6,true); stroke(9,-7,9,7); break;
    case ToggleMute:
        stroke(-10,-3,-5,-3); stroke(-5,-3,0,-8); stroke(0,-8,0,8);
        stroke(0,8,-5,3); stroke(-5,3,-10,3); stroke(-10,3,-10,-3);
        if (engine_.state().muted) { stroke(5,-4,12,4); stroke(5,4,12,-4); }
        else { stroke(5,-5,8,-2); stroke(8,-2,8,2); stroke(8,2,5,5); stroke(10,-8,14,-4); stroke(14,-4,14,4); stroke(14,4,10,8); }
        break;
    case ToggleFullscreen:
        stroke(-9,-3,-9,-8); stroke(-9,-8,-3,-8); stroke(3,-8,9,-8); stroke(9,-8,9,-3);
        stroke(-9,3,-9,8); stroke(-9,8,-3,8); stroke(3,8,9,8); stroke(9,8,9,3); break;
    case TogglePlaylist:
        for (int i=-1;i<=1;++i) { stroke(-7,static_cast<float>(i*6),9,static_cast<float>(i*6)); fill(rect(cx-12,cy+static_cast<float>(i*6)-1,2,2),color,1); } break;
    case ToggleRepeat:
        stroke(-8,-5,8,-5); stroke(8,-5,8,1); stroke(8,-5,4,-9); stroke(8,-5,4,-1);
        stroke(8,5,-8,5); stroke(-8,5,-8,-1); stroke(-8,5,-4,9); stroke(-8,5,-4,1); break;
    case Screenshot:
        brush_->SetColor(color); target_->DrawRoundedRectangle(D2D1::RoundedRect(rect(cx-10,cy-6,20,14),2,2),brush_,1.5f);
        target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy+1),4,4),brush_,1.5f);
        stroke(-4,-6,-2,-10); stroke(-2,-10,3,-10); stroke(3,-10,5,-6); break;
    case SubtitleMenu:
        brush_->SetColor(color); target_->DrawRoundedRectangle(D2D1::RoundedRect(rect(cx-11,cy-8,22,16),2,2),brush_,1.5f);
        text(L"CC",rect(cx-10,cy-9,20,18),10,color,true,DWRITE_TEXT_ALIGNMENT_CENTER); break;
    case AddFiles: stroke(-7,0,7,0); stroke(0,-7,0,7); break;
    case OpenFiles: case OpenFolder:
        stroke(-11,-5,-11,9); stroke(-11,9,11,9); stroke(11,9,11,-5); stroke(11,-5,-11,-5);
        stroke(-11,-5,-11,-9); stroke(-11,-9,-3,-9); stroke(-3,-9,0,-5); break;
    default: text(L"\u2022\u2022\u2022",bounds,12,color,false,DWRITE_TEXT_ALIGNMENT_CENTER); break;
    }
}

void PlayerWindow::button(int action, D2D1_RECT_F bounds, std::wstring_view label,
                          std::wstring hint, bool primary, int data) {
    const bool hover = hovered_ == static_cast<int>(hits_.size());
    fill(bounds, primary ? accent : (hover ? shade(0x2c2e3c) : shade(0x22252e)), 7);
    text(label,bounds,13,primary ? background : foreground,true,DWRITE_TEXT_ALIGNMENT_CENTER);
    hits_.push_back({bounds,action,data,std::move(hint)});
}

void PlayerWindow::iconButton(int action, D2D1_RECT_F bounds, std::wstring hint, bool active) {
    const bool hover = hovered_ == static_cast<int>(hits_.size());
    if (hover || active) fill(bounds,active ? shade(0x302a43) : shade(0x242630),7);
    icon(action,bounds,active ? accent : (hover ? foreground : secondary));
    hits_.push_back({bounds,action,0,std::move(hint)});
}

float PlayerWindow::sidebarWidth() const { const float side=std::clamp(static_cast<float>(preferences_.sidebarWidth),240.f,640.f);return showPlaylist_ && width_>=side+534.f?side:0.f; }
D2D1_RECT_F PlayerWindow::videoRect() const {
    if(uiHidden_ && !engine_.state().idle)return {0,0,width_,height_};
    if (pip_) return {0,0,width_,height_-(controlsVisible_?52.f:0.f)};
    if (fullscreen_ && !engine_.state().idle) return {0,0,width_,height_-(controlsVisible_?128.f:0.f)};
    const float side = sidebarWidth();
    return {24.f,76.f,width_-24.f-side,height_-146.f};
}

void PlayerWindow::draw() {
    createResources();
    if (!target_ || !brush_) return;
    const auto& state = engine_.state();
    hits_.clear();
    target_->BeginDraw();
    target_->Clear(background);
    if(uiHidden_ && !state.idle){if(target_->EndDraw()==D2DERR_RECREATE_TARGET)discardTarget();updateAccessibility();return;}
    if (pip_) {
        if (controlsVisible_) {
            iconButton(TogglePause,rect(width_/2-20,height_-46,40,40),L"Play / pause  (Space)");
            iconButton(Previous,rect(width_/2-76,height_-42,32,32),L"Previous item");
            iconButton(Next,rect(width_/2+44,height_-42,32,32),L"Next item");
            button(PictureInPicture,rect(width_-66,height_-39,58,26),L"Restore",L"Leave picture in picture  (Esc)");
            iconButton(ToggleMute,rect(8,height_-42,32,32),L"Mute  (M)",state.muted);
        }
        if (target_->EndDraw()==D2DERR_RECREATE_TARGET) discardTarget();
        updateAccessibility(); return;
    }
    if (fullscreen_ && !state.idle && !controlsVisible_) {
        if (target_->EndDraw()==D2DERR_RECREATE_TARGET) discardTarget();
        return;
    }
    // The entire interface uses device-independent pixels and real DirectWrite text.
    if (!fullscreen_ || state.idle) {
    fill(rect(24,20,28,28),accent,8);
    line(31,28,38,41,background,2.5f); line(38,41,46,27,background,2.5f);
    text(L"velin",rect(63,14,95,37),25,foreground,true);
    const std::array<std::pair<int,const wchar_t*>,6> menus{{{FileMenu,L"File"},{PlaybackMenu,L"Playback"},
        {VideoMenu,L"Video"},{AudioMenu,L"Audio"},{SubtitleMenu,L"Subtitles"},{ViewMenu,L"View"}}};
    float menuX = 173;
    for (const auto& [action,label] : menus) {
        float w = action == PlaybackMenu ? 78.f : action == SubtitleMenu ? 78.f : 57.f;
        if (menuX+w > width_-95) break;
        auto bounds = rect(menuX,19,w,32);
        if (hovered_ == static_cast<int>(hits_.size())) fill(bounds,shade(0x242630),6);
        text(label,bounds,12,secondary,false,DWRITE_TEXT_ALIGNMENT_CENTER);
        hits_.push_back({bounds,action,0,L"Open menu"}); menuX += w;
    }
    iconButton(TogglePlaylist,rect(width_-59,17,36,36),L"Playlist / chapters  (P)",showPlaylist_);
    line(24,63,width_-24,63,border);
    }
    const auto view = videoRect();
    fill(view,shade(0x15171e),12);
    if (state.idle) {
        const float center = (view.left+view.right)/2;
        const float areaHeight = view.bottom-view.top;
        const bool smallLayout = areaHeight < 380;
        const float top = view.top + std::max(18.f,(areaHeight-(preferences_.recentFiles.empty()?310.f:380.f))/2);
        if (!smallLayout) {
            // Original geometric Velin mark; no IINA artwork is reused.
            fill(rect(center-32,top,64,64),shade(0x242333),20);
            line(center-18,top+20,center,top+46,accent,4);
            line(center,top+46,center+18,top+18,accent,4);
        }
        const float headingY = smallLayout ? top : top+84;
        text(L"Make room for the movie.",rect(view.left+12,headingY,view.right-view.left-24,45),
            smallLayout ? 24.f : 30.f,foreground,true,DWRITE_TEXT_ALIGNMENT_CENTER);
        text(L"Your videos, music, and a moment to yourself.",rect(view.left+12,headingY+48,view.right-view.left-24,25),
            13,secondary,false,DWRITE_TEXT_ALIGNMENT_CENTER);
        button(OpenFiles,rect(center-149,headingY+94,154,42),L"Open files",L"Open media files  (Ctrl+O)",true);
        button(OpenUrl,rect(center+17,headingY+94,132,42),L"Open URL",L"Open a network stream  (Ctrl+L)");
        text(L"or drop files or a folder anywhere",rect(view.left+10,headingY+151,view.right-view.left-20,24),
            12,muted,false,DWRITE_TEXT_ALIGNMENT_CENTER);
        if (!smallLayout && !preferences_.recentFiles.empty()) {
            float y = headingY+201;
            if (y+55 < view.bottom-20) {
                text(L"PICK UP WHERE YOU LEFT OFF",rect(view.left+32,y,view.right-view.left-64,20),10,muted,true);
                for (size_t i=0;i<std::min<size_t>(2,preferences_.recentFiles.size());++i) {
                    auto bounds = rect(view.left+24,y+26+static_cast<float>(i)*37,view.right-view.left-48,34);
                    if (bounds.bottom > view.bottom-12) break;
                    if (hovered_ == static_cast<int>(hits_.size())) fill(bounds,shade(0x22252e),6);
                    text(fromUtf8(mediaTitle(preferences_.recentFiles[i])),rect(bounds.left+10,bounds.top,bounds.right-bounds.left-75,34),12,secondary);
                    text(L"Play  \u2197",rect(bounds.right-66,bounds.top,56,34),11,accent);
                    hits_.push_back({bounds,RecentItem,static_cast<int>(i),L"Open recent media"});
                }
            }
        }
    }
    if (!state.idle && state.loaded && state.width==0) {
        const float cx=(view.left+view.right)/2,cy=(view.top+view.bottom)/2-32;
        brush_->SetColor(shade(0x262334));
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy-24),52,52),brush_);
        brush_->SetColor(shade(0x3e3555));
        for (float radius:{39.f,30.f,21.f}) target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy-24),radius,radius),brush_,1.2f);
        brush_->SetColor(accent); target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy-24),9,9),brush_);
        text(fromUtf8(state.title.empty()?mediaTitle(state.path):state.title),rect(view.left+24,cy+50,view.right-view.left-48,39),23,foreground,true,DWRITE_TEXT_ALIGNMENT_CENTER);
        text(L"AUDIO  \u00b7  "+fromUtf8(state.audioCodec),rect(view.left+24,cy+94,view.right-view.left-48,25),11,accent,false,DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (sidebarWidth()>0 && (!fullscreen_ || state.idle)) {
        const float panelWidth=sidebarWidth()-18;
        const float x = width_-sidebarWidth()-6;
        fill(rect(x,76,panelWidth,height_-222),panel,12);
        text(chapters_ ? L"Chapters" : L"Up next",rect(x+20,89,150,26),16,foreground,true);
        iconButton(AddFiles,rect(x+panelWidth-52,86,32,32),L"Add files to playlist");
        auto tab = [&](int action,float offset,const wchar_t* label,bool active) {
            auto bounds = rect(x+16+offset,130,(panelWidth-40)/2,29);
            if (active) fill(bounds,shade(0x2a2937),6);
            text(label,bounds,11,active?accent:secondary,active,DWRITE_TEXT_ALIGNMENT_CENTER);
            hits_.push_back({bounds,action,0,L"Switch sidebar"});
        };
        tab(PlaylistTab,0,L"Playlist",!chapters_); tab(ChaptersTab,(panelWidth-40)/2+8,L"Chapters",chapters_);
        line(x+16,172,x+panelWidth-16,172,border);
        const int count = static_cast<int>(chapters_?state.chapters.size():state.playlist.size());
        const int visible = std::max(1,static_cast<int>((height_-360)/58));
        listOffset_ = std::clamp(listOffset_,0,std::max(0,count-visible));
        if (count == 0) {
            const float y = 188+std::max(0.f,(height_-460)/2);
            icon(TogglePlaylist,rect(x+112,y,44,44),muted);
            text(chapters_?L"No chapters in this file":L"A good place for what's next.",rect(x+12,y+56,244,25),12,secondary,false,DWRITE_TEXT_ALIGNMENT_CENTER);
            if (y+105<height_-182) text(chapters_?L"Embedded chapters appear here.":L"Add a few files to make a playlist.",rect(x+12,y+83,244,22),11,muted,false,DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            for (int row=0;row<visible && row+listOffset_<count;++row) {
                const int index=row+listOffset_;
                const float y=184+static_cast<float>(row)*58;
                auto bounds=rect(x+10,y,panelWidth-20,53);
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
                if (current || hovered_==static_cast<int>(hits_.size())) fill(bounds,current?shade(0x292639):shade(0x21242d),7);
                if (current) fill(rect(x+10,y+15,3,23),accent,1.5f);
                text(std::to_wstring(index+1),rect(x+22,y+8,25,32),11,current?accent:muted,false,DWRITE_TEXT_ALIGNMENT_CENTER);
                text(title,rect(x+55,y+5,panelWidth-83,23),12,current?foreground:secondary,current);
                text(detail,rect(x+55,y+28,panelWidth-83,18),10,current?accent:muted);
                hits_.push_back({bounds,chapters_?ChapterItem:PlayItem,index,chapters_?L"Seek to chapter":L"Play this item; right-click for playlist actions"});
            }
        }
        text(std::to_wstring(count)+(chapters_?L" chapters":L" items"),rect(x+20,height_-176,150,20),10,muted);
        if (count>visible) text(L"Scroll for more",rect(x+151,height_-176,100,20),10,muted,false,DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    // Transport remains outside the video HWND so it also works with D3D11 output.
    const float seekY=height_-119;
    const auto seekBounds=rect(25,seekY-9,width_-50,23);
    fill(rect(25,seekY,width_-50,3),shade(0x30333e),1.5f);
    const double position=dragAction_==SeekBar?dragPosition_:state.position;
    const float fraction=state.duration>0?static_cast<float>(std::clamp(position/state.duration,0.,1.)):0.f;
    fill(rect(25,seekY,(width_-50)*fraction,3),accent,1.5f);
    if (preferences_.showChapterMarkers && state.duration>0) for (const auto& chapter:state.chapters) {
        if (chapter.time>0 && chapter.time<state.duration) {
            const float x=25+(width_-50)*static_cast<float>(chapter.time/state.duration);
            line(x,seekY-2,x,seekY+5,secondary);
        }
    }
    if (fraction>0 || dragAction_==SeekBar) { brush_->SetColor(accent); target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(25+(width_-50)*fraction,seekY+1.5f),4,4),brush_); }
    hits_.push_back({seekBounds,SeekBar,0,L"Seek in media"});
    text(fromUtf8(formatTime(position)),rect(25,seekY+9,82,20),11,secondary);
    text(fromUtf8(formatTime(state.duration)),rect(width_-107,seekY+9,82,20),11,secondary,false,DWRITE_TEXT_ALIGNMENT_TRAILING);
    const float cy=height_-64;
    iconButton(ToggleMute,rect(23,cy-18,36,36),L"Mute  (M)",state.muted);
    fill(rect(72,cy-1,88,3),shade(0x343743),1.5f);
    fill(rect(72,cy-1,88*static_cast<float>(state.volume/100),3),state.muted?muted:secondary,1.5f);
    brush_->SetColor(state.muted?muted:foreground);
    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(72+88*static_cast<float>(state.volume/100),cy+.5f),4,4),brush_);
    hits_.push_back({rect(65,cy-12,103,26),VolumeBar,0,L"Volume  (Up / Down)"});
    text(std::to_wstring(static_cast<int>(state.volume))+L"%",rect(178,cy-12,45,24),11,muted);
    const float center=width_/2;
    iconButton(Previous,rect(center-95,cy-18,36,36),L"Previous item  (Shift+Left)");
    auto playBounds=rect(center-24,cy-24,48,48);
    fill(playBounds,hovered_==static_cast<int>(hits_.size())?D2D1::ColorF(0xd2c5ff):accent,24);
    icon(TogglePause,playBounds,background);
    hits_.push_back({playBounds,TogglePause,0,state.paused?L"Play  (Space)":L"Pause  (Space)"});
    iconButton(Next,rect(center+59,cy-18,36,36),L"Next item  (Shift+Right)");
    if (width_>=900) button(SpeedMenu,rect(width_-270,cy-15,52,30),number(state.speed)+L"\u00d7",L"Playback speed  ([ / ])");
    iconButton(ToggleRepeat,rect(width_-206,cy-18,36,36),L"Repeat current file  (R)",repeating_);
    iconButton(SubtitleMenu,rect(width_-160,cy-18,36,36),L"Subtitles  (J to cycle)");
    iconButton(Screenshot,rect(width_-114,cy-18,36,36),L"Save screenshot  (S)");
    iconButton(ToggleFullscreen,rect(width_-68,cy-18,36,36),L"Fullscreen  (F)",fullscreen_);
    line(24,height_-28,width_-24,height_-28,border);
    std::wstring status;
    D2D1_COLOR_F statusColor=muted;
    if (GetTickCount64()<noticeUntil_) { status=notice_; statusColor=noticeError_?errorColor:accent; }
    else if (hovered_>=0 && hovered_<static_cast<int>(hits_.size())) status=hits_[static_cast<size_t>(hovered_)].hint;
    else if (!engine_.ready()) { status=L"Playback runtime missing \u00b7 See File > About for setup"; statusColor=errorColor; }
    else if (state.idle) status=L"Ready when you are";
    else status=fromUtf8(state.title.empty()?mediaTitle(state.path):state.title);
    text(status,rect(25,height_-25,width_-210,22),10,statusColor);
    text(engine_.ready()?L"\u25cf  libmpv connected":L"\u25cf  runtime unavailable",rect(width_-184,height_-25,159,22),
        10,engine_.ready()?green:errorColor,false,DWRITE_TEXT_ALIGNMENT_TRAILING);
    const HRESULT result=target_->EndDraw();
    if (result==D2DERR_RECREATE_TARGET) discardTarget();
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
        dragPosition_=std::clamp(static_cast<double>((x-25)/(width_-50)),0.,1.)*engine_.state().duration;
        if (commit) engine_.seek(dragPosition_,true);
    } else if (dragAction_==VolumeBar) {
        engine_.setVolume(std::clamp(static_cast<double>((x-72)/88),0.,1.)*100);
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
        limits->ptMinTrackSize={static_cast<LONG>((pip_?240:740)*dpi_),static_cast<LONG>((pip_?135:500)*dpi_)};
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
            const bool visible=(!fullscreen_ && !pip_) || !preferences_.fullscreenAutoHide || state.idle || state.paused || state.width==0 || GetTickCount64()<interactionUntil_ || hovered_>=0;
            if (controlsVisible_!=visible) { controlsVisible_=visible; layoutVideo(); }
            if (!IsIconic(window_) && ((!state.idle && !state.paused && (!fullscreen_ || controlsVisible_)) || (noticeUntil_>0 && GetTickCount64()>=noticeUntil_))) {
                InvalidateRect(window_,nullptr,FALSE);
                if (GetTickCount64()>=noticeUntil_) noticeUntil_=0;
            }
            if (state.loaded && GetTickCount64()-lastPersist_>=10000) { persistPlayback(); updatePlatform(); }
        }
        return 0;
    case WM_MOUSEMOVE: {
        interactionUntil_=GetTickCount64()+static_cast<ULONGLONG>(preferences_.controlsHideSeconds*1000);
        const float x=static_cast<float>(GET_X_LPARAM(lParam))/dpi_,y=static_cast<float>(GET_Y_LPARAM(lParam))/dpi_;
        if (dragAction_==SeekBar || dragAction_==VolumeBar) updatePointer(x,y,false);
        else {
            const int hit=hitAt(x,y);
            if (hovered_!=hit) { hovered_=hit; InvalidateRect(window_,nullptr,FALSE); }
            if (hit>=0 && hits_[static_cast<size_t>(hit)].action==SeekBar && preferences_.showTimelineThumbnails && engine_.state().duration>0 && thumbnails_.available()) {
                const double time=std::clamp(static_cast<double>((x-25)/(width_-50)),0.,1.)*engine_.state().duration;
                thumbnailAnchor_={GET_X_LPARAM(lParam),static_cast<LONG>((height_-128)*dpi_)}; ClientToScreen(window_,&thumbnailAnchor_);
                if (std::abs(time-thumbnailTime_)>=.5) { thumbnailTime_=time; thumbnails_.request(engine_.state().path,time,static_cast<unsigned>(preferences_.timelineThumbnailWidth)); }
            } else { preview_.hide(); thumbnailTime_=-1; }
            TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,window_,0}; TrackMouseEvent(&tracking);
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
        if (sidebarWidth()>0 && static_cast<float>(point.x)/dpi_>width_-sidebarWidth()-6 && static_cast<float>(point.y)/dpi_<height_-146) {
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
    case WM_MOVE: updateWebViews(); updateAccessibility(); return 0;
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
            POINT point{static_cast<LONG>(175*dpi_),static_cast<LONG>(57*dpi_)}; ClientToScreen(window_,&point);
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
    controlsVisible_=true; interactionUntil_=GetTickCount64()+2500; hovered_=-1;
    if (fullscreen_) {
        placement_.length=sizeof(placement_); GetWindowPlacement(window_,&placement_);
        MONITORINFO monitor{}; monitor.cbSize=sizeof(monitor);
        GetMonitorInfoW(MonitorFromWindow(window_,MONITOR_DEFAULTTONEAREST),&monitor);
        SetWindowLongPtrW(window_,GWL_STYLE,GetWindowLongPtrW(window_,GWL_STYLE)&~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW));
        SetWindowPos(window_,HWND_TOP,monitor.rcMonitor.left,monitor.rcMonitor.top,
            monitor.rcMonitor.right-monitor.rcMonitor.left,monitor.rcMonitor.bottom-monitor.rcMonitor.top,
            SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(window_,GWL_STYLE,GetWindowLongPtrW(window_,GWL_STYLE)|WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(window_,&placement_);
        SetWindowPos(window_,preferences_.alwaysOnTop?HWND_TOPMOST:HWND_NOTOPMOST,0,0,0,0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
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
    const double side=std::clamp(static_cast<double>(preferences_.sidebarWidth),240.,640.);
    double width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
    const bool vertical=edge==WMSZ_TOP || edge==WMSZ_BOTTOM;
    // The sidebar disappears below its layout threshold, so resolve its width
    // from the candidate window rather than the previous window size.
    double sidebar=showPlaylist_ && (width-frameWidth)/dpi_>=side+534. ? side*dpi_ : 0.;
    if (vertical) {
        height=std::max(height,500.*dpi_);
        width=(height-frameHeight-222.*dpi_)*ratio+frameWidth+48.*dpi_+sidebar;
        sidebar=showPlaylist_ && (width-frameWidth)/dpi_>=side+534. ? side*dpi_ : 0.;
        width=(height-frameHeight-222.*dpi_)*ratio+frameWidth+48.*dpi_+sidebar;
    } else {
        width=std::max(width,740.*dpi_);
        height=(width-frameWidth-48.*dpi_-sidebar)/ratio+frameHeight+222.*dpi_;
    }
    if (height<500.*dpi_) {
        height=500.*dpi_;
        width=(height-frameHeight-222.*dpi_)*ratio+frameWidth+48.*dpi_+sidebar;
    }
    if (width<740.*dpi_) {
        width=740.*dpi_;
        sidebar=showPlaylist_ && (width-frameWidth)/dpi_>=side+534. ? side*dpi_ : 0.;
        height=(width-frameWidth-48.*dpi_-sidebar)/ratio+frameHeight+222.*dpi_;
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
    const double side=std::clamp(static_cast<double>(preferences_.sidebarWidth),240.,640.);
    const double sidePx=showPlaylist_ ? side*dpi_ : 0.;
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(window_,MONITOR_DEFAULTTONEAREST),&monitor);
    const double availableWidth=monitor.rcWork.right-monitor.rcWork.left;
    const double availableHeight=monitor.rcWork.bottom-monitor.rcWork.top;
    if (fitScreen) scale=std::min((availableWidth-frameWidth-48.*dpi_-sidePx)/videoWidth,
                                  (availableHeight-frameHeight-222.*dpi_)/videoHeight);
    const double minScale=std::max((740.*dpi_-frameWidth-48.*dpi_)/videoWidth,
                                   (500.*dpi_-frameHeight-222.*dpi_)/videoHeight);
    scale=std::max(scale,minScale);
    double sidebar=showPlaylist_ && (videoWidth*scale+48.*dpi_)/dpi_>=534. ? sidePx : 0.;
    int width=static_cast<int>(std::lround(videoWidth*scale+frameWidth+48.*dpi_+sidebar));
    int height=static_cast<int>(std::lround(videoHeight*scale+frameHeight+222.*dpi_));
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
