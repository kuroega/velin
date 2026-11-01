// SPDX-License-Identifier: GPL-3.0-or-later
#include "velin/accessibility.hpp"
#include "velin/platform.hpp"
#include <oleacc.h>
#include <shellapi.h>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
struct Context {velin::AccessibilityProvider accessibility;velin::PlatformIntegration platform;int invoked=0;double value=0;};
LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wp,LPARAM lp){auto* context=reinterpret_cast<Context*>(GetWindowLongPtrW(window,GWLP_USERDATA));if(message==WM_NCCREATE){context=static_cast<Context*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(context));}if(context){if(auto result=context->accessibility.handleMessage(message,wp,lp))return *result;if(context->platform.handleMessage(message,wp,lp))return 0;}return DefWindowProcW(window,message,wp,lp);}
void pump(){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}}
}
int main(){CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);try{
    const auto cwd = std::filesystem::current_path();
    std::vector<std::wstring> launch{L"--command", L"seek 5", L"--data-dir", L"C:\\Settings", L"--enqueue",
        L"media folder\\\u5f71\u7247.mkv", L"https://example.com/video", L"--", L"--fullscreen"};
    velin::resolveLaunchPaths(launch);
    check(launch[1]==L"seek 5" && launch[3]==L"C:\\Settings" && launch[4]==L"--enqueue", "Launch options retain their values");
    check(launch[5]==(cwd / L"media folder" / L"\u5f71\u7247.mkv").lexically_normal().wstring(), "Relative Unicode media path uses sender directory");
    check(launch[6]==L"https://example.com/video" && launch[7]==L"--", "URLs and option separator remain unchanged");
    check(launch[8]==(cwd / L"--fullscreen").lexically_normal().wstring(), "Option-like media after separator is resolved");
    const std::vector<std::wstring> quotedValues{L"", L"simple", L"space and \u5f71\u7247", L"quote\"inside", L"trailing slash\\", L"slashes\\\\\"quote"};
    std::wstring commandLine = L"velin.exe";
    for (const auto& value : quotedValues) commandLine += L" " + velin::quoteWindowsArgument(value);
    int parsedCount = 0;
    auto* parsed = CommandLineToArgvW(commandLine.c_str(), &parsedCount);
    check(parsed && parsedCount == static_cast<int>(quotedValues.size()) + 1, "Quoted argument count round trips");
    for (size_t i = 0; i < quotedValues.size(); ++i)
        check(quotedValues[i] == parsed[i + 1], "Quoted argument value round trips");
    LocalFree(parsed);
    std::cout<<"PASS launch path resolution and Windows argument quoting\n";
    Context context;WNDCLASSW type{};type.lpfnWndProc=procedure;type.hInstance=GetModuleHandleW(nullptr);type.lpszClassName=L"Velin.PlatformTest";RegisterClassW(&type);
    HWND window=CreateWindowExW(0,type.lpszClassName,L"Velin platform test",WS_OVERLAPPEDWINDOW,0,0,640,480,nullptr,nullptr,type.hInstance,&context);check(window!=nullptr,"Create platform test window");
    context.accessibility.initialize(window,[&](int id,double value){context.invoked=id;context.value=value;});
    velin::AccessibleNode play;play.id=1;play.name=L"Play media";play.bounds={10,10,100,40};
    velin::AccessibleNode volume;volume.id=2;volume.role=velin::AccessibleRole::Slider;volume.name=L"Volume";volume.bounds={10,60,300,100};volume.value=70;
    context.accessibility.update({play,volume});
    IAccessible* accessible=nullptr;check(SUCCEEDED(AccessibleObjectFromWindow(window,OBJID_CLIENT,IID_IAccessible,reinterpret_cast<void**>(&accessible))),"MSAA client obtains provider");
    long count=0;check(SUCCEEDED(accessible->get_accChildCount(&count))&&count==2,"MSAA exposes custom player controls");
    VARIANT child{};child.vt=VT_I4;child.lVal=1;BSTR name=nullptr;check(SUCCEEDED(accessible->get_accName(child,&name))&&std::wstring_view(name)==L"Play media","MSAA accessible button label");SysFreeString(name);
    check(SUCCEEDED(accessible->accDoDefaultAction(child)),"MSAA invoke accepted");pump();check(context.invoked==1,"Accessibility invoke dispatches to UI thread");
    child.lVal=2;BSTR newValue=SysAllocString(L"45");check(SUCCEEDED(accessible->put_accValue(child,newValue)),"MSAA slider accepts value");SysFreeString(newValue);pump();check(context.invoked==2&&context.value==45,"Accessible slider updates player value");accessible->Release();
    context.platform.initialize(window,[&](velin::PlatformCommand,double){});context.platform.update({L"Fixture",L"Artist",L"Album",1,4,1,false,false,true,false,true});
    std::cout<<"PASS native MSAA discovery, labels, invoke and slider dispatch; SMTC="<<context.platform.mediaControlsAvailable()<<"\n";
    context.accessibility.shutdown();DestroyWindow(window);pump();
    // Render local text for the Windows OCR pipeline. No network service is used.
    constexpr unsigned width=640,height=180;BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-static_cast<LONG>(height);info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void* pixels=nullptr;HDC dc=CreateCompatibleDC(nullptr);HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);HGDIOBJ oldBitmap=SelectObject(dc,bitmap);RECT area{0,0,width,height};FillRect(dc,&area,static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    HFONT font=CreateFontW(80,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");HGDIOBJ oldFont=SelectObject(dc,font);SetTextColor(dc,RGB(0,0,0));SetBkColor(dc,RGB(255,255,255));DrawTextW(dc,L"Velin 42",-1,&area,DT_CENTER|DT_VCENTER|DT_SINGLELINE);GdiFlush();
    std::vector<std::uint8_t> image(static_cast<std::uint8_t*>(pixels),static_cast<std::uint8_t*>(pixels)+width*height*4);for(size_t i=3;i<image.size();i+=4)image[i]=255;
    SelectObject(dc,oldFont);SelectObject(dc,oldBitmap);DeleteObject(font);DeleteObject(bitmap);DeleteDC(dc);
    const auto recognized=velin::recognizeTextBgra(width,height,image);
    if(recognized.error.empty()){check(recognized.text.find(L"42")!=std::wstring::npos,"Windows OCR recognizes generated frame text");std::cout<<"PASS Windows OCR actual BGRA text recognition\n";}else std::wcout<<L"OCR unavailable on this Windows installation: "<<recognized.error<<L"\n";
    CoUninitialize();return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<"\n";CoUninitialize();return 1;}}
