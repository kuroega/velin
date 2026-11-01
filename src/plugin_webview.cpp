// SPDX-License-Identifier: GPL-3.0-or-later
#include "velin/plugin_webview.hpp"
#include "velin/core.hpp"
#include "velin/platform.hpp"
#include "velin/plugin_url_policy.hpp"
#include <objbase.h>
#include <objidl.h>
#include "webview2/WebView2.h"
#include <shellapi.h>
#include <shlwapi.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <deque>
#include <fstream>
#include <stdexcept>

namespace velin {
namespace {
template<class T> void drop(T*& pointer) { if (pointer) { pointer->Release(); pointer=nullptr; } }
template<class Interface,class... Arguments> class Callback final:public Interface {
public:
    using Function=std::function<HRESULT(Arguments...)>;
    Callback(const IID& iid,Function function):iid_(iid),function_(std::move(function)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** result) override {
        if (!result) return E_POINTER;
        *result=nullptr;
        if (IsEqualIID(iid,IID_IUnknown) || IsEqualIID(iid,iid_)) { *result=static_cast<Interface*>(this); AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto left=--references_; if (!left) delete this; return left; }
    HRESULT STDMETHODCALLTYPE Invoke(Arguments... arguments) override {
        try { return function_(arguments...); } catch (...) { return E_FAIL; }
    }
private:
    std::atomic<ULONG> references_{1};
    IID iid_;
    Function function_;
};
std::string argument(const Json& arguments,size_t index,std::string fallback={}) {
    if (arguments.is_array() && index<arguments.size() && arguments[index].is_string()) return arguments[index].get<std::string>();
    return fallback;
}
constexpr const wchar_t* bridge=LR"JS((()=>{
const listeners=new Map();
const api={postMessage:(name,data)=>window.chrome.webview.postMessage({name,data}),
onMessage:(name,callback)=>{listeners.set(name,callback);},
_emit:(name,data)=>{const cb=listeners.get(name);if(cb)cb(data);}};
window.iina=api;
window.chrome.webview.addEventListener('message',event=>{
const value=event.data;if(value&&typeof value.name==='string')api._emit(value.name,value.data);
});
})();)JS";
constexpr const wchar_t* simpleDocument=LR"HTML(<!DOCTYPE html><html><head><meta charset="UTF-8"><style>
html,body{margin:0;padding:0;background:transparent;font:13px 'Segoe UI',sans-serif;color:#eee}
</style><style id="style"></style></head><body><div id="content"></div></body></html>)HTML";
}

struct PluginWebView::Impl:std::enable_shared_from_this<Impl> {
    HWND owner=nullptr,window=nullptr;
    std::string kind,lastError;
    std::filesystem::path pluginDirectory,dataDirectory;
    std::vector<std::string> domains;
    Message message;
    ICoreWebView2Environment* environment=nullptr;
    ICoreWebView2Controller* controller=nullptr;
    ICoreWebView2* view=nullptr;
    ICoreWebView2WebResourceResponse* deniedResponse=nullptr;
    bool closed=false,ready=false,simple=false,desiredVisible=false,documentReady=false;
    std::deque<std::pair<std::string,Json>> queue;
    RECT bounds{};

    ~Impl() { close(); }
    void close() {
        closed=true;
        if (controller) controller->Close();
        drop(deniedResponse); drop(view); drop(controller); drop(environment);
        if (window) { SetWindowLongPtrW(window,GWLP_USERDATA,0); DestroyWindow(window); window=nullptr; }
    }
    bool allowed(const std::string& uri) const {
        return pluginWebViewAllowsUrl(uri,domains);
    }
    void resize() {
        if (controller && window) { RECT client{}; GetClientRect(window,&client); controller->put_Bounds(client); }
    }
    static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if (message==WM_NCCREATE) {
            self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if (self) {
            if (message==WM_SIZE) { self->resize(); return 0; }
            if (message==WM_CLOSE) { self->desiredVisible=false; ShowWindow(window,SW_HIDE); return 0; }
            if (message==WM_ERASEBKGND) return 1;
        }
        return DefWindowProcW(window,message,wparam,lparam);
    }
    void create() {
        WNDCLASSW type{}; type.hInstance=GetModuleHandleW(nullptr); type.lpfnWndProc=procedure;
        type.hCursor=LoadCursorW(nullptr,IDC_ARROW); type.hbrBackground=static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        type.lpszClassName=L"Velin.PluginWebView"; RegisterClassW(&type);
        const bool standalone=kind=="standalone";
        window=CreateWindowExW(WS_EX_TOOLWINDOW | (standalone?0:WS_EX_NOACTIVATE),type.lpszClassName,
            L"Velin plugin",standalone?(WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN):(WS_POPUP|WS_CLIPCHILDREN),
            CW_USEDEFAULT,CW_USEDEFAULT,640,480,owner,nullptr,type.hInstance,this);
        if (!window) throw std::runtime_error("Cannot create the plugin HTML window.");
        using CreateEnvironment=HRESULT(STDAPICALLTYPE*)(PCWSTR,PCWSTR,ICoreWebView2EnvironmentOptions*,ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
        // Keep the loader mapped for the lifetime of all WebView2 callbacks.
        static HMODULE loader=LoadLibraryExW((executableDirectory()/L"WebView2Loader.dll").c_str(),nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!loader) throw std::runtime_error("WebView2Loader.dll is missing. Run scripts/Get-WebView2.ps1 and rebuild.");
        const auto create=std::bit_cast<CreateEnvironment>(GetProcAddress(loader,"CreateCoreWebView2EnvironmentWithOptions"));
        if (!create) throw std::runtime_error("The WebView2 loader does not expose its environment API.");
        std::filesystem::create_directories(dataDirectory);
        auto weak=weak_from_this();
        using EnvironmentReady=Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,HRESULT,ICoreWebView2Environment*>;
        auto* callback=new EnvironmentReady(IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
            [weak](HRESULT result,ICoreWebView2Environment* environment)->HRESULT {
                auto self=weak.lock(); if (!self || self->closed) return S_OK;
                if (FAILED(result) || !environment) { self->lastError="Microsoft Edge WebView2 Runtime is not installed. Install the Evergreen WebView2 Runtime to use plugin HTML views."; return S_OK; }
                self->environment=environment; environment->AddRef();
                using ControllerReady=Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,HRESULT,ICoreWebView2Controller*>;
                auto* completion=new ControllerReady(IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                    [weak](HRESULT status,ICoreWebView2Controller* controller)->HRESULT {
                        auto owner=weak.lock(); if (!owner || owner->closed) return S_OK;
                        if (FAILED(status) || !controller) { owner->lastError="WebView2 could not create a plugin view."; return S_OK; }
                        owner->controller=controller; controller->AddRef();
                        controller->get_CoreWebView2(&owner->view);
                        owner->configure(); return S_OK;
                    });
                const HRESULT status=environment->CreateCoreWebView2Controller(self->window,completion);
                completion->Release();
                return status;
            });
        const HRESULT result=create(nullptr,dataDirectory.c_str(),nullptr,callback);
        callback->Release();
        if (FAILED(result)) throw std::runtime_error("WebView2 Runtime could not start. Install Microsoft's Evergreen WebView2 Runtime.");
    }
    void configure() {
        if (!view) return;
        ICoreWebView2Settings* settings=nullptr;
        if (SUCCEEDED(view->get_Settings(&settings))) {
            settings->put_IsWebMessageEnabled(TRUE); settings->put_AreDevToolsEnabled(FALSE);
            settings->put_AreDefaultContextMenusEnabled(FALSE); settings->put_AreHostObjectsAllowed(FALSE);
            settings->Release();
        }
        ICoreWebView2_3* mapping=nullptr;
        if (SUCCEEDED(view->QueryInterface(IID_ICoreWebView2_3,reinterpret_cast<void**>(&mapping)))) {
            mapping->SetVirtualHostNameToFolderMapping(L"velin.plugin",pluginDirectory.c_str(),COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
            mapping->Release();
        } else { lastError="This WebView2 Runtime is too old for secure plugin folder mapping."; return; }
        if (kind=="overlay") {
            ICoreWebView2Controller2* transparent=nullptr;
            if (SUCCEEDED(controller->QueryInterface(IID_ICoreWebView2Controller2,reinterpret_cast<void**>(&transparent)))) {
                transparent->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{0,0,0,0}); transparent->Release();
            }
        }
        view->AddScriptToExecuteOnDocumentCreated(bridge,nullptr);
        auto weak=weak_from_this();
        using MessageHandler=Callback<ICoreWebView2WebMessageReceivedEventHandler,ICoreWebView2*,ICoreWebView2WebMessageReceivedEventArgs*>;
        auto* messages=new MessageHandler(IID_ICoreWebView2WebMessageReceivedEventHandler,
            [weak](ICoreWebView2*,ICoreWebView2WebMessageReceivedEventArgs* event)->HRESULT {
                auto self=weak.lock(); if (!self || self->closed) return S_OK;
                LPWSTR source=nullptr,raw=nullptr;
                event->get_Source(&source); event->get_WebMessageAsJson(&raw);
                const bool trusted=source && toUtf8(source).starts_with("https://velin.plugin/");
                if (raw && (trusted || (source && std::wstring_view(source)==L"about:blank"))) {
                    const auto json=Json::parse(toUtf8(raw),nullptr,false);
                    if (!json.is_discarded() && json.is_object() && json.contains("name") && json["name"].is_string() && self->message)
                        self->message(json["name"].get<std::string>(),json.value("data",Json{}));
                }
                CoTaskMemFree(source); CoTaskMemFree(raw); return S_OK;
            });
        EventRegistrationToken token{}; view->add_WebMessageReceived(messages,&token); messages->Release();
        using Navigation=Callback<ICoreWebView2NavigationStartingEventHandler,ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs*>;
        auto* navigation=new Navigation(IID_ICoreWebView2NavigationStartingEventHandler,
            [weak](ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs* event)->HRESULT {
                auto self=weak.lock(); LPWSTR uri=nullptr; event->get_Uri(&uri);
                if (!self || !uri || !self->allowed(toUtf8(uri))) event->put_Cancel(TRUE);
                CoTaskMemFree(uri); return S_OK;
            });
        const auto navigationResult=view->add_NavigationStarting(navigation,&token); navigation->Release();
        if (FAILED(navigationResult)) { lastError="WebView2 could not install the plugin navigation filter."; return; }
        ICoreWebView2_22* resourceFilter=nullptr;
        if (FAILED(view->QueryInterface(IID_ICoreWebView2_22,reinterpret_cast<void**>(&resourceFilter)))) {
            lastError="This WebView2 Runtime is too old to securely filter plugin requests. Update the WebView2 Runtime."; return;
        }
        const auto filterResult=resourceFilter->AddWebResourceRequestedFilterWithRequestSourceKinds(
            L"*",COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL,COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS_ALL);
        resourceFilter->Release();
        if (FAILED(filterResult)) { lastError="WebView2 could not install the plugin resource filter."; return; }
        if (FAILED(environment->CreateWebResourceResponse(nullptr,403,L"Permission denied",L"Content-Type: text/plain",&deniedResponse)) || !deniedResponse) {
            lastError="WebView2 could not create the plugin permission response."; return;
        }
        using Resource=Callback<ICoreWebView2WebResourceRequestedEventHandler,ICoreWebView2*,ICoreWebView2WebResourceRequestedEventArgs*>;
        auto* resources=new Resource(IID_ICoreWebView2WebResourceRequestedEventHandler,
            [weak](ICoreWebView2*,ICoreWebView2WebResourceRequestedEventArgs* event)->HRESULT {
                auto self=weak.lock(); if (!self || self->closed) return S_OK;
                ICoreWebView2WebResourceRequest* request=nullptr; LPWSTR uri=nullptr;
                if (SUCCEEDED(event->get_Request(&request))) { request->get_Uri(&uri); request->Release(); }
                if (!uri || !self->allowed(toUtf8(uri))) {
                    const auto status=event->put_Response(self->deniedResponse);
                    if (FAILED(status)) {
                        CoTaskMemFree(uri);
                        self->lastError="WebView2 could not deny a plugin resource request.";
                        self->view->Stop(); self->close();
                        return status;
                    }
                }
                CoTaskMemFree(uri); return S_OK;
            });
        const auto resourceResult=view->add_WebResourceRequested(resources,&token); resources->Release();
        if (FAILED(resourceResult)) { lastError="WebView2 could not register the plugin resource handler."; return; }
        using Permission=Callback<ICoreWebView2PermissionRequestedEventHandler,ICoreWebView2*,ICoreWebView2PermissionRequestedEventArgs*>;
        auto* permission=new Permission(IID_ICoreWebView2PermissionRequestedEventHandler,
            [](ICoreWebView2*,ICoreWebView2PermissionRequestedEventArgs* event)->HRESULT { return event->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY); });
        view->add_PermissionRequested(permission,&token); permission->Release();
        // Keep every plugin document inside its permission-filtered controller.
        using Popup=Callback<ICoreWebView2NewWindowRequestedEventHandler,ICoreWebView2*,ICoreWebView2NewWindowRequestedEventArgs*>;
        auto* popup=new Popup(IID_ICoreWebView2NewWindowRequestedEventHandler,
            [](ICoreWebView2*,ICoreWebView2NewWindowRequestedEventArgs* event)->HRESULT { return event->put_Handled(TRUE); });
        view->add_NewWindowRequested(popup,&token); popup->Release();
        using Completed=Callback<ICoreWebView2NavigationCompletedEventHandler,ICoreWebView2*,ICoreWebView2NavigationCompletedEventArgs*>;
        auto* completed=new Completed(IID_ICoreWebView2NavigationCompletedEventHandler,
            [weak](ICoreWebView2*,ICoreWebView2NavigationCompletedEventArgs*)->HRESULT {
                if (auto self=weak.lock();self && !self->closed) self->flush();
                return S_OK;
            });
        view->add_NavigationCompleted(completed,&token); completed->Release();
        ready=true; resize(); controller->put_IsVisible(desiredVisible);
        auto operations=std::move(queue); queue.clear();
        for (auto& [operation,args]:operations) operate(operation,args);
    }
    void script(const std::string& code) { if (view) view->ExecuteScript(fromUtf8(code).c_str(),nullptr); }
    void flush() {
        documentReady=true;
        auto pending=std::move(queue); queue.clear();
        for (const auto& [operation,args]:pending) operate(operation,args);
    }
    Json operate(const std::string& operation,const Json& arguments) {
        if (closed) throw std::runtime_error("The plugin view has been closed.");
        if (!lastError.empty()) throw std::runtime_error(lastError);
        if (operation=="isOpen") return window && IsWindowVisible(window);
        if (!ready) { queue.emplace_back(operation,arguments); return nullptr; }
        if (operation=="show" || operation=="open") {
            desiredVisible=true; controller->put_IsVisible(TRUE); ShowWindow(window,kind=="standalone"?SW_SHOW:SW_SHOWNOACTIVATE);
        } else if (operation=="hide" || operation=="close") {
            desiredVisible=false; controller->put_IsVisible(FALSE); ShowWindow(window,SW_HIDE);
        } else if (operation=="loadFile") {
            auto path=std::filesystem::path(fromUtf8(argument(arguments,0))).lexically_normal();
            if (path.is_absolute()) path=std::filesystem::relative(std::filesystem::weakly_canonical(path),pluginDirectory);
            if (path.is_absolute() || path.empty() || *path.begin()==L"..") throw std::runtime_error("Plugin HTML paths must stay within the plugin directory.");
            std::string uri="https://velin.plugin/";
            for (const auto ch:toUtf8(path.generic_wstring())) {
                const auto c=static_cast<unsigned char>(ch);
                if (std::isalnum(c) || ch=='/' || ch=='-' || ch=='_' || ch=='.' || ch=='~') uri+=ch;
                else { constexpr char hex[]="0123456789ABCDEF"; uri+='%'; uri+=hex[c>>4]; uri+=hex[c&15]; }
            }
            const auto canonical=std::filesystem::weakly_canonical(pluginDirectory/path);
            const auto relative=std::filesystem::relative(canonical,pluginDirectory);
            if (relative.empty() || *relative.begin()==L"..") throw std::runtime_error("Plugin HTML paths cannot traverse outside the plugin directory.");
            simple=false; documentReady=false; view->Navigate(fromUtf8(uri).c_str());
        } else if (operation=="simpleMode") { simple=true; documentReady=false; view->NavigateToString(simpleDocument); }
        else if (operation=="setStyle" || operation=="setContent") {
            if (!simple) throw std::runtime_error("setStyle and setContent require simpleMode().");
            if (!documentReady) { queue.emplace_back(operation,arguments); return nullptr; }
            const auto content=Json(argument(arguments,0)).dump();
            const auto id=operation=="setStyle"?"style":"content";
            script("document.getElementById('"+std::string(id)+"')."+(operation=="setStyle"?"textContent":"innerHTML")+"="+content);
        } else if (operation=="postMessage") {
            const Json data=arguments.is_array() && arguments.size()>1?arguments[1]:Json{};
            view->PostWebMessageAsJson(fromUtf8(Json{{"name",argument(arguments,0)},{"data",data}}.dump()).c_str());
        } else if (operation=="setClickable") {
            const bool clickable=arguments.is_array() && !arguments.empty() && arguments[0].is_boolean() && arguments[0].get<bool>();
            auto style=GetWindowLongPtrW(window,GWL_EXSTYLE);
            SetWindowLongPtrW(window,GWL_EXSTYLE,clickable?style&~WS_EX_TRANSPARENT:style|WS_EX_TRANSPARENT);
        } else if (operation=="setOpacity") {
            const double opacity=arguments.is_array() && !arguments.empty() && arguments[0].is_number()?arguments[0].get<double>():1.;
            SetWindowLongPtrW(window,GWL_EXSTYLE,GetWindowLongPtrW(window,GWL_EXSTYLE)|WS_EX_LAYERED);
            SetLayeredWindowAttributes(window,0,static_cast<BYTE>(std::clamp(opacity,0.,1.)*255),LWA_ALPHA);
        } else if (operation=="setFrame") {
            RECT position{}; GetWindowRect(window,&position);
            auto value=[&](size_t index,int fallback) { return arguments.is_array() && index<arguments.size() && arguments[index].is_number()?arguments[index].get<int>():fallback; };
            SetWindowPos(window,nullptr,value(2,position.left),value(3,position.top),std::clamp(value(0,position.right-position.left),100,8192),
                std::clamp(value(1,position.bottom-position.top),60,8192),SWP_NOZORDER|SWP_NOACTIVATE);
        } else if (operation=="setProperty") {
            const Json properties=arguments.is_array() && !arguments.empty()?arguments[0]:arguments;
            if (!properties.is_object()) throw std::runtime_error("Window properties must be an object.");
            if (properties.contains("title") && properties["title"].is_string()) SetWindowTextW(window,fromUtf8(properties["title"].get<std::string>()).c_str());
            auto style=GetWindowLongPtrW(window,GWL_STYLE);
            if (properties.contains("resizable")) style=properties["resizable"].get<bool>()?style|WS_THICKFRAME:style&~WS_THICKFRAME;
            if (properties.contains("hideTitleBar")) style=properties["hideTitleBar"].get<bool>()?style&~WS_CAPTION:style|WS_CAPTION;
            if (properties.contains("fullSizeContentView")) style=properties["fullSizeContentView"].get<bool>()?style&~WS_CAPTION:style|WS_CAPTION;
            SetWindowLongPtrW(window,GWL_STYLE,style); SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
        } else throw std::runtime_error("Unknown plugin view operation: "+operation);
        return nullptr;
    }
};

PluginWebView::PluginWebView(HWND owner,std::string kind,std::filesystem::path directory,std::filesystem::path data,
    std::vector<std::string> domains,Message message):impl_(std::make_shared<Impl>()) {
    if (!pluginWebViewHasEnforceableNetworkPolicy(domains))
        throw std::runtime_error("Plugin HTML views are disabled because their WebSocket network permissions cannot be enforced.");
    impl_->owner=owner; impl_->kind=std::move(kind); impl_->pluginDirectory=std::filesystem::weakly_canonical(directory);
    impl_->dataDirectory=std::move(data); impl_->domains=std::move(domains); impl_->message=std::move(message); impl_->create();
}
PluginWebView::~PluginWebView() { impl_->close(); }
Json PluginWebView::operate(const std::string& operation,const Json& arguments) { return impl_->operate(operation,arguments); }
void PluginWebView::setBounds(RECT bounds) {
    impl_->bounds=bounds;
    if (impl_->window && impl_->kind!="standalone") SetWindowPos(impl_->window,HWND_TOP,bounds.left,bounds.top,
        std::max(1L,bounds.right-bounds.left),std::max(1L,bounds.bottom-bounds.top),SWP_NOACTIVATE);
}
bool PluginWebView::visible() const { return impl_->window && IsWindowVisible(impl_->window); }
std::string PluginWebView::error() const { return impl_->lastError; }
} // namespace velin
