// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "velin/core.hpp"
#include "velin/library.hpp"
#include <functional>
#include <windows.h>

namespace velin {

// Apply and OK validate every page and atomically save before invoking onApply.
// Returns true when at least one Apply/OK operation succeeded.
bool showPreferencesDialog(HWND parent, Preferences& preferences, SettingsStore& store,
    std::function<void(const Preferences&)> onApply = {}, std::string initialSection = {});

// Native searchable history view. Opening an entry returns through onOpen.
void showHistoryDialog(HWND parent, HistoryStore& history,
    std::function<void(const HistoryEntry&)> onOpen,
    Preferences* preferences = nullptr, SettingsStore* settings = nullptr);

}  // namespace velin
