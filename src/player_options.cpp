// SPDX-License-Identifier: GPL-3.0-or-later
#include "velin/player_options.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace velin {
namespace {
std::string number(double value) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(12) << value;
    return stream.str();
}
std::string utf8(const std::filesystem::path& path) {
    const auto value=path.u8string();
    return {reinterpret_cast<const char*>(value.data()),value.size()};
}
const char* flag(bool value) { return value?"yes":"no"; }
}

std::string audioFilterChain(const Preferences& preferences) {
    constexpr double frequencies[]{31.25,62.5,125,250,500,1000,2000,4000,8000,16000};
    std::string graph;
    for (size_t i=0;i<preferences.audioEqualizer.size();++i) {
        const auto gain=preferences.audioEqualizer[i];
        if (!std::isfinite(gain) || std::abs(gain)<.00001) continue;
        if (!graph.empty()) graph+=',';
        graph+="equalizer=f="+number(frequencies[i])+":t=o:w=1:g="+number(std::clamp(gain,-24.,24.));
    }
    if (preferences.audioNormalization) {
        if (!graph.empty()) graph+=',';
        graph+="dynaudnorm=f=500:g=31:p=0.95";
    }
    std::string result;
    if (!graph.empty()) result="@velin-equalizer:lavfi=["+graph+"]";
    if (auto iterator=preferences.mpvOptions.find("af");iterator!=preferences.mpvOptions.end() && !iterator->second.empty()) {
        if (!result.empty()) result+=',';
        result+=iterator->second;
    }
    return result;
}

bool isInitializationOption(std::string_view name) {
    constexpr std::string_view names[]{"vo","ao","gpu-api","gpu-context","config","config-dir","input-conf",
        "load-scripts","scripts","script","script-opts","ytdl","ytdl-raw-options","ytdl-format",
        "http-proxy","user-agent","network-timeout","demuxer-max-bytes","demuxer-readahead-secs"};
    return std::find(std::begin(names),std::end(names),name)!=std::end(names);
}

std::map<std::string,std::string> playbackOptions(const Preferences& input,const std::filesystem::path& directory) {
    const auto p=normalizePreferences(input);
    std::map<std::string,std::string> result{
        {"volume",number(p.volume)},{"mute",flag(p.muted)},{"speed",number(p.speed)},
        {"resume-playback",flag(p.rememberPosition)},{"save-position-on-quit",flag(p.rememberPosition)},
        {"keep-open",!p.autoPlayNext?"always":p.pauseAtEnd?"yes":"no"},{"pause",flag(p.pauseOnOpen)},
        {"stop-screensaver",flag(p.preventSleep)},
        {"screenshot-format",p.screenshotFormat},{"screenshot-template",p.screenshotTemplate},
        {"screenshot-directory",p.screenshotDirectory.empty()?utf8(directory/L"Screenshots"):p.screenshotDirectory},
        {"hwdec",p.hardwareDecoding},{"target-colorspace-hint",p.hdrEnabled?"auto":"no"},
        {"icc-profile-auto",flag(p.iccProfileAuto)},{"scale",p.videoScaler},{"dscale",p.downScaler},
        {"interpolation",p.interpolation},{"deinterlace",flag(p.deinterlace)},{"deband",flag(p.deband)},
        {"vo",p.videoOutput},{"audio-device",p.audioDevice},{"audio-spdif",p.audioPassthrough},
        {"replaygain",p.replayGain},{"audio-delay",number(p.audioDelay)},
        {"sub-font",p.subtitleFont},{"sub-font-size",number(p.subtitleSize)},
        {"sub-color",p.subtitleColor},{"sub-border-size",number(p.subtitleBorderSize)},
        {"sub-border-color",p.subtitleBorderColor},{"sub-shadow-offset",number(p.subtitleShadowSize)},
        {"sub-shadow-color",p.subtitleShadowColor},{"sub-pos",number(p.subtitlePosition)},
        {"sub-scale",number(p.subtitleScale)},{"sub-delay",number(p.subtitleDelay)},
        {"sub-visibility",flag(p.subtitleVisible)},{"sub-auto",p.subtitleAutoLoad},
        {"sub-ass-override",p.subtitleOverrideAss?"force":"yes"},
        {"secondary-sid",p.secondarySubtitle},{"secondary-sub-delay",number(p.secondarySubtitleDelay)},
        {"secondary-sub-pos",number(p.secondarySubtitlePosition)},
        {"cache",p.networkCacheSeconds>0?"yes":"no"},{"cache-secs",std::to_string(p.networkCacheSeconds)},
        {"demuxer-max-bytes",std::to_string(static_cast<long long>(p.networkCacheMegabytes)*1024*1024)},
        {"network-timeout",std::to_string(p.networkTimeoutSeconds)},
        {"ytdl",flag(p.youtubeDlEnabled)},{"ytdl-format",p.youtubeDlFormat},
        {"config",flag(p.loadMpvConfig)},{"load-scripts",flag(p.scriptsEnabled)}
    };
    if (!p.iccProfile.empty()) result["icc-profile"]=p.iccProfile;
    if (!p.audioLanguages.empty()) result["alang"]=p.audioLanguages;
    if (!p.subtitleLanguages.empty()) result["slang"]=p.subtitleLanguages;
    if (!p.subtitleSearchPaths.empty()) result["sub-file-paths"]=p.subtitleSearchPaths;
    if (!p.networkProxy.empty()) result["http-proxy"]=p.networkProxy;
    if (!p.networkUserAgent.empty()) result["user-agent"]=p.networkUserAgent;
    if (!p.mpvConfigDirectory.empty()) result["config-dir"]=p.mpvConfigDirectory;
    if (p.scriptsEnabled && !p.scriptsDirectory.empty()) result["scripts"]=p.scriptsDirectory;
    if (p.youtubeDlEnabled && !p.youtubeDlPath.empty()) result["script-opts"]="ytdl_hook-ytdl_path="+p.youtubeDlPath;
    for (const auto& [key,value]:p.mpvOptions) {
        if (key=="af" || key=="wid" || key=="input-vo-keyboard" || key=="terminal" || key=="osc") continue;
        result[key]=value;
    }
    std::istringstream extra(p.extraMpvOptions);
    std::string row;
    while (std::getline(extra,row)) {
        auto begin=row.find_first_not_of(" \t\r");
        if (begin==std::string::npos || row[begin]=='#') continue;
        if (row.compare(begin,2,"--")==0) begin+=2;
        const auto equal=row.find('=',begin);
        if (equal==std::string::npos) continue;
        auto key=row.substr(begin,equal-begin);
        key.erase(key.find_last_not_of(" \t")+1);
        auto value=row.substr(equal+1);
        if (!value.empty() && value.back()=='\r') value.pop_back();
        if (key.empty() || key=="wid" || key=="input-vo-keyboard" || key=="terminal" || key=="osc") continue;
        result[key]=value;
    }
    result["af"]=audioFilterChain(p);
    return result;
}
} // namespace velin
