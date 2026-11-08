// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "velin/media_engine.hpp"
#include "velin/thumbnails.hpp"
#include <windows.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace velin {
// The system message font (Segoe UI) at the window's DPI, cached per DPI.
HFONT dialogFont(HWND window);
// secret masks the typed text, for passwords.
std::optional<std::wstring> showTextPrompt(HWND owner,std::wstring title,std::wstring label,
    std::wstring value={},bool multiline=false,bool secret=false);
struct SignInForm { std::wstring server,username,password; };
// Server, username and a masked password. message is shown above the fields,
// for example why the previous attempt failed.
std::optional<SignInForm> showSignInPrompt(HWND owner,std::wstring title,std::wstring message,SignInForm initial);
std::optional<size_t> chooseListItem(HWND owner,std::wstring title,
    const std::vector<std::pair<std::wstring,std::wstring>>& items,std::wstring caption={});
std::vector<std::string> choosePaths(HWND owner,std::wstring title,bool multiple=false,
    bool folders=false,bool save=false,std::wstring filterName=L"All files",std::wstring filter=L"*.*");
bool copyTextToClipboard(HWND owner,std::wstring_view text);
bool copyImageToClipboard(HWND owner,unsigned width,unsigned height,const std::vector<std::uint8_t>& bgra);
struct VideoCrop { int width=0,height=0,x=0,y=0; };
std::optional<VideoCrop> selectVideoCrop(HWND owner,RECT screenBounds,int sourceWidth,int sourceHeight);
void showMediaInspector(HWND owner,MediaEngine& engine);
void showFilterManager(HWND owner,MediaEngine& engine,bool audio);
void showLogViewer(HWND owner,const std::vector<std::string>& lines);
class ThumbnailPopup final {
public:
    ThumbnailPopup();
    ~ThumbnailPopup();
    void show(HWND owner,const TimelineThumbnail& thumbnail,POINT anchor);
    void hide();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace velin
