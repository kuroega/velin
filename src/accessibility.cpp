// SPDX-License-Identifier: GPL-3.0-or-later
#include "velin/accessibility.hpp"
#include <oleacc.h>
#include <uiautomation.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <deque>
#include <mutex>
#include <utility>

namespace velin {
namespace {
constexpr UINT accessibilityMessage = WM_APP + 62;
struct AccessibleAction { int id; double value; bool focus; };
struct AccessibleState {
    std::mutex mutex;
    HWND window{};
    std::vector<AccessibleNode> nodes;
    std::deque<AccessibleAction> pending;
    std::vector<AccessibleNode> snapshot() { std::lock_guard lock(mutex); return nodes; }
    std::optional<AccessibleNode> node(int id) {
        std::lock_guard lock(mutex);
        auto item = std::find_if(nodes.begin(), nodes.end(), [id](const auto& n) { return n.id == id; });
        return item == nodes.end() ? std::nullopt : std::optional<AccessibleNode>(*item);
    }
    HWND target() { std::lock_guard lock(mutex); return window; }
    HRESULT enqueue(int id, double value, bool focus = false) {
        std::lock_guard lock(mutex);
        if (!window) return UIA_E_ELEMENTNOTAVAILABLE;
        if (pending.size() >= 256) return HRESULT_FROM_WIN32(ERROR_BUSY);
        pending.push_back({id, value, focus});
        PostMessageW(window, accessibilityMessage, 0, 0);
        return S_OK;
    }
};
CONTROLTYPEID controlType(AccessibleRole role) {
    switch (role) {
    case AccessibleRole::Slider: return UIA_SliderControlTypeId;
    case AccessibleRole::List: return UIA_ListControlTypeId;
    case AccessibleRole::ListItem: return UIA_ListItemControlTypeId;
    case AccessibleRole::Tab: return UIA_TabItemControlTypeId;
    case AccessibleRole::Text: return UIA_TextControlTypeId;
    default: return UIA_ButtonControlTypeId;
    }
}
long msaaRole(AccessibleRole role) {
    switch (role) {
    case AccessibleRole::Slider: return ROLE_SYSTEM_SLIDER;
    case AccessibleRole::List: return ROLE_SYSTEM_LIST;
    case AccessibleRole::ListItem: return ROLE_SYSTEM_LISTITEM;
    case AccessibleRole::Tab: return ROLE_SYSTEM_PAGETAB;
    case AccessibleRole::Text: return ROLE_SYSTEM_STATICTEXT;
    default: return ROLE_SYSTEM_PUSHBUTTON;
    }
}
HRESULT bstr(std::wstring_view value, BSTR* result) {
    if (!result) return E_POINTER;
    *result = SysAllocStringLen(value.data(), static_cast<UINT>(value.size()));
    return *result ? S_OK : E_OUTOFMEMORY;
}

class ElementProvider final : public IRawElementProviderSimple, public IRawElementProviderFragment, public IRawElementProviderFragmentRoot,
    public IInvokeProvider, public IRangeValueProvider, public IValueProvider,
    public ISelectionProvider, public ISelectionItemProvider, public IAccessible {
public:
    ElementProvider(std::shared_ptr<AccessibleState> state, int id = 0) : state_(std::move(state)), id_(id) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        const auto node = state_->node(id_);
        const bool slider = node && node->role == AccessibleRole::Slider;
        const bool item = node && (node->role == AccessibleRole::ListItem || node->role == AccessibleRole::Tab);
        if (iid == IID_IUnknown || iid == __uuidof(IRawElementProviderSimple) || iid == IID_IAgileObject)
            *result = static_cast<IRawElementProviderSimple*>(this);
        else if (iid == __uuidof(IRawElementProviderFragment)) *result = static_cast<IRawElementProviderFragment*>(this);
        else if (iid == __uuidof(IRawElementProviderFragmentRoot) && !id_) *result = static_cast<IRawElementProviderFragmentRoot*>(this);
        else if (iid == __uuidof(IInvokeProvider) && node && node->role != AccessibleRole::Text && node->role != AccessibleRole::List)
            *result = static_cast<IInvokeProvider*>(this);
        else if (iid == __uuidof(IRangeValueProvider) && slider) *result = static_cast<IRangeValueProvider*>(this);
        else if (iid == __uuidof(IValueProvider) && slider) *result = static_cast<IValueProvider*>(this);
        else if (iid == __uuidof(ISelectionProvider) && (!id_ || (node && node->role == AccessibleRole::List))) *result = static_cast<ISelectionProvider*>(this);
        else if (iid == __uuidof(ISelectionItemProvider) && item) *result = static_cast<ISelectionItemProvider*>(this);
        else if (iid == IID_IAccessible || iid == IID_IDispatch) *result = static_cast<IAccessible*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto value = --references_; if (!value) delete this; return value; }
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* result) override {
        if (!result) return E_POINTER;
        *result = ProviderOptions_ServerSideProvider; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern, IUnknown** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (!alive()) return UIA_E_ELEMENTNOTAVAILABLE;
        const auto node = state_->node(id_);
        const bool slider = node && node->role == AccessibleRole::Slider;
        if (pattern == UIA_InvokePatternId && node && node->role != AccessibleRole::Text && node->role != AccessibleRole::List)
            *result = static_cast<IInvokeProvider*>(this);
        else if (pattern == UIA_RangeValuePatternId && slider) *result = static_cast<IRangeValueProvider*>(this);
        else if (pattern == UIA_ValuePatternId && slider) *result = static_cast<IValueProvider*>(this);
        else if (pattern == UIA_SelectionPatternId && (!id_ || (node && node->role == AccessibleRole::List))) *result = static_cast<ISelectionProvider*>(this);
        else if (pattern == UIA_SelectionItemPatternId && node && (node->role == AccessibleRole::ListItem || node->role == AccessibleRole::Tab))
            *result = static_cast<ISelectionItemProvider*>(this);
        if (*result) AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property, VARIANT* result) override {
        if (!result) return E_POINTER;
        VariantInit(result);
        if (!alive()) return UIA_E_ELEMENTNOTAVAILABLE;
        const auto node = state_->node(id_);
        const auto stringProperty = [&](std::wstring_view value) { result->vt = VT_BSTR; return bstr(value, &result->bstrVal); };
        switch (property) {
        case UIA_NamePropertyId: return stringProperty(node ? node->name : L"Velin player");
        case UIA_AutomationIdPropertyId: return stringProperty(!id_ ? L"Velin.Player" : L"Velin.Control." + std::to_wstring(id_));
        case UIA_FrameworkIdPropertyId: return stringProperty(L"Velin.Win32");
        case UIA_HelpTextPropertyId: return stringProperty(node ? node->help : L"Media playback controls");
        case UIA_AcceleratorKeyPropertyId: return stringProperty(node ? node->shortcut : L"");
        case UIA_ControlTypePropertyId: result->vt = VT_I4; result->lVal = node ? controlType(node->role) : UIA_PaneControlTypeId; return S_OK;
        case UIA_IsControlElementPropertyId:
        case UIA_IsContentElementPropertyId: result->vt = VT_BOOL; result->boolVal = VARIANT_TRUE; return S_OK;
        case UIA_IsKeyboardFocusablePropertyId: result->vt = VT_BOOL; result->boolVal = !node || node->role != AccessibleRole::Text ? VARIANT_TRUE : VARIANT_FALSE; return S_OK;
        case UIA_HasKeyboardFocusPropertyId: result->vt = VT_BOOL; result->boolVal = node && node->focused ? VARIANT_TRUE : VARIANT_FALSE; return S_OK;
        case UIA_IsEnabledPropertyId: result->vt = VT_BOOL; result->boolVal = !node || node->enabled ? VARIANT_TRUE : VARIANT_FALSE; return S_OK;
        case UIA_IsOffscreenPropertyId: result->vt = VT_BOOL; result->boolVal = node && (IsRectEmpty(&node->bounds) || !IsWindowVisible(state_->target())) ? VARIANT_TRUE : VARIANT_FALSE; return S_OK;
        default: return S_OK;
        }
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (!alive()) return UIA_E_ELEMENTNOTAVAILABLE;
        return id_ ? S_OK : UiaHostProviderFromHwnd(state_->target(), result);
    }
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction, IRawElementProviderFragment** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (!alive()) return UIA_E_ELEMENTNOTAVAILABLE;
        const auto nodes = state_->snapshot();
        int target = -1;
        if (!id_) {
            if (!nodes.empty() && direction == NavigateDirection_FirstChild) target = nodes.front().id;
            if (!nodes.empty() && direction == NavigateDirection_LastChild) target = nodes.back().id;
        } else {
            if (direction == NavigateDirection_Parent) target = 0;
            auto current = std::find_if(nodes.begin(), nodes.end(), [&](const auto& n) { return n.id == id_; });
            if (current != nodes.end()) {
                if (direction == NavigateDirection_NextSibling && std::next(current) != nodes.end()) target = std::next(current)->id;
                if (direction == NavigateDirection_PreviousSibling && current != nodes.begin()) target = std::prev(current)->id;
            }
        }
        if (target >= 0) *result = static_cast<IRawElementProviderFragment*>(new ElementProvider(state_, target));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (!id_) return S_OK;
        LONG values[]{UiaAppendRuntimeId, id_};
        *result = SafeArrayCreateVector(VT_I4, 0, 2);
        if (!*result) return E_OUTOFMEMORY;
        for (LONG i = 0; i < 2; ++i) SafeArrayPutElement(*result, &i, &values[i]);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* result) override {
        if (!result) return E_POINTER;
        *result = {};
        if (!alive()) return UIA_E_ELEMENTNOTAVAILABLE;
        RECT rectangle{};
        const auto node = state_->node(id_);
        if (node) rectangle = node->bounds; else GetWindowRect(state_->target(), &rectangle);
        *result = {static_cast<double>(rectangle.left), static_cast<double>(rectangle.top),
            static_cast<double>(rectangle.right - rectangle.left), static_cast<double>(rectangle.bottom - rectangle.top)};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** result) override { if (!result) return E_POINTER; *result = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetFocus() override { return state_->enqueue(id_, 0, true); }
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** result) override {
        if (!result) return E_POINTER;
        *result = static_cast<IRawElementProviderFragmentRoot*>(new ElementProvider(state_)); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (!alive()) return UIA_E_ELEMENTNOTAVAILABLE;
        const auto nodes = state_->snapshot(); const POINT point{static_cast<LONG>(x), static_cast<LONG>(y)};
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) if (PtInRect(&it->bounds, point)) {
            *result = static_cast<IRawElementProviderFragment*>(new ElementProvider(state_, it->id)); return S_OK;
        }
        *result = static_cast<IRawElementProviderFragment*>(new ElementProvider(state_)); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (!alive()) return UIA_E_ELEMENTNOTAVAILABLE;
        for (const auto& node : state_->snapshot()) if (node.focused) {
            *result = static_cast<IRawElementProviderFragment*>(new ElementProvider(state_, node.id)); break;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Invoke() override {
        const auto node = state_->node(id_);
        if (!node) return UIA_E_ELEMENTNOTAVAILABLE;
        if (!node->enabled) return UIA_E_ELEMENTNOTENABLED;
        return state_->enqueue(id_, node->value);
    }
    HRESULT STDMETHODCALLTYPE SetValue(double value) override {
        const auto node = state_->node(id_);
        if (!node) return UIA_E_ELEMENTNOTAVAILABLE;
        if (!node->enabled) return UIA_E_ELEMENTNOTENABLED;
        if (!std::isfinite(value) || value < node->minimum || value > node->maximum) return E_INVALIDARG;
        return state_->enqueue(id_, value);
    }
    HRESULT STDMETHODCALLTYPE get_Value(double* result) override { return numeric(result, 0); }
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* result) override { if (!result) return E_POINTER; *result = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Maximum(double* result) override { return numeric(result, 2); }
    HRESULT STDMETHODCALLTYPE get_Minimum(double* result) override { return numeric(result, 1); }
    HRESULT STDMETHODCALLTYPE get_LargeChange(double* result) override { return numeric(result, 4); }
    HRESULT STDMETHODCALLTYPE get_SmallChange(double* result) override { return numeric(result, 3); }
    HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR value) override {
        if (!value) return E_INVALIDARG;
        wchar_t* end{}; const double number = wcstod(value, &end);
        return end == value || *end ? E_INVALIDARG : SetValue(number);
    }
    HRESULT STDMETHODCALLTYPE get_Value(BSTR* result) override {
        const auto node = state_->node(id_); return node ? bstr(std::to_wstring(node->value), result) : UIA_E_ELEMENTNOTAVAILABLE;
    }
    HRESULT STDMETHODCALLTYPE GetSelection(SAFEARRAY** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        std::vector<int> selected;
        for (const auto& node : state_->snapshot()) if (node.selected) selected.push_back(node.id);
        *result = SafeArrayCreateVector(VT_UNKNOWN, 0, static_cast<ULONG>(selected.size()));
        if (!*result) return E_OUTOFMEMORY;
        for (LONG i = 0; i < static_cast<LONG>(selected.size()); ++i) {
            auto* provider = static_cast<IRawElementProviderSimple*>(new ElementProvider(state_, selected[static_cast<size_t>(i)]));
            SafeArrayPutElement(*result, &i, provider); provider->Release();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_CanSelectMultiple(BOOL* result) override { if (!result) return E_POINTER; *result = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_IsSelectionRequired(BOOL* result) override { if (!result) return E_POINTER; *result = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE Select() override { return Invoke(); }
    HRESULT STDMETHODCALLTYPE AddToSelection() override { const auto node = state_->node(id_); return node && node->selected ? S_OK : Invoke(); }
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() override { return UIA_E_INVALIDOPERATION; }
    HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* result) override {
        if (!result) return E_POINTER;
        const auto node = state_->node(id_);
        if (!node) return UIA_E_ELEMENTNOTAVAILABLE;
        *result = node->selected; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple** result) override {
        if (!result) return E_POINTER;
        *result = static_cast<IRawElementProviderSimple*>(new ElementProvider(state_)); return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* result) override { if (!result) return E_POINTER; *result = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return DISP_E_UNKNOWNNAME; }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override { return DISP_E_MEMBERNOTFOUND; }
    HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (!alive()) return CO_E_OBJNOTCONNECTED;
        if (id_) { *result = static_cast<IAccessible*>(new ElementProvider(state_)); return S_OK; }
        return CreateStdAccessibleObject(state_->target(), OBJID_WINDOW, IID_IDispatch, reinterpret_cast<void**>(result));
    }
    HRESULT STDMETHODCALLTYPE get_accChildCount(long* result) override { if (!result) return E_POINTER; *result = id_ ? 0 : static_cast<long>(state_->snapshot().size()); return S_OK; }
    HRESULT STDMETHODCALLTYPE get_accChild(VARIANT child, IDispatch** result) override {
        if (!result) return E_POINTER;
        *result = nullptr; const int target = resolveChild(child);
        if (target <= 0) return E_INVALIDARG;
        *result = static_cast<IAccessible*>(new ElementProvider(state_, target)); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accName(VARIANT child, BSTR* result) override { return msaaString(child, result, 0); }
    HRESULT STDMETHODCALLTYPE get_accValue(VARIANT child, BSTR* result) override { return msaaString(child, result, 1); }
    HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT child, BSTR* result) override { return msaaString(child, result, 2); }
    HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT child, BSTR* result) override { return msaaString(child, result, 2); }
    HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT child, BSTR* result) override { return msaaString(child, result, 3); }
    HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT child, BSTR* result) override { return msaaString(child, result, 4); }
    HRESULT STDMETHODCALLTYPE get_accRole(VARIANT child, VARIANT* result) override {
        if (!result) return E_POINTER;
        VariantInit(result); const int target = resolveChild(child);
        if (target < 0) return E_INVALIDARG;
        const auto node = state_->node(target);
        result->vt = VT_I4; result->lVal = node ? msaaRole(node->role) : ROLE_SYSTEM_CLIENT; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accState(VARIANT child, VARIANT* result) override {
        if (!result) return E_POINTER;
        VariantInit(result); const int target = resolveChild(child);
        if (target < 0) return E_INVALIDARG;
        const auto node = state_->node(target);
        result->vt = VT_I4; result->lVal = STATE_SYSTEM_FOCUSABLE;
        if (node) { if (!node->enabled) result->lVal |= STATE_SYSTEM_UNAVAILABLE; if (node->focused) result->lVal |= STATE_SYSTEM_FOCUSED; if (node->selected) result->lVal |= STATE_SYSTEM_SELECTED; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR* file, VARIANT, long* topic) override { if (file) *file = nullptr; if (topic) *topic = 0; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT* result) override { return msaaSelection(result, true); }
    HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT* result) override { return msaaSelection(result, false); }
    HRESULT STDMETHODCALLTYPE accSelect(long flags, VARIANT child) override {
        const int target = resolveChild(child); if (target < 0) return E_INVALIDARG;
        if (flags & SELFLAG_TAKEFOCUS) state_->enqueue(target, 0, true);
        if (flags & (SELFLAG_TAKESELECTION | SELFLAG_ADDSELECTION)) return state_->enqueue(target, 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accLocation(long* x, long* y, long* width, long* height, VARIANT child) override {
        if (!x || !y || !width || !height) return E_POINTER;
        const int target = resolveChild(child);
        if (target < 0) return E_INVALIDARG;
        RECT rectangle{}; const auto node = state_->node(target);
        if (node) rectangle = node->bounds; else GetWindowRect(state_->target(), &rectangle);
        *x = rectangle.left; *y = rectangle.top; *width = rectangle.right - rectangle.left; *height = rectangle.bottom - rectangle.top;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accNavigate(long direction, VARIANT start, VARIANT* result) override {
        if (!result) return E_POINTER;
        VariantInit(result); const auto nodes = state_->snapshot();
        if (direction == NAVDIR_FIRSTCHILD && !id_ && !nodes.empty()) { result->vt = VT_I4; result->lVal = 1; return S_OK; }
        if (direction == NAVDIR_LASTCHILD && !id_ && !nodes.empty()) { result->vt = VT_I4; result->lVal = static_cast<LONG>(nodes.size()); return S_OK; }
        if (start.vt != VT_I4) return E_INVALIDARG;
        const long index = start.lVal + (direction == NAVDIR_NEXT ? 1 : direction == NAVDIR_PREVIOUS ? -1 : 0);
        if ((direction == NAVDIR_NEXT || direction == NAVDIR_PREVIOUS) && index > 0 && index <= static_cast<long>(nodes.size())) { result->vt = VT_I4; result->lVal = index; return S_OK; }
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE accHitTest(long x, long y, VARIANT* result) override {
        if (!result) return E_POINTER;
        VariantInit(result); result->vt = VT_I4; result->lVal = CHILDID_SELF;
        const auto nodes = state_->snapshot(); const POINT point{x, y};
        for (size_t index = nodes.size(); index > 0; --index) if (PtInRect(&nodes[index - 1].bounds, point)) { result->lVal = static_cast<LONG>(index); break; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT child) override {
        const int target = resolveChild(child); if (target <= 0) return E_INVALIDARG;
        const auto node = state_->node(target); if (!node || !node->enabled) return E_ACCESSDENIED;
        return state_->enqueue(target, node->value);
    }
    HRESULT STDMETHODCALLTYPE put_accName(VARIANT, BSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE put_accValue(VARIANT child, BSTR value) override {
        const int target = resolveChild(child); if (target <= 0 || !value) return E_INVALIDARG;
        auto* element = new ElementProvider(state_, target); const HRESULT status = element->SetValue(value); element->Release(); return status;
    }
private:
    bool alive() { return state_->target() && (!id_ || state_->node(id_).has_value()); }
    HRESULT numeric(double* result, int field) {
        if (!result) return E_POINTER;
        const auto node = state_->node(id_);
        if (!node) return UIA_E_ELEMENTNOTAVAILABLE;
        *result = field == 0 ? node->value : field == 1 ? node->minimum : field == 2 ? node->maximum : field == 3 ? node->step : node->step * 10;
        return S_OK;
    }
    int resolveChild(VARIANT child) {
        if (child.vt != VT_I4 || child.lVal < 0) return -1;
        if (child.lVal == CHILDID_SELF) return id_;
        const auto nodes = state_->snapshot();
        return !id_ && static_cast<size_t>(child.lVal) <= nodes.size() ? nodes[static_cast<size_t>(child.lVal - 1)].id : -1;
    }
    HRESULT msaaString(VARIANT child, BSTR* result, int field) {
        if (!result) return E_POINTER;
        *result = nullptr; const int target = resolveChild(child);
        if (target < 0) return E_INVALIDARG;
        const auto node = state_->node(target);
        if (!node) return field == 0 ? bstr(L"Velin player", result) : S_FALSE;
        if (field == 0) return bstr(node->name, result);
        if (field == 1) return node->role == AccessibleRole::Slider ? bstr(std::to_wstring(node->value), result) : S_FALSE;
        if (field == 2) return bstr(node->help, result);
        if (field == 3) return bstr(node->shortcut, result);
        return bstr(node->role == AccessibleRole::Slider ? L"Adjust" : node->role == AccessibleRole::ListItem || node->role == AccessibleRole::Tab ? L"Select" : L"Press", result);
    }
    HRESULT msaaSelection(VARIANT* result, bool focus) {
        if (!result) return E_POINTER;
        VariantInit(result); const auto nodes = state_->snapshot();
        for (size_t index = 0; index < nodes.size(); ++index) if (focus ? nodes[index].focused : nodes[index].selected) {
            result->vt = VT_I4; result->lVal = id_ ? CHILDID_SELF : static_cast<LONG>(index + 1); break;
        }
        return S_OK;
    }
    std::atomic<ULONG> references_{1};
    std::shared_ptr<AccessibleState> state_;
    int id_{};
};
} // namespace

struct AccessibilityProvider::Impl {
    std::shared_ptr<AccessibleState> state = std::make_shared<AccessibleState>();
    ElementProvider* root = nullptr;
    Action action;
    Focus focus;
};
AccessibilityProvider::AccessibilityProvider() : impl_(std::make_unique<Impl>()) {}
AccessibilityProvider::~AccessibilityProvider() { shutdown(); }
void AccessibilityProvider::initialize(HWND window, Action action, Focus focus) {
    shutdown();
    impl_->state = std::make_shared<AccessibleState>(); impl_->state->window = window;
    impl_->action = std::move(action); impl_->focus = std::move(focus);
    impl_->root = new ElementProvider(impl_->state);
}
void AccessibilityProvider::shutdown() {
    { std::lock_guard lock(impl_->state->mutex); impl_->state->window = nullptr; impl_->state->nodes.clear(); impl_->state->pending.clear(); }
    if (impl_->root) { UiaDisconnectProvider(static_cast<IRawElementProviderSimple*>(impl_->root)); impl_->root->Release(); impl_->root = nullptr; }
}
void AccessibilityProvider::update(std::vector<AccessibleNode> nodes) {
    const auto before = impl_->state->snapshot();
    const bool structureChanged = before.size() != nodes.size() || !std::equal(before.begin(), before.end(), nodes.begin(), nodes.end(),
        [](const auto& left, const auto& right) { return left.id == right.id; });
    { std::lock_guard lock(impl_->state->mutex); impl_->state->nodes = std::move(nodes); }
    const auto window = impl_->state->target();
    if (!window || !impl_->root) return;
    if (structureChanged) {
        NotifyWinEvent(EVENT_OBJECT_REORDER, window, OBJID_CLIENT, CHILDID_SELF);
        UiaRaiseStructureChangedEvent(static_cast<IRawElementProviderSimple*>(impl_->root), StructureChangeType_ChildrenInvalidated, nullptr, 0);
    }
}
std::optional<LRESULT> AccessibilityProvider::handleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    if (!impl_->root) return std::nullopt;
    const auto window = impl_->state->target();
    if (message == WM_GETOBJECT) {
        if (static_cast<LONG>(lparam) == UiaRootObjectId)
            return UiaReturnRawElementProvider(window, wparam, lparam, static_cast<IRawElementProviderSimple*>(impl_->root));
        if (static_cast<LONG>(lparam) == OBJID_CLIENT)
            return LresultFromObject(IID_IAccessible, wparam, static_cast<IAccessible*>(impl_->root));
    }
    if (message != accessibilityMessage) return std::nullopt;
    std::deque<AccessibleAction> actions;
    { std::lock_guard lock(impl_->state->mutex); actions.swap(impl_->state->pending); }
    for (const auto& action : actions) {
        const auto node = impl_->state->node(action.id);
        if (!node || !node->enabled) continue;
        if (action.focus) { ::SetFocus(window); if (impl_->focus) impl_->focus(action.id); notifyFocus(action.id); }
        else if (impl_->action) impl_->action(action.id, action.value);
    }
    return 0;
}
void AccessibilityProvider::notifyFocus(int id) {
    const auto window = impl_->state->target(); if (!window) return;
    auto nodes = impl_->state->snapshot();
    for (size_t index = 0; index < nodes.size(); ++index) {
        nodes[index].focused = nodes[index].id == id;
        if (nodes[index].focused) NotifyWinEvent(EVENT_OBJECT_FOCUS, window, OBJID_CLIENT, static_cast<LONG>(index + 1));
    }
    { std::lock_guard lock(impl_->state->mutex); impl_->state->nodes = std::move(nodes); }
    auto* provider = new ElementProvider(impl_->state, id);
    UiaRaiseAutomationEvent(static_cast<IRawElementProviderSimple*>(provider), UIA_AutomationFocusChangedEventId);
    provider->Release();
}
} // namespace velin
