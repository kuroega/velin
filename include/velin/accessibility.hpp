// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace velin {
enum class AccessibleRole { Button, Slider, List, ListItem, Tab, Text };
struct AccessibleNode {
    int id = 0;
    AccessibleRole role = AccessibleRole::Button;
    std::wstring name, help, shortcut;
    RECT bounds{}; // Physical screen coordinates, not client DIPs.
    bool enabled = true, selected = false, focused = false;
    double value = 0, minimum = 0, maximum = 100, step = 1;
};

// Supplies both MSAA and UI Automation fragment/pattern providers. Call update
// after layout/state changes. Call handleMessage before the regular WndProc.
class AccessibilityProvider final {
public:
    using Action = std::function<void(int id, double value)>;
    using Focus = std::function<void(int id)>;
    AccessibilityProvider();
    ~AccessibilityProvider();
    void initialize(HWND window, Action action, Focus focus = {});
    void update(std::vector<AccessibleNode> nodes);
    std::optional<LRESULT> handleMessage(UINT message, WPARAM wparam, LPARAM lparam);
    void notifyFocus(int id);
    void shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace velin
