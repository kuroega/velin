// SPDX-License-Identifier: GPL-3.0-or-later
#include "velin/tool_windows.hpp"
#include "velin/core.hpp"
#include "velin/library.hpp"
#include "nlohmann/json.hpp"
#include <commctrl.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <functional>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace velin {
HFONT dialogFont(HWND window) {
    // The stock GUI font is a bitmap-era face that ignores DPI; the message
    // font from the system metrics is the one Windows itself uses in dialogs.
    static std::map<UINT,HFONT> fonts;
    const UINT dpi=window?GetDpiForWindow(window):96;
    if (auto found=fonts.find(dpi);found!=fonts.end()) return found->second;
    NONCLIENTMETRICSW metrics{}; metrics.cbSize=sizeof(metrics);
    HFONT font=nullptr;
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0,dpi)) font=CreateFontIndirectW(&metrics.lfMessageFont);
    if (!font) return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    fonts.emplace(dpi,font);
    return font;
}

namespace {
struct ModalWindow {
    HWND window=nullptr;
    bool done=false,accepted=false;
    std::function<LRESULT(HWND,UINT,WPARAM,LPARAM)> handler;
    static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
        auto* self=reinterpret_cast<ModalWindow*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if (message==WM_NCCREATE) {
            self=static_cast<ModalWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->window=window; SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if (self) {
            if (message==WM_CLOSE) { self->done=true; return 0; }
            if (self->handler) { const auto result=self->handler(window,message,wparam,lparam); if (result!=-1) return result; }
        }
        return DefWindowProcW(window,message,wparam,lparam);
    }
    void create(HWND owner,const std::wstring& title,int width,int height,DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU) {
        WNDCLASSW type{}; type.hInstance=GetModuleHandleW(nullptr); type.lpfnWndProc=procedure;
        type.hCursor=LoadCursorW(nullptr,IDC_ARROW); type.hbrBackground=GetSysColorBrush(COLOR_3DFACE);
        type.lpszClassName=L"Velin.NativeTool"; RegisterClassW(&type);
        const double scale=GetDpiForWindow(owner)/96.; RECT parent{}; GetWindowRect(owner,&parent);
        width=static_cast<int>(width*scale); height=static_cast<int>(height*scale);
        window=CreateWindowExW(WS_EX_DLGMODALFRAME,type.lpszClassName,title.c_str(),style,
            parent.left+(parent.right-parent.left-width)/2,parent.top+(parent.bottom-parent.top-height)/2,width,height,
            owner,nullptr,type.hInstance,this);
    }
    void run(HWND owner) {
        if (!window) return;
        EnableWindow(owner,FALSE); ShowWindow(window,SW_SHOW); MSG message{}; bool quit=false;
        while (!done) {
            const int result=GetMessageW(&message,nullptr,0,0);
            if (result<=0) { quit=result==0; break; }
            if (message.message==WM_KEYDOWN && message.wParam==VK_ESCAPE) { done=true; break; }
            if (!IsDialogMessageW(window,&message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        EnableWindow(owner,TRUE); DestroyWindow(window); window=nullptr; SetForegroundWindow(owner);
        if (quit) PostQuitMessage(static_cast<int>(message.wParam));
    }
};
HWND control(HWND parent,const wchar_t* type,const std::wstring& text,DWORD style,int x,int y,int w,int h,int id) {
    const double scale=GetDpiForWindow(parent)/96.;
    HWND result=CreateWindowExW((std::wstring_view(type)==L"EDIT" || std::wstring_view(type)==WC_LISTVIEWW)?WS_EX_CLIENTEDGE:0,
        type,text.c_str(),WS_CHILD|WS_VISIBLE|style,static_cast<int>(x*scale),static_cast<int>(y*scale),
        static_cast<int>(w*scale),static_cast<int>(h*scale),parent,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr),nullptr);
    SendMessageW(result,WM_SETFONT,reinterpret_cast<WPARAM>(dialogFont(parent)),TRUE);
    return result;
}
std::wstring value(HWND edit) {
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(edit))+1,L'\0');
    GetWindowTextW(edit,text.data(),static_cast<int>(text.size())); text.resize(text.size()-1); return text;
}

std::filesystem::path inspectorWatchFile() {
    std::wstring overridePath(32768,L'\0');
    const DWORD length=GetEnvironmentVariableW(L"VELIN_DATA_DIR",overridePath.data(),static_cast<DWORD>(overridePath.size()));
    if (length && length<overridePath.size()) {
        overridePath.resize(length); std::filesystem::path directory(overridePath);
        if (directory.is_absolute()) return directory/L"inspector-watches.txt";
    }
    PWSTR path=nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&path))) return {};
    const auto result=std::filesystem::path(path)/L"Velin"/L"inspector-watches.txt";
    CoTaskMemFree(path); return result;
}

bool validWatchName(std::string_view name) {
    return !name.empty() && name.size()<=256 && std::all_of(name.begin(),name.end(),[](unsigned char c) {
        return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') ||
            c=='-' || c=='_' || c=='/' || c=='.';
    });
}

bool loadInspectorWatches(const std::filesystem::path& file,std::vector<std::string>& watches,std::string& error) {
    if (file.empty()) { error="Cannot locate the application data directory."; return false; }
    std::error_code ec; const auto exists=std::filesystem::exists(file,ec);
    if (ec) { error="Cannot inspect the watch list: "+ec.message(); return false; }
    if (!exists) return true;
    const auto size=std::filesystem::file_size(file,ec);
    if (ec || size>30000) { error="Cannot read the watch list, or it exceeds the size limit."; return false; }
    std::ifstream input(file,std::ios::binary);
    if (!input) { error="Cannot open the watch list."; return false; }
    std::string name;
    while (std::getline(input,name)) {
        if (!name.empty() && name.back()=='\r') name.pop_back();
        if (!validWatchName(name) || watches.size()>=100 || std::find(watches.begin(),watches.end(),name)!=watches.end()) {
            error="The watch list is invalid. It was not changed."; return false;
        }
        watches.push_back(name);
    }
    if (input.bad()) { error="Cannot finish reading the watch list."; return false; }
    return true;
}

bool saveInspectorWatches(const std::filesystem::path& file,const std::vector<std::string>& watches,std::string& error) {
    std::error_code ec; std::filesystem::create_directories(file.parent_path(),ec);
    if (ec) { error="Cannot create the application data directory: "+ec.message(); return false; }
    std::string contents; for (const auto& watch:watches) contents+=watch+'\n';
    return writeTextFileAtomic(file,contents,error);
}
}

std::optional<std::wstring> showTextPrompt(HWND owner,std::wstring title,std::wstring label,std::wstring initial,bool multiline,bool secret) {
    ModalWindow dialog; HWND edit=nullptr;
    dialog.handler=[&](HWND,UINT message,WPARAM wparam,LPARAM)->LRESULT {
        if (message==WM_COMMAND && (LOWORD(wparam)==IDOK || LOWORD(wparam)==IDCANCEL)) {
            dialog.accepted=LOWORD(wparam)==IDOK; dialog.done=true; return 0;
        }
        return -1;
    };
    dialog.create(owner,title,600,multiline?430:200);
    control(dialog.window,L"STATIC",label,0,18,16,550,26,0);
    edit=control(dialog.window,L"EDIT",initial,WS_TABSTOP|(multiline?(ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN|WS_VSCROLL):ES_AUTOHSCROLL)|(secret && !multiline?ES_PASSWORD:0),
        18,47,550,multiline?280:27,101);
    control(dialog.window,L"BUTTON",L"OK",WS_TABSTOP|BS_DEFPUSHBUTTON,376,multiline?344:99,90,29,IDOK);
    control(dialog.window,L"BUTTON",L"Cancel",WS_TABSTOP,478,multiline?344:99,90,29,IDCANCEL);
    SetFocus(edit);
    // Capture input before the modal helper destroys its controls.
    std::wstring answer;
    const auto original=dialog.handler;
    dialog.handler=[&](HWND window,UINT message,WPARAM wparam,LPARAM lparam)->LRESULT {
        if (message==WM_COMMAND && LOWORD(wparam)==IDOK) answer=value(edit);
        return original(window,message,wparam,lparam);
    };
    dialog.run(owner);
    if (dialog.accepted) return answer;
    return std::nullopt;
}

std::optional<SignInForm> showSignInPrompt(HWND owner,std::wstring title,std::wstring message,SignInForm initial) {
    ModalWindow dialog;
    dialog.handler=[&](HWND,UINT command,WPARAM wparam,LPARAM)->LRESULT {
        if (command==WM_COMMAND && (LOWORD(wparam)==IDOK || LOWORD(wparam)==IDCANCEL)) {
            dialog.accepted=LOWORD(wparam)==IDOK; dialog.done=true; return 0;
        }
        return -1;
    };
    const int top=message.empty()?16:62;
    dialog.create(owner,title,600,top+228);
    if (!message.empty()) control(dialog.window,L"STATIC",message,SS_EDITCONTROL,18,14,550,40,0);
    HWND fields[3]{};
    const wchar_t* labels[]={L"Server",L"Username",L"Password"};
    const std::wstring* values[]={&initial.server,&initial.username,&initial.password};
    for (int i=0;i<3;++i) {
        control(dialog.window,L"STATIC",labels[i],0,18,top+4+i*42,110,24,0);
        fields[i]=control(dialog.window,L"EDIT",*values[i],WS_TABSTOP|ES_AUTOHSCROLL|(i==2?ES_PASSWORD:0),130,top+i*42,438,27,101+i);
    }
    control(dialog.window,L"BUTTON",L"Sign in",WS_TABSTOP|BS_DEFPUSHBUTTON,376,top+136,90,29,IDOK);
    control(dialog.window,L"BUTTON",L"Cancel",WS_TABSTOP,478,top+136,90,29,IDCANCEL);
    // Start on the first empty field, so a remembered server and username go
    // straight to the password.
    HWND focus=fields[2];
    for (HWND field:fields) if (GetWindowTextLengthW(field)==0) { focus=field; break; }
    SetFocus(focus);
    SignInForm answer;
    const auto original=dialog.handler;
    dialog.handler=[&](HWND window,UINT command,WPARAM wparam,LPARAM lparam)->LRESULT {
        if (command==WM_COMMAND && LOWORD(wparam)==IDOK) {
            answer={value(fields[0]),value(fields[1]),value(fields[2])};
            SetWindowTextW(fields[2],L"");
        }
        return original(window,command,wparam,lparam);
    };
    dialog.run(owner);
    if (dialog.accepted) return answer;
    SecureZeroMemory(answer.password.data(),answer.password.size()*sizeof(wchar_t));
    return std::nullopt;
}

std::vector<std::string> choosePaths(HWND owner,std::wstring title,bool multiple,bool folders,bool save,std::wstring name,std::wstring filter) {
    IFileDialog* dialog=nullptr;
    const HRESULT result=CoCreateInstance(save?CLSID_FileSaveDialog:CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,
        IID_IFileDialog,reinterpret_cast<void**>(&dialog));
    if (FAILED(result)) return {};
    DWORD options=0; dialog->GetOptions(&options);
    options|=FOS_FORCEFILESYSTEM|FOS_NOCHANGEDIR;
    if (folders) options|=FOS_PICKFOLDERS;
    if (!save) options|=FOS_FILEMUSTEXIST;
    else options|=FOS_OVERWRITEPROMPT;
    if (multiple && !save) options|=FOS_ALLOWMULTISELECT;
    dialog->SetOptions(options); dialog->SetTitle(title.c_str());
    COMDLG_FILTERSPEC filters[]={{name.c_str(),filter.c_str()},{L"All files",L"*.*"}};
    if (!folders) dialog->SetFileTypes(2,filters);
    std::vector<std::string> paths;
    if (SUCCEEDED(dialog->Show(owner))) {
        auto add=[&](IShellItem* item) { PWSTR path=nullptr; if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))) {
            paths.push_back(toUtf8(path)); CoTaskMemFree(path);
        } };
        IFileOpenDialog* open=nullptr;
        if (!save && SUCCEEDED(dialog->QueryInterface(IID_IFileOpenDialog,reinterpret_cast<void**>(&open)))) {
            IShellItemArray* items=nullptr;
            if (SUCCEEDED(open->GetResults(&items))) {
                DWORD count=0; items->GetCount(&count);
                for (DWORD i=0;i<count;++i) { IShellItem* item=nullptr; if (SUCCEEDED(items->GetItemAt(i,&item))) { add(item); item->Release(); } }
                items->Release();
            }
            open->Release();
        } else {
            IShellItem* item=nullptr; if (SUCCEEDED(dialog->GetResult(&item))) { add(item); item->Release(); }
        }
    }
    dialog->Release(); return paths;
}

std::optional<size_t> chooseListItem(HWND owner,std::wstring title,const std::vector<std::pair<std::wstring,std::wstring>>& items,std::wstring caption) {
    ModalWindow dialog; HWND list=nullptr,search=nullptr; std::vector<size_t> visible; std::optional<size_t> selected;
    auto refresh=[&] {
        ListView_DeleteAllItems(list); visible.clear(); auto query=value(search);
        std::transform(query.begin(),query.end(),query.begin(),[](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        for (size_t i=0;i<items.size();++i) {
            auto combined=items[i].first+L" "+items[i].second;
            std::transform(combined.begin(),combined.end(),combined.begin(),[](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            if (!query.empty() && combined.find(query)==std::wstring::npos) continue;
            LVITEMW row{}; row.mask=LVIF_TEXT; row.iItem=static_cast<int>(visible.size()); row.pszText=const_cast<wchar_t*>(items[i].first.c_str());
            ListView_InsertItem(list,&row); row.iSubItem=1; row.pszText=const_cast<wchar_t*>(items[i].second.c_str());
            SendMessageW(list,LVM_SETITEMTEXTW,row.iItem,reinterpret_cast<LPARAM>(&row)); visible.push_back(i);
        }
        if (!visible.empty()) ListView_SetItemState(list,0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
    };
    auto accept=[&] {
        const int index=ListView_GetNextItem(list,-1,LVNI_SELECTED);
        if (index>=0 && static_cast<size_t>(index)<visible.size()) { selected=visible[static_cast<size_t>(index)]; dialog.accepted=true; dialog.done=true; }
    };
    dialog.handler=[&](HWND,UINT message,WPARAM wparam,LPARAM lparam)->LRESULT {
        if (message==WM_COMMAND && LOWORD(wparam)==101 && HIWORD(wparam)==EN_CHANGE) { refresh(); return 0; }
        if (message==WM_COMMAND && LOWORD(wparam)==IDOK) { accept(); return 0; }
        if (message==WM_COMMAND && LOWORD(wparam)==IDCANCEL) { dialog.done=true; return 0; }
        if (message==WM_NOTIFY && reinterpret_cast<NMHDR*>(lparam)->hwndFrom==list && reinterpret_cast<NMHDR*>(lparam)->code==NM_DBLCLK) { accept(); return 0; }
        return -1;
    };
    dialog.create(owner,title,820,590);
    control(dialog.window,L"STATIC",caption.empty()?L"Choose an item, or type to filter the list.":caption,0,16,12,770,25,0);
    search=control(dialog.window,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,16,44,770,27,101);
    list=control(dialog.window,WC_LISTVIEWW,L"",WS_TABSTOP|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS,16,84,770,395,102);
    ListView_SetExtendedListViewStyle(list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW column{}; column.mask=LVCF_TEXT|LVCF_WIDTH; column.cx=300; column.pszText=const_cast<wchar_t*>(L"Name"); ListView_InsertColumn(list,0,&column);
    column.cx=450; column.pszText=const_cast<wchar_t*>(L"Details"); ListView_InsertColumn(list,1,&column);
    control(dialog.window,L"BUTTON",L"Choose",WS_TABSTOP|BS_DEFPUSHBUTTON,584,499,95,30,IDOK);
    control(dialog.window,L"BUTTON",L"Cancel",WS_TABSTOP,691,499,95,30,IDCANCEL);
    refresh(); dialog.run(owner); return selected;
}

bool copyTextToClipboard(HWND owner,std::wstring_view text) {
    if (!OpenClipboard(owner)) return false;
    const size_t bytes=(text.size()+1)*sizeof(wchar_t);
    HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,bytes); bool success=false;
    if (memory) {
        void* destination=GlobalLock(memory);
        if (destination) {
            std::memcpy(destination,text.data(),text.size()*sizeof(wchar_t));
            static_cast<wchar_t*>(destination)[text.size()]=L'\0'; GlobalUnlock(memory);
            EmptyClipboard(); success=SetClipboardData(CF_UNICODETEXT,memory)!=nullptr;
        }
        if (!success) GlobalFree(memory);
    }
    CloseClipboard(); return success;
}

bool copyImageToClipboard(HWND owner,unsigned width,unsigned height,const std::vector<std::uint8_t>& pixels) {
    if (!width || !height || width>32768 || height>32768 || pixels.size()!=static_cast<size_t>(width)*height*4 || !OpenClipboard(owner)) return false;
    HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,sizeof(BITMAPV5HEADER)+pixels.size()); bool success=false;
    if (memory) {
        auto* header=static_cast<BITMAPV5HEADER*>(GlobalLock(memory));
        if (header) {
            *header={}; header->bV5Size=sizeof(*header); header->bV5Width=static_cast<LONG>(width); header->bV5Height=-static_cast<LONG>(height);
            header->bV5Planes=1; header->bV5BitCount=32; header->bV5Compression=BI_BITFIELDS;
            header->bV5RedMask=0x00ff0000; header->bV5GreenMask=0x0000ff00; header->bV5BlueMask=0x000000ff; header->bV5AlphaMask=0xff000000;
            header->bV5CSType=0x73524742; std::memcpy(header+1,pixels.data(),pixels.size()); GlobalUnlock(memory);
            EmptyClipboard(); success=SetClipboardData(CF_DIBV5,memory)!=nullptr;
        }
        if (!success) GlobalFree(memory);
    }
    CloseClipboard(); return success;
}

std::optional<VideoCrop> selectVideoCrop(HWND owner,RECT bounds,int sourceWidth,int sourceHeight) {
    if (sourceWidth<=0 || sourceHeight<=0) return std::nullopt;
    struct Selection {
        HWND window=nullptr; POINT first{},last{}; bool dragging=false,done=false,accepted=false;
        static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
            auto* self=reinterpret_cast<Selection*>(GetWindowLongPtrW(window,GWLP_USERDATA));
            if (message==WM_NCCREATE) { self=static_cast<Selection*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams); SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self)); }
            if (self) {
                if (message==WM_LBUTTONDOWN) { self->first=self->last={GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)}; self->dragging=true; SetCapture(window); return 0; }
                if (message==WM_MOUSEMOVE && self->dragging) { self->last={GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)}; InvalidateRect(window,nullptr,TRUE); return 0; }
                if (message==WM_LBUTTONUP) { self->dragging=false; ReleaseCapture(); self->accepted=true; self->done=true; return 0; }
                if (message==WM_KEYDOWN && wparam==VK_ESCAPE) { self->done=true; return 0; }
                if (message==WM_PAINT) {
                    PAINTSTRUCT paint{}; HDC dc=BeginPaint(window,&paint); RECT rect{}; GetClientRect(window,&rect);
                    HBRUSH background=CreateSolidBrush(RGB(12,13,17)); FillRect(dc,&rect,background); DeleteObject(background);
                    SetTextColor(dc,RGB(243,242,248)); SetBkMode(dc,TRANSPARENT);
                    const HGDIOBJ oldFont=SelectObject(dc,dialogFont(window));
                    RECT hint=rect; hint.top+=MulDiv(12,static_cast<int>(GetDpiForWindow(window)),96);
                    DrawTextW(dc,L"Drag to crop the video. Press Esc to cancel.",-1,&hint,DT_CENTER|DT_TOP|DT_SINGLELINE);
                    SelectObject(dc,oldFont);
                    HPEN pen=CreatePen(PS_SOLID,2,RGB(183,164,255)); HGDIOBJ oldPen=SelectObject(dc,pen),oldBrush=SelectObject(dc,GetStockObject(NULL_BRUSH));
                    Rectangle(dc,std::min(self->first.x,self->last.x),std::min(self->first.y,self->last.y),std::max(self->first.x,self->last.x),std::max(self->first.y,self->last.y));
                    SelectObject(dc,oldBrush); SelectObject(dc,oldPen); DeleteObject(pen); EndPaint(window,&paint); return 0;
                }
            }
            return DefWindowProcW(window,message,wparam,lparam);
        }
    } selection;
    // Match the source image inside the letterboxed video viewport.
    const double scale=std::min(static_cast<double>(bounds.right-bounds.left)/sourceWidth,static_cast<double>(bounds.bottom-bounds.top)/sourceHeight);
    const int width=static_cast<int>(sourceWidth*scale),height=static_cast<int>(sourceHeight*scale);
    bounds.left+=(bounds.right-bounds.left-width)/2; bounds.top+=(bounds.bottom-bounds.top-height)/2;
    WNDCLASSW type{}; type.lpfnWndProc=Selection::procedure; type.hInstance=GetModuleHandleW(nullptr);
    type.hCursor=LoadCursorW(nullptr,IDC_CROSS); type.lpszClassName=L"Velin.VideoCrop"; RegisterClassW(&type);
    selection.window=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW,type.lpszClassName,L"Crop video",WS_POPUP,
        bounds.left,bounds.top,width,height,owner,nullptr,type.hInstance,&selection);
    if (!selection.window) return std::nullopt;
    SetLayeredWindowAttributes(selection.window,0,145,LWA_ALPHA); EnableWindow(owner,FALSE); ShowWindow(selection.window,SW_SHOW); SetFocus(selection.window);
    MSG message{}; bool quit=false;
    while (!selection.done) { const int result=GetMessageW(&message,nullptr,0,0); if (result<=0) { quit=result==0; break; } TranslateMessage(&message); DispatchMessageW(&message); }
    EnableWindow(owner,TRUE); DestroyWindow(selection.window); SetForegroundWindow(owner);
    if (quit) PostQuitMessage(static_cast<int>(message.wParam));
    if (!selection.accepted) return std::nullopt;
    const int x=std::clamp(static_cast<int>(std::min(selection.first.x,selection.last.x)/scale),0,sourceWidth-1);
    const int y=std::clamp(static_cast<int>(std::min(selection.first.y,selection.last.y)/scale),0,sourceHeight-1);
    const int w=std::clamp(static_cast<int>(std::abs(selection.first.x-selection.last.x)/scale),0,sourceWidth-x);
    const int h=std::clamp(static_cast<int>(std::abs(selection.first.y-selection.last.y)/scale),0,sourceHeight-y);
    if (w<2 || h<2) return std::nullopt;
    return VideoCrop{w-w%2,h-h%2,x-x%2,y-y%2};
}

void showMediaInspector(HWND owner,MediaEngine& engine) {
    ModalWindow dialog; HWND list=nullptr,watchInput=nullptr;
    const std::vector<std::pair<std::wstring,std::string>> properties{{L"Title","media-title"},{L"Source","path"},{L"Metadata","metadata"},
        {L"Duration","duration"},{L"Position","time-pos"},{L"Video codec","video-codec"},{L"Video parameters","video-params"},
        {L"Hardware decoder","hwdec-current"},{L"Frame rate","estimated-vf-fps"},{L"Display frame rate","display-fps"},
        {L"Video bitrate","video-bitrate"},{L"Dropped frames","frame-drop-count"},{L"Decoder dropped frames","decoder-frame-drop-count"},
        {L"Audio codec","audio-codec-name"},{L"Audio parameters","audio-params"},{L"Audio device","audio-device"},{L"Audio bitrate","audio-bitrate"},
        {L"A/V sync","avsync"},{L"Cache state","demuxer-cache-state"},{L"Video filters","vf"},{L"Audio filters","af"},{L"Tracks","track-list"}};
    const auto watchFile=inspectorWatchFile(); std::vector<std::string> watches; std::string watchError;
    const bool watchesAvailable=loadInspectorWatches(watchFile,watches,watchError);
    if (!watchesAvailable) watches.clear();
    auto addRow=[&](int index,const std::wstring& label) {
        LVITEMW row{}; row.mask=LVIF_TEXT; row.iItem=index; row.pszText=const_cast<wchar_t*>(label.c_str()); ListView_InsertItem(list,&row);
    };
    auto refresh=[&]() {
        for (size_t i=0;i<properties.size();++i) {
            auto data=fromUtf8(engine.propertyJson(properties[i].second));
            LVITEMW row{}; row.iSubItem=1; row.pszText=data.data(); SendMessageW(list,LVM_SETITEMTEXTW,i,reinterpret_cast<LPARAM>(&row));
        }
        for (size_t i=0;i<watches.size();++i) {
            const auto json=engine.propertyJson(watches[i]);
            auto data=json=="null"?L"Unavailable or null":fromUtf8(json);
            LVITEMW row{}; row.iSubItem=1; row.pszText=data.data();
            SendMessageW(list,LVM_SETITEMTEXTW,properties.size()+i,reinterpret_cast<LPARAM>(&row));
        }
    };
    dialog.handler=[&](HWND,UINT message,WPARAM wparam,LPARAM)->LRESULT {
        if (message==WM_TIMER) { refresh(); return 0; }
        if (message==WM_COMMAND && LOWORD(wparam)==IDOK) { dialog.done=true; return 0; }
        if (message==WM_COMMAND && LOWORD(wparam)==102) {
            std::wstring all;
            for (const auto& [label,name]:properties) all+=label+L": "+fromUtf8(engine.propertyJson(name))+L"\r\n";
            for (const auto& name:watches) all+=fromUtf8(name)+L": "+fromUtf8(engine.propertyJson(name))+L"\r\n";
            copyTextToClipboard(owner,all); return 0;
        }
        if (message==WM_COMMAND && LOWORD(wparam)==104) {
            const auto name=toUtf8(value(watchInput));
            if (!validWatchName(name)) { MessageBoxW(dialog.window,L"Enter an mpv property name using letters, numbers, hyphens, underscores, periods or slashes (up to 256 characters).",L"Add watch",MB_OK|MB_ICONWARNING); return 0; }
            if (watches.size()>=100 || std::find(watches.begin(),watches.end(),name)!=watches.end()) { MessageBoxW(dialog.window,L"This watch already exists or the 100-watch limit has been reached.",L"Add watch",MB_OK|MB_ICONWARNING); return 0; }
            auto updated=watches; updated.push_back(name); std::string error;
            if (!saveInspectorWatches(watchFile,updated,error)) { MessageBoxW(dialog.window,fromUtf8(error).c_str(),L"Cannot save watch",MB_OK|MB_ICONERROR); return 0; }
            watches=std::move(updated); addRow(static_cast<int>(properties.size()+watches.size()-1),fromUtf8(name)); SetWindowTextW(watchInput,L""); refresh(); return 0;
        }
        if (message==WM_COMMAND && LOWORD(wparam)==105) {
            const int selected=ListView_GetNextItem(list,-1,LVNI_SELECTED);
            if (selected<static_cast<int>(properties.size()) || selected>=static_cast<int>(properties.size()+watches.size())) return 0;
            auto updated=watches; updated.erase(updated.begin()+(selected-static_cast<int>(properties.size()))); std::string error;
            if (!saveInspectorWatches(watchFile,updated,error)) { MessageBoxW(dialog.window,fromUtf8(error).c_str(),L"Cannot save watch",MB_OK|MB_ICONERROR); return 0; }
            watches=std::move(updated); ListView_DeleteItem(list,selected); refresh(); return 0;
        }
        return -1;
    };
    dialog.create(owner,L"Media inspector",850,680,WS_OVERLAPPEDWINDOW);
    list=control(dialog.window,WC_LISTVIEWW,L"",LVS_REPORT|LVS_SINGLESEL|WS_TABSTOP,16,16,800,510,101);
    ListView_SetExtendedListViewStyle(list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_GRIDLINES);
    LVCOLUMNW column{}; column.mask=LVCF_TEXT|LVCF_WIDTH; column.cx=180; column.pszText=const_cast<wchar_t*>(L"Property"); ListView_InsertColumn(list,0,&column);
    column.cx=590; column.pszText=const_cast<wchar_t*>(L"Value"); ListView_InsertColumn(list,1,&column);
    for (size_t i=0;i<properties.size();++i) addRow(static_cast<int>(i),properties[i].first);
    for (size_t i=0;i<watches.size();++i) addRow(static_cast<int>(properties.size()+i),fromUtf8(watches[i]));
    control(dialog.window,L"STATIC",L"mpv property:",0,16,547,95,25,0);
    watchInput=control(dialog.window,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,113,542,340,27,103);
    const auto add=control(dialog.window,L"BUTTON",L"Add watch",WS_TABSTOP,465,541,110,29,104);
    const auto remove=control(dialog.window,L"BUTTON",L"Remove watch",WS_TABSTOP,585,541,130,29,105);
    EnableWindow(watchInput,watchesAvailable); EnableWindow(add,watchesAvailable); EnableWindow(remove,watchesAvailable);
    control(dialog.window,L"BUTTON",L"Copy details",WS_TABSTOP,16,588,120,30,102);
    control(dialog.window,L"BUTTON",L"Close",WS_TABSTOP|BS_DEFPUSHBUTTON,710,588,106,30,IDOK);
    refresh(); SetTimer(dialog.window,1,500,nullptr);
    if (!watchesAvailable) MessageBoxW(dialog.window,fromUtf8(watchError).c_str(),L"Inspector watches",MB_OK|MB_ICONERROR);
    dialog.run(owner);
}

void showFilterManager(HWND owner,MediaEngine& engine,bool audio) {
    using Json=nlohmann::json;
    const std::string property=audio?"af":"vf", kind=audio?"audio":"video";
    const auto watchFile=inspectorWatchFile();
    const auto file=watchFile.empty()?std::filesystem::path{}:watchFile.parent_path()/L"filter-presets.json";
    Json presets=Json::array(); HWND errorOwner=owner;
    auto report=[&](const std::string& message) { MessageBoxW(errorOwner,fromUtf8(message).c_str(),L"Filter manager",MB_OK|MB_ICONERROR); };
    if (file.empty()) { report("Cannot locate the application data directory."); return; }
    std::error_code ec;
    if (std::filesystem::exists(file,ec)) {
        const auto bytes=std::filesystem::file_size(file,ec);
        if (ec || bytes>1024*1024) { report("Cannot read filter presets or the file exceeds 1 MB."); return; }
        std::ifstream input(file,std::ios::binary);
        presets=Json::parse(input,nullptr,false);
        if (!input && !input.eof()) { report("Cannot read filter presets."); return; }
        if (!presets.is_array() || presets.size()>1000) { report("The filter preset file is invalid; it was not changed."); return; }
        std::set<std::string> ids;
        for (const auto& preset:presets) {
            if (!preset.is_object() || !preset.contains("id") || !preset["id"].is_string() ||
                !preset.contains("name") || !preset["name"].is_string() || !preset.contains("kind") || !preset["kind"].is_string() ||
                !preset.contains("filter") || !preset["filter"].is_object() ||
                !preset["filter"].contains("name") || !preset["filter"]["name"].is_string() ||
                preset["filter"]["name"].get<std::string>().empty() ||
                (preset["filter"].contains("label") && !preset["filter"]["label"].is_string()) ||
                (preset["kind"]!="audio" && preset["kind"]!="video")) {
                report("The filter preset file is invalid; it was not changed."); return;
            }
            const auto id=preset["id"].get<std::string>(); GUID guid{};
            if (!ids.insert(id).second || FAILED(CLSIDFromString(fromUtf8(id).c_str(),&guid))) {
                report("The filter preset file has an invalid or duplicate identifier; it was not changed."); return;
            }
        }
    } else if (ec) { report("Cannot inspect the filter preset file: "+ec.message()); return; }
    auto save=[&](const Json& data)->bool {
        std::error_code saveError; std::filesystem::create_directories(file.parent_path(),saveError);
        if (saveError) { report("Cannot create the application data directory: "+saveError.message()); return false; }
        std::string message;
        if (!writeTextFileAtomic(file,data.dump(2)+"\n",message)) { report(message); return false; }
        return true;
    };
    auto readChain=[&]()->Json { auto chain=Json::parse(engine.propertyJson(property),nullptr,false); return chain.is_array()?chain:Json(); };
    auto apply=[&](const Json& chain)->bool {
        std::string message;
        if (!engine.setPropertyJsonChecked(property,chain.dump(),message)) { report("Cannot apply filters: "+message); return false; }
        return true;
    };
    auto promptFilter=[&](HWND window,const Json& initial)->std::optional<Json> {
        auto answer=showTextPrompt(window,L"Filter definition",L"Enter one mpv filter as a JSON object (for example {\"name\":\"hflip\"}).",fromUtf8(initial.dump(2)),true);
        if (!answer) return std::nullopt;
        auto filter=Json::parse(toUtf8(*answer),nullptr,false);
        if (!filter.is_object() || !filter.contains("name") || !filter["name"].is_string() || filter["name"].get<std::string>().empty()) {
            report("A filter must be a JSON object with a nonempty string name."); return std::nullopt;
        }
        return filter;
    };
    ModalWindow dialog; HWND activeList=nullptr,presetList=nullptr; std::vector<size_t> visible;
    auto refresh=[&] {
        const auto chain=readChain(); ListView_DeleteAllItems(activeList); ListView_DeleteAllItems(presetList); visible.clear();
        if (chain.is_array()) for (size_t i=0;i<chain.size();++i) {
            auto text=fromUtf8(chain[i].dump()); LVITEMW row{}; row.mask=LVIF_TEXT; row.iItem=static_cast<int>(i); row.pszText=text.data(); ListView_InsertItem(activeList,&row);
        }
        for (size_t i=0;i<presets.size();++i) if (presets[i]["kind"]==kind) {
            const auto label="velin-preset-"+presets[i]["id"].get<std::string>(); bool enabled=false;
            if (chain.is_array()) for (const auto& filter:chain) if (filter.is_object() && filter.contains("label") && filter["label"].is_string() && filter["label"]==label) enabled=true;
            auto text=fromUtf8(presets[i]["name"].get<std::string>()+(enabled?" (active)":""));
            LVITEMW row{}; row.mask=LVIF_TEXT; row.iItem=static_cast<int>(visible.size()); row.pszText=text.data(); ListView_InsertItem(presetList,&row); visible.push_back(i);
        }
    };
    auto selectedPreset=[&]()->std::optional<size_t> { const int row=ListView_GetNextItem(presetList,-1,LVNI_SELECTED); if (row<0 || static_cast<size_t>(row)>=visible.size()) return std::nullopt; return visible[static_cast<size_t>(row)]; };
    dialog.handler=[&](HWND window,UINT message,WPARAM wparam,LPARAM)->LRESULT {
        if (message!=WM_COMMAND) return -1;
        const auto action=LOWORD(wparam); if (action==IDOK) { dialog.done=true; return 0; }
        if (action<101 || action>107) return -1;
        auto chain=readChain(); if (!chain.is_array()) { report("Cannot read the active filter chain."); return 0; }
        const int selected=ListView_GetNextItem(activeList,-1,LVNI_SELECTED);
        if (action==101 || action==102) {
            if (action==102 && (selected<0 || static_cast<size_t>(selected)>=chain.size())) return 0;
            auto filter=promptFilter(window,action==101?Json{{"name",audio?"volume":"hflip"}}:chain[static_cast<size_t>(selected)]);
            if (!filter) return 0;
            if (action==101) chain.push_back(*filter); else chain[static_cast<size_t>(selected)]=*filter;
            if (apply(chain)) refresh(); return 0;
        }
        if (action==103) {
            if (selected<0 || static_cast<size_t>(selected)>=chain.size()) return 0;
            chain.erase(static_cast<size_t>(selected)); if (apply(chain)) refresh(); return 0;
        }
        if (action==104) {
            if (selected<0 || static_cast<size_t>(selected)>=chain.size()) return 0;
            auto name=showTextPrompt(window,L"Save filter preset",L"Preset name:");
            if (!name || name->empty()) return 0;
            if (presets.size()>=1000 || name->size()>200) { report("The preset limit or name length limit has been reached."); return 0; }
            GUID guid{}; if (FAILED(CoCreateGuid(&guid))) { report("Cannot generate a preset identifier."); return 0; }
            wchar_t idText[40]{}; StringFromGUID2(guid,idText,40); auto id=toUtf8(idText);
            auto filter=chain[static_cast<size_t>(selected)]; if (!filter.is_object()) { report("The selected filter cannot be saved."); return 0; }
            filter.erase("label"); auto updated=presets;
            updated.push_back({{"id",id},{"name",toUtf8(*name)},{"kind",kind},{"filter",filter}});
            if (save(updated)) { presets=std::move(updated); refresh(); } return 0;
        }
        const auto index=selectedPreset(); if (!index) return 0;
        const auto label="velin-preset-"+presets[*index]["id"].get<std::string>();
        if (action==105) {
            bool found=false;
            for (size_t i=chain.size();i>0;--i) if (chain[i-1].is_object() && chain[i-1].contains("label") && chain[i-1]["label"].is_string() && chain[i-1]["label"]==label) { chain.erase(i-1); found=true; }
            if (!found) { auto filter=presets[*index]["filter"]; filter["label"]=label; chain.push_back(filter); }
            if (apply(chain)) refresh(); return 0;
        }
        if (action==106) {
            auto name=showTextPrompt(window,L"Edit preset",L"Preset name:",fromUtf8(presets[*index]["name"].get<std::string>()));
            if (!name || name->empty()) return 0;
            if (name->size()>200) { report("The preset name is too long."); return 0; }
            auto filter=promptFilter(window,presets[*index]["filter"]); if (!filter) return 0;
            filter->erase("label"); auto updated=presets; updated[*index]["name"]=toUtf8(*name); updated[*index]["filter"]=*filter;
            if (save(updated)) { presets=std::move(updated); refresh(); } return 0;
        }
        if (action==107 && MessageBoxW(window,L"Delete this saved preset? Active filters will remain in the current chain.",L"Delete preset",MB_YESNO|MB_ICONQUESTION)==IDYES) {
            auto updated=presets; updated.erase(*index); if (save(updated)) { presets=std::move(updated); refresh(); }
        }
        return 0;
    };
    dialog.create(owner,audio?L"Audio filters":L"Video filters",850,650);
    if (!dialog.window) return;
    errorOwner=dialog.window;
    control(dialog.window,L"STATIC",L"Active filters (mpv filter objects):",0,16,12,500,25,0);
    activeList=control(dialog.window,WC_LISTVIEWW,L"",WS_TABSTOP|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS,16,38,800,180,110);
    LVCOLUMNW column{}; column.mask=LVCF_TEXT|LVCF_WIDTH; column.cx=770; column.pszText=const_cast<wchar_t*>(L"Filter"); ListView_InsertColumn(activeList,0,&column);
    ListView_SetExtendedListViewStyle(activeList,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
    control(dialog.window,L"BUTTON",L"Add...",WS_TABSTOP,16,230,100,30,101);
    control(dialog.window,L"BUTTON",L"Edit...",WS_TABSTOP,125,230,100,30,102);
    control(dialog.window,L"BUTTON",L"Remove",WS_TABSTOP,234,230,100,30,103);
    control(dialog.window,L"BUTTON",L"Save as preset...",WS_TABSTOP,343,230,145,30,104);
    control(dialog.window,L"STATIC",L"Saved presets (edits apply the next time a preset is enabled):",0,16,278,750,25,0);
    presetList=control(dialog.window,WC_LISTVIEWW,L"",WS_TABSTOP|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS,16,305,800,180,111);
    column.pszText=const_cast<wchar_t*>(L"Preset"); ListView_InsertColumn(presetList,0,&column);
    ListView_SetExtendedListViewStyle(presetList,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
    control(dialog.window,L"BUTTON",L"Toggle",WS_TABSTOP,16,498,100,30,105);
    control(dialog.window,L"BUTTON",L"Edit...",WS_TABSTOP,125,498,100,30,106);
    control(dialog.window,L"BUTTON",L"Delete",WS_TABSTOP,234,498,100,30,107);
    control(dialog.window,L"BUTTON",L"Close",WS_TABSTOP|BS_DEFPUSHBUTTON,710,550,106,30,IDOK);
    refresh(); dialog.run(owner);
}

void showLogViewer(HWND owner,const std::vector<std::string>& lines) {
    std::wstring log; for (const auto& line:lines) log+=fromUtf8(line)+L"\r\n";
    ModalWindow dialog;
    dialog.handler=[&](HWND,UINT message,WPARAM wparam,LPARAM)->LRESULT {
        if (message==WM_COMMAND && LOWORD(wparam)==IDOK) { dialog.done=true; return 0; }
        if (message==WM_COMMAND && LOWORD(wparam)==102) { copyTextToClipboard(owner,log); return 0; }
        return -1;
    };
    dialog.create(owner,L"Playback and plugin log",900,650,WS_OVERLAPPEDWINDOW);
    control(dialog.window,L"EDIT",log,WS_TABSTOP|ES_MULTILINE|ES_READONLY|ES_AUTOHSCROLL|ES_AUTOVSCROLL|WS_VSCROLL|WS_HSCROLL,16,16,850,525,101);
    control(dialog.window,L"BUTTON",L"Copy log",WS_TABSTOP,16,562,115,30,102);
    control(dialog.window,L"BUTTON",L"Close",WS_TABSTOP|BS_DEFPUSHBUTTON,760,562,106,30,IDOK);
    dialog.run(owner);
}

struct ThumbnailPopup::Impl {
    HWND window=nullptr;
    TimelineThumbnail thumbnail;
    HFONT font=nullptr;
    UINT fontDpi=0;
    int inset=4,caption=26;
    ~Impl() { if (font) DeleteObject(font); }
    static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if (message==WM_NCCREATE) { self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams); SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self)); }
        if (self && message==WM_PAINT) {
            PAINTSTRUCT paint{}; HDC dc=BeginPaint(window,&paint); RECT client{}; GetClientRect(window,&client);
            HBRUSH brush=CreateSolidBrush(RGB(20,21,27)); FillRect(dc,&client,brush); DeleteObject(brush);
            const int inset=self->inset,caption=self->caption;
            const auto& frame=self->thumbnail;
            if (!frame.bgra.empty()) {
                BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(info.bmiHeader); info.bmiHeader.biWidth=static_cast<LONG>(frame.width);
                info.bmiHeader.biHeight=-static_cast<LONG>(frame.height); info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
                SetStretchBltMode(dc,HALFTONE); StretchDIBits(dc,inset,inset,client.right-2*inset,client.bottom-inset-caption,0,0,static_cast<int>(frame.width),static_cast<int>(frame.height),frame.bgra.data(),&info,DIB_RGB_COLORS,SRCCOPY);
            }
            RECT label{0,client.bottom-caption,client.right,client.bottom}; SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(243,242,248));
            const HGDIOBJ previous=self->font?SelectObject(dc,self->font):nullptr;
            const auto time=fromUtf8(formatTime(frame.seconds)); DrawTextW(dc,time.c_str(),-1,&label,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            if (previous) SelectObject(dc,previous);
            EndPaint(window,&paint); return 0;
        }
        if (message==WM_NCHITTEST) return HTTRANSPARENT;
        return DefWindowProcW(window,message,wparam,lparam);
    }
};
ThumbnailPopup::ThumbnailPopup():impl_(std::make_unique<Impl>()) {}
ThumbnailPopup::~ThumbnailPopup() { if (impl_->window) DestroyWindow(impl_->window); }
void ThumbnailPopup::show(HWND owner,const TimelineThumbnail& frame,POINT anchor) {
    if (!frame.width || !frame.height || frame.bgra.size()!=static_cast<size_t>(frame.width)*frame.height*4) return;
    if (!impl_->window) {
        WNDCLASSW type{}; type.hInstance=GetModuleHandleW(nullptr); type.lpfnWndProc=Impl::procedure; type.lpszClassName=L"Velin.TimelinePreview"; RegisterClassW(&type);
        impl_->window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT,type.lpszClassName,L"Timeline preview",WS_POPUP,0,0,1,1,owner,nullptr,type.hInstance,impl_.get());
        if (impl_->window) {
            // Windows 11 rounds the popup and draws a hairline border; older systems ignore both.
            const DWORD corners=2; DwmSetWindowAttribute(impl_->window,33,&corners,sizeof(corners));
            const COLORREF border=RGB(48,49,60); DwmSetWindowAttribute(impl_->window,34,&border,sizeof(border));
        }
    }
    if (const UINT dpi=GetDpiForWindow(owner);dpi!=impl_->fontDpi) {
        if (impl_->font) DeleteObject(impl_->font);
        impl_->font=CreateFontW(-MulDiv(9,static_cast<int>(dpi),72),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        impl_->fontDpi=dpi;
    }
    impl_->thumbnail=frame;
    const double scale=GetDpiForWindow(owner)/96.;
    impl_->inset=static_cast<int>(4*scale); impl_->caption=static_cast<int>(26*scale);
    const int width=static_cast<int>(frame.width*scale)+2*impl_->inset,height=static_cast<int>(frame.height*scale)+impl_->inset+impl_->caption;
    MONITORINFO monitor{}; monitor.cbSize=sizeof(monitor); GetMonitorInfoW(MonitorFromPoint(anchor,MONITOR_DEFAULTTONEAREST),&monitor);
    SetWindowPos(impl_->window,HWND_TOP,std::clamp(static_cast<int>(anchor.x)-width/2,static_cast<int>(monitor.rcWork.left),std::max(static_cast<int>(monitor.rcWork.left),static_cast<int>(monitor.rcWork.right)-width)),
        std::max(static_cast<int>(monitor.rcWork.top),static_cast<int>(anchor.y)-height-12),width,height,SWP_NOACTIVATE|SWP_SHOWWINDOW);
    InvalidateRect(impl_->window,nullptr,FALSE);
}
void ThumbnailPopup::hide() { if (impl_->window) ShowWindow(impl_->window,SW_HIDE); }
} // namespace velin
