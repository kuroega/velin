// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace velin {
// WebView2 does not expose WebSocket handshakes through its resource filter.
// Until those connections can be restricted, only an unrestricted grant is safe.
inline bool pluginWebViewHasEnforceableNetworkPolicy(const std::vector<std::string>& domains) {
    return std::find(domains.begin(),domains.end(),"*")!=domains.end();
}
inline bool pluginWebViewAllowsUrl(const std::string& uri,const std::vector<std::string>& domains) {
    if (uri=="about:blank" || uri.starts_with("data:") || uri.starts_with("blob:https://velin.plugin/")) return true;
    if (uri.starts_with("https://velin.plugin/")) return true;
    if (!(uri.starts_with("https://") || uri.starts_with("http://"))) return false;
    const auto start=uri.find("://")+3;
    const auto end=uri.find_first_of("/?#",start);
    const auto authority=uri.substr(start,end-start);
    if (authority.find('@')!=std::string::npos) return false;
    auto host=authority.substr(0,authority.find(':'));
    if (host.empty()) return false;
    std::transform(host.begin(),host.end(),host.begin(),[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::any_of(domains.begin(),domains.end(),[&](std::string domain) {
        std::transform(domain.begin(),domain.end(),domain.begin(),[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (domain=="*" || host==domain) return true;
        if (!domain.starts_with("*.")) return false;
        const auto suffix=std::string_view(domain).substr(1);
        return host.size()>suffix.size() && host.ends_with(suffix);
    });
}
} // namespace velin
