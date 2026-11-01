// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "velin/extensions.hpp"
#include <windows.h>
#include <filesystem>
#include <functional>
#include <memory>

namespace velin {
class PluginWebView final {
public:
    using Message=std::function<void(const std::string&,const Json&)>;
    PluginWebView(HWND owner,std::string kind,std::filesystem::path pluginDirectory,
        std::filesystem::path dataDirectory,std::vector<std::string> allowedDomains,Message message);
    ~PluginWebView();
    Json operate(const std::string& operation,const Json& arguments);
    void setBounds(RECT physicalScreenBounds);
    bool visible() const;
    std::string error() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace velin
