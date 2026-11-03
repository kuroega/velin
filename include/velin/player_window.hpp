// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "velin/core.hpp"
#include "velin/media_engine.hpp"
#include "velin/library.hpp"
#include "velin/platform.hpp"
#include "velin/accessibility.hpp"
#include "velin/extensions.hpp"
#include "velin/plugin_webview.hpp"
#include "velin/tool_windows.hpp"
#include <climits>
#include <windows.h>
#include <d2d1.h>
#include <array>
#include <dwrite.h>
#include <filesystem>
#include <string>
#include <vector>
#include <tuple>
#include <thread>
#include <mutex>
#include <optional>

namespace velin {

class PlayerWindow final {
public:
    explicit PlayerWindow(HINSTANCE instance);
    ~PlayerWindow();
    int run(int showCommand, const std::vector<std::string>& paths);

private:
    enum Action : int {
        None, FileMenu, PlaybackMenu, VideoMenu, AudioMenu, SubtitleMenu, ViewMenu,
        OpenFiles, OpenUrl, OpenFolder, AddFiles, AddSubtitle, TogglePause, Stop,
        Previous, Next, ToggleMute, SpeedMenu, ToggleFullscreen, TogglePlaylist,
        ToggleRepeat, Screenshot, SeekBar, VolumeBar, PlayItem, RecentItem,
        PlaylistTab, ChaptersTab, ChapterItem, RemoveItem, ClearPlaylist, About,
        ToggleTopmost, ToggleResume, ClearHistory, ShowInfo, ShortcutHelp,
        SeekBack, SeekForward, VolumeUp, VolumeDown, PreviousChapter, NextChapter,
        FrameStep, CycleSubtitle, CycleAudio, SubtitleDelayDown, SubtitleDelayUp,
        AudioDelayDown, AudioDelayUp, ToggleSubtitles, Rotate, ResetVideo,
        LoopAB, Quit, Compact, OpenSettings, TogglePlaylistLoop,
        ToggleSecondarySubtitles=90,
        PreferencesAction=100, HistoryAction, SavePlaylist, LoadPlaylist, ShufflePlaylist,
        UnshufflePlaylist, MoveItemUp, MoveItemDown, ClearEntirePlaylist, PictureInPicture,
        MusicMode, VideoSettings, AudioSettings, SubtitleSettings, NetworkSettings, AdvancedSettings,
        CropVideo, ClearCrop, ExternalAudio, ToggleDeinterlace, ToggleDeband, FlipVideo, MirrorVideo,
        SubtitleSizeUp, SubtitleSizeDown, GoToTime, FrameBack, ZoomIn, ZoomOut, ResetPan,
        RevealFile, TrashFile, OpenDisc, OpenClipboard, NewWindow, PluginManager, InstallPlugin,
        ReloadPlugins, PluginConsole, OnlineSubtitles, CheckUpdates, InstallUpdate, MediaLog,
        CopyScreenshot, RecognizeText, RegisterAssociations, UnregisterAssociations,
        CustomCropAspect, JumpToBeginning, VideoFilters, AudioFilters, OpenScreenshotFolder,
        SpeedBase = 1000, AudioBase = 2000, SubtitleBase = 3000,
        ChapterBase = 4000, RecentBase = 5000, AspectBase = 6000,
        PluginMenuBase=7000,AudioDeviceBase=8000,VideoTrackBase=9000,SecondarySubtitleBase=10000,
        CropAspectBase=11000, WindowSizeBase=12000, ToggleWindowAspectLock=12010
    };
    struct Hit { D2D1_RECT_F rect; int action; int data; std::wstring hint; };
    // Layout metrics in device-independent pixels. The video fills the client
    // area beside the docked sidebar; the top bar, controller, notices and
    // tooltips float above it in layered overlay windows (see Layer).
    static constexpr float topBarHeight = 38, sidebarThreshold = 360;
    static constexpr float minWindowWidth = 560, minWindowHeight = 360;
    // Each overlay is an owned, per-pixel-alpha popup aligned to the client
    // area. Transparent pixels let input fall through to the video beneath.
    enum LayerId { TopBarLayer, ControlLayer, NoticeLayer, TooltipLayer, LayerCount };
    struct Layer {
        HWND window{};
        HDC dc{};
        HBITMAP bitmap{};
        HGDIOBJ previous{};
        ID2D1DCRenderTarget* target{};
        ID2D1SolidColorBrush* brush{};
        int left = 0, top = 0, width = 0, height = 0;
        float dpi = 0;
        bool shown = false;
    };
    static constexpr UINT engineMessage = WM_APP + 1;
    static constexpr UINT thumbnailMessage = WM_APP + 10;
    static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK videoProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK overlayProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handleMessage(UINT, WPARAM, LPARAM);
    void createResources();
    void discardTarget();
    void draw();
    void layoutVideo();
    void onEngineEvent();
    void execute(int action, int data = 0);
    bool executeFeature(int action,int data);
    void showMenu(int action, POINT point);
    void showFileDialog(bool append, bool subtitles = false, bool folder = false);
    void openPaths(const std::vector<std::string>& paths, bool append = false);
    void toggleFullscreen();
    std::pair<double,double> videoDisplaySize() const;
    void resizeForVideo(double scale, bool fitScreen);
    bool constrainWindowSizing(WPARAM edge, RECT& bounds) const;
    void togglePip();
    void toggleMusicMode(bool resize=true);
    bool initializeEngine();
    void applyPreferences(const Preferences& before);
    void applyTheme();
    void showPreferences(std::string section={});
    void persistPlayback(bool countPlay=false);
    void savePlaylist();
    void restoreSession();
    void updatePlatform();
    void updateAccessibility();
    void updateWebViews();
    void pollServices();
    void showPluginManager();
    void installPluginPackage(const std::filesystem::path& package,std::string repository={});
    void browsePluginCatalog();
    void downloadPluginRepository(std::string repository);
    void searchSubtitles();
    void dispatchCommand(const std::string& command);
    void acceptLaunch(const std::vector<std::wstring>& arguments);
    bool receivePluginMessage(WPARAM sender,LPARAM payload);
    Json extensionUi(const PluginInfo& metadata,const std::string& operation,const Json& arguments);
    void savePreferences();
    void notice(const std::wstring& message, bool error = false);
    std::filesystem::path screenshotDirectory() const;
    void keyDown(WPARAM key, bool released = false);
    void updatePointer(float x, float y, bool commit);
    int hitAt(float x, float y) const;
    D2D1_RECT_F videoRect() const;
    D2D1_RECT_F sidebarRect() const;
    D2D1_RECT_F controlRect() const;
    float sidebarWidth() const;
    bool sidebarShown() const;
    bool chromeShown() const;
    bool beginLayer(LayerId id, D2D1_RECT_F bounds);
    void endLayer(LayerId id);
    void hideLayer(LayerId id);
    void moveLayers();
    void releaseLayers();
    bool isLayerWindow(HWND window) const;
    bool pointerOverChrome() const;
    void pointerMoved();
    void drawTopBar();
    void drawControls();
    void drawSidebar();
    void drawWelcome(D2D1_RECT_F area);
    void drawNowPlaying(D2D1_RECT_F area);
    void drawNotice(D2D1_RECT_F bounds);
    void drawTooltip(D2D1_RECT_F bounds, std::wstring_view tip);
    void hud(D2D1_RECT_F rect, float radius, bool shadow = true);
    void slider(int action, D2D1_RECT_F track, double fraction, std::wstring hint);
    void fill(D2D1_RECT_F rect, D2D1_COLOR_F color, float radius = 0);
    void outline(D2D1_RECT_F rect, D2D1_COLOR_F color, float radius = 0, float width = 1);
    void gradient(D2D1_RECT_F rect, D2D1_COLOR_F from, D2D1_COLOR_F to, float radius = 0, bool horizontal = false);
    void circle(float cx, float cy, float radius, D2D1_COLOR_F color);
    void line(float x1, float y1, float x2, float y2, D2D1_COLOR_F color, float width = 1);
    void text(std::wstring_view value, D2D1_RECT_F rect, float size,
              D2D1_COLOR_F color, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
              DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING);
    IDWriteTextFormat* textFormat(float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT alignment);
    float textWidth(std::wstring_view value, float size, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL);
    void icon(int action, D2D1_RECT_F rect, D2D1_COLOR_F color);
    void button(int action, D2D1_RECT_F rect, std::wstring_view label,
                std::wstring hint, bool primary = false, int data = 0);
    void iconButton(int action, D2D1_RECT_F rect, std::wstring hint, bool active = false);

    HINSTANCE instance_{};
    HWND window_{}, video_{}, tooltip_{};
    ID2D1Factory* factory_{};
    IDWriteFactory* writeFactory_{};
    ID2D1HwndRenderTarget* target_{};
    ID2D1SolidColorBrush* brush_{};
    // The surface and brush the drawing helpers currently paint with: the
    // main window target, or the overlay layer being rendered.
    ID2D1RenderTarget* canvas_{};
    ID2D1SolidColorBrush* paint_{};
    ID2D1StrokeStyle* roundStroke_{};
    std::array<Layer, LayerCount> layers_{};
    HWND pointerWindow_{};
    COLORREF captionColor_ = CLR_INVALID;
    std::wstring textFamily_, displayFamily_;
    D2D1_RECT_F seekTrack_{}, volumeTrack_{}, controlBounds_{};
    MediaEngine engine_;
    SettingsStore settings_;
    Preferences preferences_;
    HistoryStore history_;
    PlaylistStore playlists_;
    PlatformIntegration platform_;
    AccessibilityProvider accessibility_;
    ThumbnailService thumbnails_;
    ThumbnailPopup preview_;
    std::unique_ptr<OnlineServices> online_;
    std::unique_ptr<PluginHost> plugins_;
    std::map<std::string,std::unique_ptr<PluginWebView>> webviews_;
    std::vector<PluginMenuItem> pluginMenus_;
    std::vector<std::string> logLines_;
    std::map<DWORD,std::pair<std::string,std::string>> pluginPeers_;
    DWORD pluginParent_=0;
    std::string pluginOwner_,pluginSecret_,playerLabel_;
    std::jthread ocrWorker_;
    std::mutex ocrMutex_;
    std::optional<TextRecognitionResult> ocrResult_;
    std::map<std::tuple<int,int,int>,IDWriteTextFormat*> textFormats_;
    std::filesystem::path dataDirectory_;
    std::map<std::uint64_t,std::function<void(const ServiceResult&)>> serviceCallbacks_;
    std::vector<Hit> hits_;
    std::vector<std::string> pendingSubtitles_;
    std::string pendingMediaPath_;
    std::string pendingResumePath_;
    float width_ = 1120, height_ = 700, dpi_ = 1;
    int hovered_ = -1, dragAction_ = None, listOffset_ = 0;
    int abLoopPhase_ = 0;
    int movingItem_=-1;
    POINT dragStart_{},thumbnailAnchor_{};
    double thumbnailTime_=-1,pendingSeek_=-1;
    bool pendingPause_=false;
    double dragPosition_ = 0;
    bool showPlaylist_ = true, chapters_ = false, fullscreen_ = false;
    bool repeating_ = false, playlistLoop_ = false, compact_ = false;
    bool closing_ = false, lastIdle_ = true;
    bool deferredClose_=false;
    bool controlsVisible_ = true;
    bool uiHidden_=false;
    bool pip_=false,musicMode_=false,shuffled_=false,pausedByWindow_=false;
    bool pendingFullscreen_=false;
    ULONGLONG lastPersist_=0,lastAnimation_=0;
    double lastDrawPosition_=-1;
    std::wstring lastTitle_;
    ULONGLONG interactionUntil_ = 0;
    POINT lastPointer_{LONG_MIN,LONG_MIN};
    WINDOWPLACEMENT placement_{};
    WINDOWPLACEMENT pipPlacement_{};
    LONG_PTR pipStyle_=0;
    std::string lastPath_, lastError_;
    std::wstring notice_;
    ULONGLONG noticeUntil_ = 0;
    bool noticeError_ = false;
};

} // namespace velin
