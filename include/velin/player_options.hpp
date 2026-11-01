// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "velin/core.hpp"
#include <filesystem>
#include <map>
#include <string>

namespace velin {
std::map<std::string,std::string> playbackOptions(const Preferences& preferences,
    const std::filesystem::path& dataDirectory);
std::string audioFilterChain(const Preferences& preferences);
bool isInitializationOption(std::string_view name);
} // namespace velin
