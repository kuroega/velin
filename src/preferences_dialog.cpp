// SPDX-License-Identifier: GPL-3.0-or-later
#include "velin/preferences_dialog.hpp"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <utility>

namespace velin {
namespace {
constexpr int applyId=1001,defaultsId=1002,importId=1003;
constexpr int shortcutEditId=1101,shortcutListId=1102,shortcutKeyId=1103,shortcutCommandId=1104,
              shortcutSetId=1105,shortcutRemoveId=1106;
constexpr int historySearchId=2001,historyListId=2002,historyOpenId=2003,
              historyFavoriteId=2004,historyRemoveId=2005,historyClearId=2006,
              historyFavoritesId=2007,historySortId=2008,historyRevealId=2009,
              historyTrashId=2010;

std::wstring windowText(HWND window){const int count=GetWindowTextLengthW(window);std::wstring result(static_cast<std::size_t>(count)+1,L'\0');GetWindowTextW(window,result.data(),count+1);result.resize(static_cast<std::size_t>(count));return result;}
std::vector<std::string> choices(std::string_view value){std::vector<std::string> result;std::size_t begin=0;while(begin<=value.size()){const auto end=value.find('|',begin);result.emplace_back(value.substr(begin,end==std::string_view::npos?value.size()-begin:end-begin));if(end==std::string_view::npos)break;begin=end+1;}return result;}

HWND control(HWND parent,const wchar_t* type,const std::wstring& label,DWORD style,int id,HFONT font,DWORD extended=0){
  HWND window=CreateWindowExW(extended,type,label.c_str(),WS_CHILD|WS_VISIBLE|style,0,0,1,1,parent,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
  SendMessageW(window,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);return window;
}
HFONT makeFont(UINT dpi){return CreateFontW(-MulDiv(10,static_cast<int>(dpi),72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");}
void position(HWND window,float scale,int x,int y,int width,int height){MoveWindow(window,static_cast<int>(x*scale),static_cast<int>(y*scale),std::max(1,static_cast<int>(width*scale)),std::max(1,static_cast<int>(height*scale)),TRUE);}

template<class Context>
void modalLoop(HWND window,HWND parent,Context& context){
  EnableWindow(parent,FALSE);ShowWindow(window,SW_SHOWNORMAL);SetForegroundWindow(window);
  bool quit=false;MSG message{};
  while(!context.done){const int result=GetMessageW(&message,nullptr,0,0);if(result<=0){quit=result==0;break;}if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
  EnableWindow(parent,TRUE);DestroyWindow(window);if(IsWindow(parent))SetForegroundWindow(parent);if(quit)PostQuitMessage(static_cast<int>(message.wParam));
}

struct PreferencesContext;
struct PreferencePage {PreferencesContext* context=nullptr;std::string section;HWND window=nullptr;int scroll=0,contentHeight=0;};
struct FieldControl {const PreferenceField* field=nullptr;HWND label=nullptr,editor=nullptr;std::size_t page=0;int y=0,height=0;std::string initial;};
struct PreferencesContext {
  HWND window=nullptr,tab=nullptr,description=nullptr;
  std::array<HWND,5> buttons{};
  HFONT font=nullptr;
  float scale=1;
  Preferences* destination=nullptr;
  Preferences draft;
  SettingsStore* store=nullptr;
  std::function<void(const Preferences&)> onApply;
  std::string initialSection;
  std::vector<PreferencePage> pages;
  std::vector<FieldControl> fields;
  std::string shortcutText;
  bool done=false,applied=false;

  int selectedPage()const{return std::max(0,TabCtrl_GetCurSel(tab));}
  void layoutPage(std::size_t index){
    auto& page=pages[index];RECT bounds{};GetClientRect(page.window,&bounds);const int width=static_cast<int>(bounds.right/scale),height=static_cast<int>(bounds.bottom/scale);
    page.scroll=std::clamp(page.scroll,0,std::max(0,page.contentHeight-height));
    SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS,0,page.contentHeight,static_cast<UINT>(std::max(1,height)),page.scroll,0};SetScrollInfo(page.window,SB_VERT,&info,TRUE);
    for(const auto& field:fields)if(field.page==index){const int y=field.y-page.scroll;
      if(field.field->kind==PreferenceKind::Boolean)position(field.editor,scale,18,y,width-48,28);
      else if(field.field->key=="keyBindings"){position(field.label,scale,18,y,width-48,24);position(field.editor,scale,18,y+27,width-48,32);}
      else if(field.field->kind==PreferenceKind::Multiline){position(field.label,scale,18,y,width-48,24);position(field.editor,scale,18,y+27,width-48,107);}
      else {const int split=std::max(260,width/2);position(field.label,scale,18,y,split-27,27);position(field.editor,scale,split,y,width-split-30,field.field->kind==PreferenceKind::Choice?220:28);}
    }
  }
  void layout(){
    RECT bounds{};GetClientRect(window,&bounds);const int width=static_cast<int>(bounds.right/scale),height=static_cast<int>(bounds.bottom/scale);
    position(description,scale,18,12,width-36,30);position(tab,scale,12,47,width-24,height-109);
    RECT display{};GetWindowRect(tab,&display);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&display),2);TabCtrl_AdjustRect(tab,FALSE,&display);
    for(std::size_t i=0;i<pages.size();++i){MoveWindow(pages[i].window,display.left+2,display.top+2,std::max<LONG>(1,display.right-display.left-4),std::max<LONG>(1,display.bottom-display.top-4),TRUE);layoutPage(i);ShowWindow(pages[i].window,static_cast<int>(i)==selectedPage()?SW_SHOW:SW_HIDE);}
    position(buttons[0],scale,16,height-49,148,30);position(buttons[1],scale,174,height-49,104,30);
    position(buttons[2],scale,width-292,height-49,84,30);position(buttons[3],scale,width-198,height-49,84,30);position(buttons[4],scale,width-104,height-49,84,30);
  }
  void populate(const Preferences& value){
    draft=value;
    shortcutText=preferenceValue(value,"keyBindings");
    for(auto& field:fields){field.initial=preferenceValue(value,field.field->key);const auto wide=fromUtf8(field.initial);
      if(field.field->kind==PreferenceKind::Boolean)SendMessageW(field.editor,BM_SETCHECK,field.initial=="1"?BST_CHECKED:BST_UNCHECKED,0);
      else if(field.field->kind==PreferenceKind::Choice){const auto values=choices(field.field->choices);const auto selected=std::find(values.begin(),values.end(),field.initial);SendMessageW(field.editor,CB_SETCURSEL,selected==values.end()?0:static_cast<WPARAM>(selected-values.begin()),0);}
      else if(field.field->key=="keyBindings"){
        SetWindowTextW(field.editor,L"Edit shortcuts...");
      }
      else SetWindowTextW(field.editor,wide.c_str());
    }
  }
  bool collect(Preferences& candidate){
    candidate=draft;
    for(const auto& field:fields){std::string value;
      if(field.field->kind==PreferenceKind::Boolean)value=SendMessageW(field.editor,BM_GETCHECK,0,0)==BST_CHECKED?"1":"0";
      else if(field.field->key=="keyBindings")value=shortcutText;
      else value=toUtf8(windowText(field.editor));
      if(value==field.initial)continue;
      std::string error;
      if(!setPreferenceValue(candidate,field.field->key,value,error)){
        TabCtrl_SetCurSel(tab,static_cast<int>(field.page));layout();auto& page=pages[field.page];page.scroll=std::max(0,field.y-10);layoutPage(field.page);SetFocus(field.editor);
        MessageBoxW(window,fromUtf8(error).c_str(),L"Invalid preference",MB_OK|MB_ICONWARNING);return false;
      }
    }
    candidate=normalizePreferences(std::move(candidate));return true;
  }
  bool apply(){
    Preferences candidate;if(!collect(candidate))return false;
    if(!store->save(candidate)){MessageBoxW(window,L"The preferences file could not be written. Check that the application data folder is writable.",L"Preferences",MB_OK|MB_ICONERROR);return false;}
    *destination=candidate;populate(candidate);applied=true;
    if(onApply)onApply(candidate);
    return true;
  }
};

struct ShortcutContext {
  HWND window=nullptr,list=nullptr,key=nullptr,command=nullptr;
  std::array<HWND,4> buttons{};HFONT font=nullptr;float scale=1;
  std::map<std::string,std::string> values;bool done=false,accepted=false;
  void refresh(){ListView_DeleteAllItems(list);for(const auto& [keyText,commandText]:values){LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=ListView_GetItemCount(list);auto keyWide=fromUtf8(keyText);item.pszText=keyWide.data();const int row=ListView_InsertItem(list,&item);auto commandWide=fromUtf8(commandText);ListView_SetItemText(list,row,1,commandWide.data());}}
};

HWND createDialogWindow(HWND parent,const wchar_t* className,const wchar_t* title,WNDPROC procedure,void* context,int width,int height);

LRESULT CALLBACK shortcutProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam){
  auto* context=reinterpret_cast<ShortcutContext*>(GetWindowLongPtrW(window,GWLP_USERDATA));
  if(message==WM_NCCREATE){context=static_cast<ShortcutContext*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);context->window=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(context));}
  if(!context)return DefWindowProcW(window,message,wParam,lParam);
  switch(message){
    case WM_CREATE:{context->scale=GetDpiForWindow(window)/96.f;context->font=makeFont(GetDpiForWindow(window));
      context->list=control(window,WC_LISTVIEWW,L"",WS_TABSTOP|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS,shortcutListId,context->font,WS_EX_CLIENTEDGE);ListView_SetExtendedListViewStyle(context->list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
      for(int i=0;i<2;++i){LVCOLUMNW column{};column.mask=LVCF_TEXT|LVCF_WIDTH;column.pszText=const_cast<PWSTR>(i?L"Command":L"Key");column.cx=static_cast<int>((i?480:170)*context->scale);ListView_InsertColumn(context->list,i,&column);}
      context->key=control(window,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,shortcutKeyId,context->font,WS_EX_CLIENTEDGE);SendMessageW(context->key,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"Key, e.g. Ctrl+Shift+O"));
      context->command=control(window,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,shortcutCommandId,context->font,WS_EX_CLIENTEDGE);SendMessageW(context->command,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"Command, e.g. open-files or play-pause"));
      const int ids[]={shortcutSetId,shortcutRemoveId,IDOK,IDCANCEL};const wchar_t* labels[]={L"Add / Update",L"Remove",L"OK",L"Cancel"};for(int i=0;i<4;++i)context->buttons[i]=control(window,L"BUTTON",labels[i],WS_TABSTOP|(i==2?BS_DEFPUSHBUTTON:0),ids[i],context->font);context->refresh();return 0;}
    case WM_SIZE:{RECT rc{};GetClientRect(window,&rc);const int w=rc.right,h=rc.bottom;auto dip=[&](int value){return MulDiv(value,static_cast<int>(context->scale*96),96);};const int pad=dip(12),rowY=h-dip(130),buttonY=h-dip(48);MoveWindow(context->list,pad,pad,w-2*pad,h-dip(154),TRUE);MoveWindow(context->key,pad,rowY,dip(210),dip(27),TRUE);MoveWindow(context->command,dip(232),rowY,w-dip(244),dip(27),TRUE);for(int i=0;i<4;++i)MoveWindow(context->buttons[i],w-(4-i)*dip(94)-pad,buttonY,dip(84),dip(30),TRUE);return 0;}
    case WM_GETMINMAXINFO:{auto* info=reinterpret_cast<MINMAXINFO*>(lParam);info->ptMinTrackSize={static_cast<LONG>(580*context->scale),static_cast<LONG>(400*context->scale)};return 0;}
    case WM_DPICHANGED:{context->scale=HIWORD(wParam)/96.f;HFONT previous=context->font;context->font=makeFont(HIWORD(wParam));for(auto child:{context->list,context->key,context->command})SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(context->font),TRUE);for(auto button:context->buttons)SendMessageW(button,WM_SETFONT,reinterpret_cast<WPARAM>(context->font),TRUE);ListView_SetColumnWidth(context->list,0,MulDiv(170,HIWORD(wParam),96));ListView_SetColumnWidth(context->list,1,MulDiv(480,HIWORD(wParam),96));if(previous)DeleteObject(previous);const auto* bounds=reinterpret_cast<RECT*>(lParam);SetWindowPos(window,nullptr,bounds->left,bounds->top,bounds->right-bounds->left,bounds->bottom-bounds->top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;}
    case WM_NOTIFY:if(reinterpret_cast<NMHDR*>(lParam)->hwndFrom==context->list&&reinterpret_cast<NMHDR*>(lParam)->code==LVN_ITEMCHANGED){const int row=ListView_GetNextItem(context->list,-1,LVNI_SELECTED);if(row>=0){wchar_t key[256]{},command[512]{};ListView_GetItemText(context->list,row,0,key,256);ListView_GetItemText(context->list,row,1,command,512);SetWindowTextW(context->key,key);SetWindowTextW(context->command,command);}return 0;}break;
    case WM_COMMAND:{const int id=LOWORD(wParam);if(id==IDCANCEL){context->done=true;return 0;}if(id==IDOK){context->accepted=true;context->done=true;return 0;}if(id==shortcutRemoveId){const int row=ListView_GetNextItem(context->list,-1,LVNI_SELECTED);if(row>=0){wchar_t key[256]{};ListView_GetItemText(context->list,row,0,key,256);context->values.erase(toUtf8(key));context->refresh();}return 0;}if(id==shortcutSetId){const auto key=normalizeKeyBinding(toUtf8(windowText(context->key)));const auto command=toUtf8(windowText(context->command));auto candidate=context->values;if(!key.empty())candidate[key]=command;std::string serialized;for(const auto& [k,v]:candidate){if(!serialized.empty())serialized+='\n';serialized+=k+'='+v;}Preferences checked;std::string error;if(command.empty()||!setPreferenceValue(checked,"keyBindings",serialized,error)){MessageBoxW(window,L"Enter a supported key and a non-empty command.",L"Shortcut editor",MB_OK|MB_ICONWARNING);return 0;}context->values=std::move(candidate);context->refresh();return 0;}break;}
    case WM_CLOSE:context->done=true;return 0;
    case WM_DESTROY:if(context->font){DeleteObject(context->font);context->font=nullptr;}return 0;
    default:break;
  }
  return DefWindowProcW(window,message,wParam,lParam);
}

std::optional<std::map<std::string,std::string>> editShortcuts(HWND parent,const std::map<std::string,std::string>& values){
  INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_LISTVIEW_CLASSES};InitCommonControlsEx(&controls);ShortcutContext context;context.values=values;
  HWND window=createDialogWindow(parent,L"Velin.ShortcutEditor",L"Keyboard Shortcuts",shortcutProc,&context,760,560);if(!window)return std::nullopt;modalLoop(window,parent,context);if(context.accepted)return context.values;return std::nullopt;
}

bool openShortcutEditor(PreferencesContext& context,HWND owner,HWND button){
  const auto field=std::find_if(context.fields.begin(),context.fields.end(),[&](const auto& item){return item.field->key=="keyBindings"&&item.editor==button;});
  if(field==context.fields.end())return false;
  Preferences current=context.draft;std::string error;
  if(!setPreferenceValue(current,"keyBindings",context.shortcutText,error)){MessageBoxW(owner,fromUtf8(error).c_str(),L"Shortcut editor",MB_OK|MB_ICONWARNING);return true;}
  auto edited=editShortcuts(owner,current.keyBindings);if(!edited)return true;
  std::string serialized;for(const auto& [key,command]:*edited){if(!serialized.empty())serialized+='\n';serialized+=key+'='+command;}
  Preferences checked=context.draft;if(!setPreferenceValue(checked,"keyBindings",serialized,error)){MessageBoxW(owner,fromUtf8(error).c_str(),L"Shortcut editor",MB_OK|MB_ICONWARNING);return true;}
  context.shortcutText=std::move(serialized);SetWindowTextW(button,(std::to_wstring(edited->size())+L" shortcut overrides — Edit...").c_str());return true;
}

LRESULT CALLBACK preferencePageProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam){
  auto* page=reinterpret_cast<PreferencePage*>(GetWindowLongPtrW(window,GWLP_USERDATA));
  if(message==WM_NCCREATE){page=static_cast<PreferencePage*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(page));}
  if(!page||!page->context)return DefWindowProcW(window,message,wParam,lParam);
  auto& context=*page->context;
  auto scroll=[&](int amount){page->scroll+=amount;context.layoutPage(static_cast<std::size_t>(page-context.pages.data()));InvalidateRect(window,nullptr,TRUE);};
  switch(message){
    case WM_VSCROLL:{SCROLLINFO info{sizeof(info),SIF_ALL,0,0,0,0,0};GetScrollInfo(window,SB_VERT,&info);switch(LOWORD(wParam)){case SB_LINEUP:scroll(-28);break;case SB_LINEDOWN:scroll(28);break;case SB_PAGEUP:scroll(-static_cast<int>(info.nPage));break;case SB_PAGEDOWN:scroll(static_cast<int>(info.nPage));break;case SB_THUMBTRACK:scroll(info.nTrackPos-page->scroll);break;case SB_TOP:scroll(-page->scroll);break;case SB_BOTTOM:scroll(page->contentHeight);break;default:break;}return 0;}
    case WM_MOUSEWHEEL:scroll(-GET_WHEEL_DELTA_WPARAM(wParam)/WHEEL_DELTA*84);return 0;
    case WM_COMMAND:{
      const auto child=reinterpret_cast<HWND>(lParam);
      const auto pageIndex=static_cast<std::size_t>(page-context.pages.data());
      if(LOWORD(wParam)==shortcutEditId&&HIWORD(wParam)==BN_CLICKED){for(const auto& field:context.fields)if(field.field->key=="keyBindings"&&field.page==pageIndex){openShortcutEditor(context,context.window,field.editor);return 0;}}
      if(child&&GetFocus()==child){RECT childBounds{},pageBounds{};GetWindowRect(child,&childBounds);GetWindowRect(window,&pageBounds);if(childBounds.top<pageBounds.top)scroll(static_cast<int>((childBounds.top-pageBounds.top)/context.scale)-8);else if(childBounds.bottom>pageBounds.bottom)scroll(static_cast<int>((childBounds.bottom-pageBounds.bottom)/context.scale)+8);}
      return 0;
    }
    case WM_CTLCOLORSTATIC:SetBkMode(reinterpret_cast<HDC>(wParam),TRANSPARENT);return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    default:break;
  }
  return DefWindowProcW(window,message,wParam,lParam);
}

void importPreferences(PreferencesContext& context){
  IFileOpenDialog* dialog=nullptr;if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog))))return;
  const COMDLG_FILTERSPEC filters[]={{L"IINA preference property lists",L"*.plist;*.xml"},{L"All files",L"*.*"}};
  dialog->SetFileTypes(2,filters);dialog->SetTitle(L"Import exported IINA preferences");DWORD options=0;dialog->GetOptions(&options);dialog->SetOptions(options|FOS_FILEMUSTEXIST|FOS_FORCEFILESYSTEM);
  if(SUCCEEDED(dialog->Show(context.window))){IShellItem* item=nullptr;if(SUCCEEDED(dialog->GetResult(&item))){PWSTR path=nullptr;if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))){Preferences candidate;if(context.collect(candidate)){std::string report;const bool imported=importIinaPreferences(std::filesystem::path(path),candidate,report);if(imported)context.populate(candidate);MessageBoxW(context.window,fromUtf8(report).c_str(),L"Import IINA preferences",MB_OK|(imported?MB_ICONINFORMATION:MB_ICONERROR));}CoTaskMemFree(path);}item->Release();}}
  dialog->Release();
}

LRESULT CALLBACK preferencesProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam){
  auto* context=reinterpret_cast<PreferencesContext*>(GetWindowLongPtrW(window,GWLP_USERDATA));
  if(message==WM_NCCREATE){context=static_cast<PreferencesContext*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);context->window=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(context));}
  if(!context)return DefWindowProcW(window,message,wParam,lParam);
  switch(message){
    case WM_CREATE:{
      context->scale=GetDpiForWindow(window)/96.f;context->font=makeFont(GetDpiForWindow(window));
      context->description=control(window,L"STATIC",L"Changes apply when you choose Apply or OK. All settings are stored locally.",SS_LEFT,0,context->font);
      context->tab=control(window,WC_TABCONTROLW,L"",WS_TABSTOP|TCS_MULTILINE,1000,context->font,WS_EX_CONTROLPARENT);
      for(const auto& field:preferenceFields())if(std::none_of(context->pages.begin(),context->pages.end(),[&](const auto& page){return page.section==field.section;}))context->pages.push_back({context,field.section,nullptr,0,0});
      for(std::size_t i=0;i<context->pages.size();++i){auto& page=context->pages[i];auto label=fromUtf8(page.section);TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=label.data();TabCtrl_InsertItem(context->tab,static_cast<int>(i),&item);page.window=CreateWindowExW(WS_EX_CONTROLPARENT,L"Velin.PreferencePage",L"",WS_CHILD|WS_VSCROLL|WS_CLIPCHILDREN,0,0,1,1,window,nullptr,GetModuleHandleW(nullptr),&page);}
      for(const auto& field:preferenceFields()){
        const auto pageIndex=static_cast<std::size_t>(std::find_if(context->pages.begin(),context->pages.end(),[&](const auto& page){return page.section==field.section;})-context->pages.begin());auto& page=context->pages[pageIndex];
        FieldControl item;item.field=&field;item.page=pageIndex;item.y=page.contentHeight+12;item.height=field.key=="keyBindings"?76:field.kind==PreferenceKind::Boolean?34:field.kind==PreferenceKind::Multiline?148:46;
        const int id=field.key=="keyBindings"?shortcutEditId:3000+static_cast<int>(context->fields.size());
        if(field.kind==PreferenceKind::Boolean)item.editor=control(page.window,L"BUTTON",fromUtf8(field.label),WS_TABSTOP|BS_AUTOCHECKBOX,id,context->font);
        else{
          item.label=control(page.window,L"STATIC",fromUtf8(field.label),SS_LEFT,0,context->font);
          if(field.key=="keyBindings")item.editor=control(page.window,L"BUTTON",L"Edit shortcuts...",WS_TABSTOP,id,context->font);
          else if(field.kind==PreferenceKind::Choice){item.editor=control(page.window,L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,id,context->font);for(const auto& value:choices(field.choices))SendMessageW(item.editor,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(fromUtf8(value).c_str()));}
          else{const bool multiline=field.kind==PreferenceKind::Multiline;item.editor=control(page.window,L"EDIT",L"",WS_TABSTOP|(multiline?(ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN|WS_VSCROLL):(ES_AUTOHSCROLL)),id,context->font,WS_EX_CLIENTEDGE);SendMessageW(item.editor,EM_SETLIMITTEXT,131072,0);if(field.key=="subtitleProviderToken"||field.key=="subtitleProviderApiKey")SendMessageW(item.editor,EM_SETPASSWORDCHAR,static_cast<WPARAM>(L'\u2022'),0);}
        }
        page.contentHeight+=item.height;context->fields.push_back(std::move(item));
      }
      context->buttons[0]=control(window,L"BUTTON",L"Import IINA...",WS_TABSTOP,importId,context->font);
      context->buttons[1]=control(window,L"BUTTON",L"Defaults",WS_TABSTOP,defaultsId,context->font);
      context->buttons[2]=control(window,L"BUTTON",L"OK",WS_TABSTOP|BS_DEFPUSHBUTTON,IDOK,context->font);
      context->buttons[3]=control(window,L"BUTTON",L"Cancel",WS_TABSTOP,IDCANCEL,context->font);
      context->buttons[4]=control(window,L"BUTTON",L"Apply",WS_TABSTOP,applyId,context->font);
      int initial=0;for(std::size_t i=0;i<context->pages.size();++i)if(context->pages[i].section==context->initialSection)initial=static_cast<int>(i);
      TabCtrl_SetCurSel(context->tab,initial);context->populate(context->draft);context->layout();return 0;
    }
    case WM_SIZE:if(context->tab)context->layout();return 0;
    case WM_GETMINMAXINFO:{auto* info=reinterpret_cast<MINMAXINFO*>(lParam);info->ptMinTrackSize={static_cast<LONG>(740*context->scale),static_cast<LONG>(500*context->scale)};return 0;}
    case WM_DPICHANGED:{context->scale=HIWORD(wParam)/96.f;HFONT previous=context->font;context->font=makeFont(HIWORD(wParam));for(const auto& field:context->fields){SendMessageW(field.editor,WM_SETFONT,reinterpret_cast<WPARAM>(context->font),TRUE);if(field.label)SendMessageW(field.label,WM_SETFONT,reinterpret_cast<WPARAM>(context->font),TRUE);}for(auto button:context->buttons)SendMessageW(button,WM_SETFONT,reinterpret_cast<WPARAM>(context->font),TRUE);SendMessageW(context->tab,WM_SETFONT,reinterpret_cast<WPARAM>(context->font),TRUE);SendMessageW(context->description,WM_SETFONT,reinterpret_cast<WPARAM>(context->font),TRUE);if(previous)DeleteObject(previous);const auto* bounds=reinterpret_cast<RECT*>(lParam);SetWindowPos(window,nullptr,bounds->left,bounds->top,bounds->right-bounds->left,bounds->bottom-bounds->top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;}
    case WM_NOTIFY:if(reinterpret_cast<NMHDR*>(lParam)->hwndFrom==context->tab&&reinterpret_cast<NMHDR*>(lParam)->code==TCN_SELCHANGE){context->layout();return 0;}break;
    case WM_COMMAND:if(LOWORD(wParam)==shortcutEditId&&HIWORD(wParam)==BN_CLICKED){for(const auto& field:context->fields)if(field.field->key=="keyBindings"){openShortcutEditor(*context,window,field.editor);return 0;}}switch(LOWORD(wParam)){case IDOK:if(context->apply())context->done=true;return 0;case IDCANCEL:context->done=true;return 0;case applyId:context->apply();return 0;case defaultsId:context->populate(Preferences{});return 0;case importId:importPreferences(*context);return 0;default:break;}break;
    case WM_MOUSEWHEEL:if(!context->pages.empty())SendMessageW(context->pages[static_cast<std::size_t>(context->selectedPage())].window,message,wParam,lParam);return 0;
    case WM_CLOSE:context->done=true;return 0;
    case WM_DESTROY:if(context->font){DeleteObject(context->font);context->font=nullptr;}return 0;
    default:break;
  }
  return DefWindowProcW(window,message,wParam,lParam);
}

struct HistoryContext {
  HWND window=nullptr,search=nullptr,list=nullptr,favorites=nullptr,sort=nullptr,status=nullptr;
  std::array<HWND,5> buttons{};HFONT font=nullptr;float scale=1;
  HistoryStore* store=nullptr;std::vector<HistoryEntry> entries;std::wstring displayText;
  std::optional<HistoryEntry> selected;bool done=false;
  int savedWidth=980,savedHeight=620;
  void layout(){RECT bounds{};GetClientRect(window,&bounds);const int width=static_cast<int>(bounds.right/scale),height=static_cast<int>(bounds.bottom/scale);position(search,scale,16,17,width-415,29);position(favorites,scale,width-387,18,170,27);position(sort,scale,width-212,17,195,220);position(list,scale,16,59,width-32,height-127);position(status,scale,18,height-59,width-480,32);position(buttons[0],scale,width-452,height-52,80,31);position(buttons[1],scale,width-364,height-52,98,31);position(buttons[2],scale,width-258,height-52,76,31);position(buttons[3],scale,width-174,height-52,76,31);position(buttons[4],scale,width-90,height-52,74,31);}
  int selection()const{return ListView_GetNextItem(list,-1,LVNI_SELECTED);}
  bool localFile(const HistoryEntry& entry)const {
    if(entry.path.empty()||entry.path.find("://")!=std::string::npos)return false;
    std::error_code error;
    return std::filesystem::is_regular_file(std::filesystem::path(fromUtf8(entry.path)),error) && !error;
  }
  void refresh(){const int choice=static_cast<int>(SendMessageW(sort,CB_GETCURSEL,0,0));const HistorySort sorts[]={HistorySort::Recent,HistorySort::Title,HistorySort::MostPlayed,HistorySort::Duration};entries=store->search(toUtf8(windowText(search)),sorts[std::clamp(choice,0,3)],SendMessageW(favorites,BM_GETCHECK,0,0)==BST_CHECKED);ListView_SetItemCountEx(list,static_cast<int>(entries.size()),LVSICF_NOSCROLL);InvalidateRect(list,nullptr,TRUE);SetWindowTextW(status,(std::to_wstring(entries.size())+L" matching entries").c_str());if(!store->error().empty())MessageBoxW(window,fromUtf8(store->error()).c_str(),L"Playback history",MB_OK|MB_ICONERROR);}
  std::wstring cell(int row,int column){if(row<0||static_cast<std::size_t>(row)>=entries.size())return{};const auto& entry=entries[static_cast<std::size_t>(row)];switch(column){case 0:return entry.favorite?L"Yes":L"";case 1:return fromUtf8(entry.title);case 2:{const auto time=static_cast<std::time_t>(entry.lastPlayed);const auto* value=std::localtime(&time);if(!value)return{};std::wostringstream out;out<<std::put_time(value,L"%Y-%m-%d %H:%M");return out.str();}case 3:return fromUtf8(formatTime(entry.position));case 4:return fromUtf8(formatTime(entry.duration));case 5:return std::to_wstring(entry.playCount);case 6:return fromUtf8(entry.path);default:return{};}}
  void action(int id){const int index=selection();if(id==historyOpenId){if(index>=0&&static_cast<std::size_t>(index)<entries.size()){selected=entries[static_cast<std::size_t>(index)];done=true;}return;}bool okay=true;if(id==historyClearId){if(MessageBoxW(window,L"Remove all playback history, including favorites? Media files are not changed.",L"Clear playback history",MB_YESNO|MB_ICONQUESTION)!=IDYES)return;okay=store->clear();}else if(index>=0&&static_cast<std::size_t>(index)<entries.size()){const auto entry=entries[static_cast<std::size_t>(index)];if(id==historyFavoriteId)okay=store->setFavorite(entry.path,!entry.favorite);else if(id==historyRemoveId)okay=store->remove(entry.path);
    else if(id==historyRevealId||id==historyTrashId){
      if(!localFile(entry)){MessageBoxW(window,L"The selected local file is unavailable.",L"Playback history",MB_OK|MB_ICONERROR);return;}
      const auto path=fromUtf8(entry.path);
      if(id==historyRevealId){PIDLIST_ABSOLUTE item=ILCreateFromPathW(path.c_str());HRESULT result=E_FAIL;if(item){result=SHOpenFolderAndSelectItems(item,0,nullptr,0);ILFree(item);}if(FAILED(result))MessageBoxW(window,L"Windows could not show this file in Explorer.",L"Playback history",MB_OK|MB_ICONERROR);return;}
      if(MessageBoxW(window,L"Move this file to the Recycle Bin and remove it from playback history?",L"Move to Recycle Bin",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES)return;
      IFileOperation* operation=nullptr;IShellItem* item=nullptr;BOOL aborted=FALSE;
      HRESULT result=CoCreateInstance(CLSID_FileOperation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&operation));
      if(SUCCEEDED(result))result=operation->SetOwnerWindow(window);
      if(SUCCEEDED(result))result=operation->SetOperationFlags(FOF_ALLOWUNDO|FOFX_RECYCLEONDELETE);
      if(SUCCEEDED(result))result=SHCreateItemFromParsingName(path.c_str(),nullptr,IID_PPV_ARGS(&item));
      if(SUCCEEDED(result))result=operation->DeleteItem(item,nullptr);
      if(SUCCEEDED(result))result=operation->PerformOperations();
      if(SUCCEEDED(result))result=operation->GetAnyOperationsAborted(&aborted);
      if(item)item->Release();
      if(operation)operation->Release();
      if(FAILED(result)){MessageBoxW(window,L"Windows could not move this file to the Recycle Bin.",L"Playback history",MB_OK|MB_ICONERROR);return;}
      if(aborted)return;
      std::error_code error;
      if(std::filesystem::exists(std::filesystem::path(path),error)||error){MessageBoxW(window,L"Windows could not move this file to the Recycle Bin.",L"Playback history",MB_OK|MB_ICONERROR);return;}
      okay=store->remove(entry.path);
    }}if(!okay)MessageBoxW(window,fromUtf8(store->error()).c_str(),L"Playback history",MB_OK|MB_ICONERROR);refresh();}
  void contextMenu(POINT point){
    const int index=selection();if(index<0||static_cast<std::size_t>(index)>=entries.size())return;
    if(point.x==-1&&point.y==-1){RECT row{};ListView_GetItemRect(list,index,&row,LVIR_BOUNDS);point={row.left,row.bottom};ClientToScreen(list,&point);}
    HMENU menu=CreatePopupMenu();if(!menu)return;
    AppendMenuW(menu,MF_STRING,historyOpenId,L"Open");
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    const UINT local=localFile(entries[static_cast<std::size_t>(index)])?MF_STRING:MF_STRING|MF_GRAYED;
    AppendMenuW(menu,local,historyRevealId,L"Show in Explorer");
    AppendMenuW(menu,local,historyTrashId,L"Move to Recycle Bin...");
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,historyRemoveId,L"Remove from history");
    const UINT command=TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,window,nullptr);DestroyMenu(menu);if(command)action(static_cast<int>(command));
  }
};

LRESULT CALLBACK historyProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam){
  auto* context=reinterpret_cast<HistoryContext*>(GetWindowLongPtrW(window,GWLP_USERDATA));
  if(message==WM_NCCREATE){context=static_cast<HistoryContext*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);context->window=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(context));}
  if(!context)return DefWindowProcW(window,message,wParam,lParam);
  switch(message){
    case WM_CREATE:{context->scale=GetDpiForWindow(window)/96.f;context->font=makeFont(GetDpiForWindow(window));context->search=control(window,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,historySearchId,context->font,WS_EX_CLIENTEDGE);SendMessageW(context->search,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"Search titles and paths"));context->favorites=control(window,L"BUTTON",L"Favorites only",WS_TABSTOP|BS_AUTOCHECKBOX,historyFavoritesId,context->font);context->sort=control(window,L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST,historySortId,context->font);for(const auto* label:{L"Recently played",L"Title",L"Most played",L"Duration"})SendMessageW(context->sort,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));SendMessageW(context->sort,CB_SETCURSEL,0,0);context->list=control(window,WC_LISTVIEWW,L"",WS_TABSTOP|LVS_REPORT|LVS_OWNERDATA|LVS_SHOWSELALWAYS|LVS_SINGLESEL,historyListId,context->font,WS_EX_CLIENTEDGE);ListView_SetExtendedListViewStyle(context->list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_LABELTIP);const wchar_t* columns[]={L"Favorite",L"Title",L"Last played",L"Position",L"Duration",L"Plays",L"Source"};const int widths[]={65,250,155,80,80,60,480};for(int i=0;i<7;++i){LVCOLUMNW column{};column.mask=LVCF_TEXT|LVCF_WIDTH;column.pszText=const_cast<PWSTR>(columns[i]);column.cx=static_cast<int>(widths[i]*context->scale);ListView_InsertColumn(context->list,i,&column);}context->status=control(window,L"STATIC",L"",SS_LEFT,0,context->font);const int ids[]={historyOpenId,historyFavoriteId,historyRemoveId,historyClearId,IDCANCEL};const wchar_t* names[]={L"Open",L"Favorite",L"Remove",L"Clear...",L"Close"};for(std::size_t i=0;i<5;++i)context->buttons[i]=control(window,L"BUTTON",names[i],WS_TABSTOP|(i==0?BS_DEFPUSHBUTTON:0),ids[i],context->font);context->layout();context->refresh();return 0;}
    case WM_SIZE:if(context->list){context->layout();if(wParam!=SIZE_MINIMIZED){RECT bounds{};GetClientRect(window,&bounds);context->savedWidth=static_cast<int>(bounds.right/context->scale);context->savedHeight=static_cast<int>(bounds.bottom/context->scale);}}return 0;
    case WM_GETMINMAXINFO:{auto* info=reinterpret_cast<MINMAXINFO*>(lParam);info->ptMinTrackSize={static_cast<LONG>(720*context->scale),static_cast<LONG>(440*context->scale)};return 0;}
    case WM_DPICHANGED:{context->scale=HIWORD(wParam)/96.f;const auto previous=context->font;context->font=makeFont(HIWORD(wParam));for(auto child:{context->search,context->list,context->favorites,context->sort,context->status})SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(context->font),TRUE);for(auto button:context->buttons)SendMessageW(button,WM_SETFONT,reinterpret_cast<WPARAM>(context->font),TRUE);if(previous)DeleteObject(previous);const int widths[]={65,250,155,80,80,60,480};for(int i=0;i<7;++i)ListView_SetColumnWidth(context->list,i,static_cast<int>(widths[i]*context->scale));const auto* bounds=reinterpret_cast<RECT*>(lParam);SetWindowPos(window,nullptr,bounds->left,bounds->top,bounds->right-bounds->left,bounds->bottom-bounds->top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;}
    case WM_COMMAND:{const int id=LOWORD(wParam);if(id==IDCANCEL){context->done=true;return 0;}if(id==IDOK){context->action(historyOpenId);return 0;}if(id==historySearchId&&HIWORD(wParam)==EN_CHANGE){SetTimer(window,44,180,nullptr);return 0;}if((id==historyFavoritesId&&HIWORD(wParam)==BN_CLICKED)||(id==historySortId&&HIWORD(wParam)==CBN_SELCHANGE)){context->refresh();return 0;}if(id==historyOpenId||id==historyFavoriteId||id==historyRemoveId||id==historyClearId||id==historyRevealId||id==historyTrashId){context->action(id);return 0;}break;}
    case WM_CONTEXTMENU:if(reinterpret_cast<HWND>(wParam)==context->list){context->contextMenu({GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)});return 0;}break;
    case WM_TIMER:if(wParam==44){KillTimer(window,44);context->refresh();}return 0;
    case WM_NOTIFY:{const auto* header=reinterpret_cast<NMHDR*>(lParam);if(header->hwndFrom==context->list){if(header->code==LVN_GETDISPINFOW){auto* info=reinterpret_cast<NMLVDISPINFOW*>(lParam);if(info->item.mask&LVIF_TEXT){context->displayText=context->cell(info->item.iItem,info->item.iSubItem);info->item.pszText=context->displayText.data();}return 0;}if(header->code==NM_DBLCLK){context->action(historyOpenId);return 0;}if(header->code==LVN_KEYDOWN){const auto key=reinterpret_cast<NMLVKEYDOWN*>(lParam)->wVKey;if(key==VK_DELETE)context->action(historyRemoveId);else if(key==VK_RETURN)context->action(historyOpenId);return 0;}if(header->code==LVN_COLUMNCLICK){const auto column=reinterpret_cast<NMLISTVIEW*>(lParam)->iSubItem;const int sort=column==1?1:column==5?2:column==4?3:0;SendMessageW(context->sort,CB_SETCURSEL,sort,0);context->refresh();return 0;}}break;}
    case WM_CLOSE:context->done=true;return 0;
    case WM_DESTROY:KillTimer(window,44);if(context->font){DeleteObject(context->font);context->font=nullptr;}return 0;
    default:break;
  }
  return DefWindowProcW(window,message,wParam,lParam);
}

HWND createDialogWindow(HWND parent,const wchar_t* className,const wchar_t* title,WNDPROC procedure,void* context,int width,int height){
  WNDCLASSW type{};type.lpfnWndProc=procedure;type.hInstance=GetModuleHandleW(nullptr);type.hCursor=LoadCursorW(nullptr,IDC_ARROW);type.hbrBackground=GetSysColorBrush(COLOR_WINDOW);type.lpszClassName=className;RegisterClassW(&type);
  const auto dpi=GetDpiForWindow(parent);const float scale=dpi/96.f;RECT parentBounds{};GetWindowRect(parent,&parentBounds);RECT size{0,0,static_cast<LONG>(width*scale),static_cast<LONG>(height*scale)};AdjustWindowRectExForDpi(&size,WS_OVERLAPPEDWINDOW,FALSE,WS_EX_CONTROLPARENT,dpi);
  const int actualWidth=size.right-size.left,actualHeight=size.bottom-size.top;
  return CreateWindowExW(WS_EX_CONTROLPARENT,className,title,WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,parentBounds.left+std::max<LONG>(0,((parentBounds.right-parentBounds.left)-actualWidth)/2),parentBounds.top+std::max<LONG>(0,((parentBounds.bottom-parentBounds.top)-actualHeight)/2),actualWidth,actualHeight,parent,nullptr,GetModuleHandleW(nullptr),context);
}
} // namespace

bool showPreferencesDialog(HWND parent,Preferences& preferences,SettingsStore& store,std::function<void(const Preferences&)> onApply,std::string initialSection){
  INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_TAB_CLASSES|ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
  WNDCLASSW page{};page.lpfnWndProc=preferencePageProc;page.hInstance=GetModuleHandleW(nullptr);page.hCursor=LoadCursorW(nullptr,IDC_ARROW);page.hbrBackground=GetSysColorBrush(COLOR_WINDOW);page.lpszClassName=L"Velin.PreferencePage";RegisterClassW(&page);
  PreferencesContext context;context.destination=&preferences;context.draft=normalizePreferences(preferences);context.store=&store;context.onApply=std::move(onApply);context.initialSection=std::move(initialSection);
  HWND window=createDialogWindow(parent,L"Velin.Preferences",L"Velin Preferences",preferencesProc,&context,820,640);if(!window)return false;
  modalLoop(window,parent,context);return context.applied;
}

void showHistoryDialog(HWND parent,HistoryStore& history,std::function<void(const HistoryEntry&)> onOpen,Preferences* preferences,SettingsStore* settings){
  INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_LISTVIEW_CLASSES|ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
  HistoryContext context;context.store=&history;if(preferences){context.savedWidth=preferences->historyWidth;context.savedHeight=preferences->historyHeight;}
  HWND window=createDialogWindow(parent,L"Velin.History",L"Playback History",historyProc,&context,context.savedWidth,context.savedHeight);if(!window)return;
  modalLoop(window,parent,context);
  if(preferences){preferences->historyWidth=context.savedWidth;preferences->historyHeight=context.savedHeight;if(settings)(void)settings->save(*preferences);}
  if(context.selected&&onOpen)onOpen(*context.selected);
}
} // namespace velin
