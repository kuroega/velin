// SPDX-License-Identifier: GPL-3.0-or-later
#include "velin/core.hpp"
#include "velin/library.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>
#include <functional>
#include <type_traits>
#include <bit>
#include <set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace velin {

std::optional<std::string> centeredCropGeometry(std::int64_t width, std::int64_t height, std::string_view aspect) {
  if (width < 2 || height < 2 || width > 1000000 || height > 1000000) return std::nullopt;
  std::istringstream input{std::string(aspect)};
  double numerator = 0, denominator = 1;
  char separator = 0;
  if (!(input >> numerator)) return std::nullopt;
  input >> std::ws;
  if (input.peek() == ':') {
    input >> separator >> denominator;
    if (!input) return std::nullopt;
    input >> std::ws;
  }
  if (!input.eof() || !std::isfinite(numerator) || !std::isfinite(denominator) ||
      numerator <= 0 || denominator <= 0) return std::nullopt;
  const double ratio = numerator / denominator;
  if (!std::isfinite(ratio) || ratio <= 0) return std::nullopt;
  std::int64_t cropWidth = width, cropHeight = height;
  if (static_cast<double>(width) / height > ratio) cropWidth = static_cast<std::int64_t>(height * ratio);
  else cropHeight = static_cast<std::int64_t>(width / ratio);
  cropWidth -= cropWidth % 2;
  cropHeight -= cropHeight % 2;
  if (cropWidth < 2 || cropHeight < 2) return std::nullopt;
  const auto x = ((width - cropWidth) / 2) & ~std::int64_t{1};
  const auto y = ((height - cropHeight) / 2) & ~std::int64_t{1};
  return std::to_string(cropWidth) + "x" + std::to_string(cropHeight) + "+" +
         std::to_string(x) + "+" + std::to_string(y);
}

namespace {

constexpr std::size_t maxRecentFiles = 12;
constexpr std::uintmax_t maxSettingsBytes = 8 * 1024 * 1024;

bool isUrl(std::string_view path) { return path.find("://") != std::string_view::npos; }

char asciiLower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string decodeEscapes(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      const auto high = hexValue(text[i + 1]);
      const auto low = hexValue(text[i + 2]);
      if (high >= 0 && low >= 0) {
        const auto byte = static_cast<char>((high << 4) | low);
        // Embedded NUL is never a valid media path or settings value.
        if (byte != '\0') result.push_back(byte);
        i += 2;
        continue;
      }
    }
    result.push_back(text[i]);
  }
  return result;
}

std::string encodeEscapes(std::string_view text) {
  constexpr char digits[] = "0123456789ABCDEF";
  std::string result;
  result.reserve(text.size());
  for (const auto value : text) {
    const auto byte = static_cast<unsigned char>(value);
    if (byte < 0x20 || byte == '%' || byte == 0x7f) {
      result.push_back('%');
      result.push_back(digits[byte >> 4]);
      result.push_back(digits[byte & 0xf]);
    } else {
      result.push_back(value);
    }
  }
  return result;
}

std::string extension(std::string_view path) {
  if (isUrl(path)) path = path.substr(0, path.find_first_of("?#"));
  const auto slash = path.find_last_of("/\\");
  const auto dot = path.find_last_of('.');
  if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return {};
  std::string result(path.substr(dot));
  std::transform(result.begin(), result.end(), result.begin(), asciiLower);
  return result;
}

std::filesystem::path pathFromUtf8(std::string_view text) {
  return std::filesystem::path(std::u8string(
      reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::string pathToUtf8(const std::filesystem::path& path) {
  const auto result = path.u8string();
  return {reinterpret_cast<const char*>(result.data()), result.size()};
}

bool samePath(std::string_view left, std::string_view right) {
  if (isUrl(left) || isUrl(right)) return left == right;
#ifdef _WIN32
  const auto wideLeft = fromUtf8(left);
  const auto wideRight = fromUtf8(right);
  if (wideLeft.empty() || wideRight.empty()) return left == right;
  return CompareStringOrdinal(wideLeft.data(), static_cast<int>(wideLeft.size()),
                              wideRight.data(), static_cast<int>(wideRight.size()), TRUE) == CSTR_EQUAL;
#else
  return left == right;
#endif
}

template <typename Number>
bool parseNumber(std::string_view text, Number& value) {
  Number parsed{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return false;
  value = parsed;
  return true;
}

bool parseBool(std::string_view text, bool& value) {
  if (text == "1" || text == "true") {
    value = true;
    return true;
  }
  if (text == "0" || text == "false") {
    value = false;
    return true;
  }
  return false;
}

std::vector<std::string> split(std::string_view text, char separator) {
  std::vector<std::string> result;
  std::size_t start = 0;
  do {
    const auto end = text.find(separator, start);
    result.emplace_back(text.substr(start, end == std::string_view::npos ? text.size() - start : end - start));
    if (end == std::string_view::npos) break;
    start = end + 1;
  } while (start <= text.size());
  return result;
}

std::string trim(std::string_view text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string_view::npos) return {};
  return std::string(text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1));
}

struct Binding {
  PreferenceField field;
  std::function<std::string(const Preferences&)> get;
  std::function<bool(Preferences&, std::string_view, bool)> set;
  std::function<void(Preferences&)> normalize;
};

template<class T>
Binding makeBinding(const char* key, const char* label, const char* section, T Preferences::*member,
             PreferenceKind kind, double minimum = 0, double maximum = 0, const char* choices = "") {
  Binding result;
  result.field = {key, label, section, kind, minimum, maximum, choices};
  const auto options = split(choices, '|');
  result.get = [member, options](const Preferences& preferences) {
    const auto& value = preferences.*member;
    if constexpr (std::is_same_v<T, std::string>) return value;
    else if constexpr (std::is_same_v<T, bool>) return std::string(value ? "1" : "0");
    else if constexpr (std::is_enum_v<T>) {
      const auto index = static_cast<std::size_t>(value);
      return index < options.size() ? options[index] : options.front();
    } else {
      std::ostringstream stream; stream.imbue(std::locale::classic());
      stream << std::setprecision(17) << value;
      return stream.str();
    }
  };
  result.set = [member, options, kind, minimum, maximum](Preferences& preferences, std::string_view text, bool strict) {
    if constexpr (std::is_same_v<T, std::string>) {
      if (text.size() > 131072 || text.find('\0') != std::string_view::npos || (!text.empty() && fromUtf8(text).empty())) return false;
      if (kind == PreferenceKind::Choice && std::find(options.begin(), options.end(), text) == options.end()) return false;
      preferences.*member = text; return true;
    } else if constexpr (std::is_same_v<T, bool>) return parseBool(text, preferences.*member);
    else if constexpr (std::is_enum_v<T>) {
      const auto it = std::find(options.begin(), options.end(), text);
      if (it == options.end()) return false;
      preferences.*member = static_cast<T>(it - options.begin()); return true;
    } else {
      T value{};
      if (!parseNumber(text, value)) return false;
      if constexpr (std::is_floating_point_v<T>) { if (strict && !std::isfinite(value)) return false; }
      if (strict && (value < minimum || value > maximum)) return false;
      preferences.*member = value; return true;
    }
  };
  result.normalize = [member, minimum, maximum, options, kind](Preferences& preferences) {
    auto& value = preferences.*member;
    if constexpr (std::is_same_v<T, std::string>) {
      if (value.size() > 131072 || value.find('\0') != std::string::npos || (!value.empty() && fromUtf8(value).empty()) ||
          (kind == PreferenceKind::Choice && std::find(options.begin(), options.end(), value) == options.end())) value = Preferences{}.*member;
    } else if constexpr (std::is_same_v<T, bool>) { /* Boolean values are already normalized. */ }
    else if constexpr (std::is_enum_v<T>) { if (static_cast<std::size_t>(value) >= options.size()) value = Preferences{}.*member; }
    else {
      if constexpr (std::is_floating_point_v<T>) if (!std::isfinite(value)) value = Preferences{}.*member;
      value = std::clamp(value, static_cast<T>(minimum), static_cast<T>(maximum));
    }
  };
  return result;
}

bool supportedShortcut(std::string primary) {
  if (primary.starts_with("Ctrl+")) primary.erase(0,5);
  if (primary.starts_with("Shift+")) primary.erase(0,6);
  const std::string singles="ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,[];/'\\`-";
  const std::vector<std::string> named{"Space","Enter","Escape","Tab","Backspace","Delete","Home","End","Left","Right","Up","Down","PageUp","PageDown"};
  if (primary.size()==1 && singles.find(primary.front())!=std::string::npos) return true;
  if (std::find(named.begin(),named.end(),primary)!=named.end()) return true;
  if (primary.size()>=2 && primary.size()<=3 && primary.front()=='F') {
    int number=0;
    return parseNumber(std::string_view(primary).substr(1),number) && number>=1 && number<=24 && primary=="F"+std::to_string(number);
  }
  return false;
}

const std::vector<Binding>& bindings() {
  using K = PreferenceKind;
  static const std::vector<Binding> value = [] {
    std::vector<Binding> list;
#define BOOL(field, label, section) list.push_back(makeBinding(#field, label, section, &Preferences::field, K::Boolean))
#define NUM(field, label, section, low, high) list.push_back(makeBinding(#field, label, section, &Preferences::field, K::Number, low, high))
#define INT(field, label, section, low, high) list.push_back(makeBinding(#field, label, section, &Preferences::field, K::Integer, low, high))
#define PREF_TEXT(field, label, section) list.push_back(makeBinding(#field, label, section, &Preferences::field, K::Text))
#define CHOICE(field, label, section, options) list.push_back(makeBinding(#field, label, section, &Preferences::field, K::Choice, 0, 0, options))
    CHOICE(launchAction, "At launch", "General", "welcome|restore-session|open-files");
    BOOL(restorePlaylist, "Restore the last playlist", "General");
    BOOL(rememberPosition, "Remember playback position", "General");
    BOOL(recordHistory, "Record playback history", "General");
    BOOL(recordRecentFiles, "Record recent files", "General");
    BOOL(pauseOnOpen, "Pause when opening media", "General");
    BOOL(pauseWhenMinimized, "Pause when minimized", "General");
    BOOL(pauseWhenInactive, "Pause when another application is active", "General");
    BOOL(pauseAtEnd, "Keep the player open at the end", "General");
    BOOL(pauseOnSleep, "Pause before system sleep", "General");
    BOOL(autoPlayNext, "Play the next playlist item automatically", "General");
    BOOL(autoAddRelatedFiles, "Add related files from the same folder", "General");
    BOOL(preventSleep, "Prevent sleep during video playback", "General");
    BOOL(fullscreenOnOpen, "Enter fullscreen when opening media", "General");
    CHOICE(screenshotFormat, "Screenshot format", "General", "png|jpg|jpeg|webp|jxl|avif");
    PREF_TEXT(screenshotDirectory, "Screenshot directory (empty uses application data)", "General");
    PREF_TEXT(screenshotTemplate, "Screenshot filename template", "General");
    BOOL(screenshotIncludeSubtitles, "Include subtitles in screenshots", "General");
    BOOL(screenshotCopyToClipboard, "Copy screenshots to the clipboard", "General");
    CHOICE(theme, "Theme", "Interface", "system|dark|light");
    BOOL(alwaysOnTop, "Always on top", "Interface");
    BOOL(lockWindowAspect, "Lock the video viewport aspect ratio while resizing", "Interface");
    BOOL(showPlaylist, "Show the playlist sidebar", "Interface");
    BOOL(musicMode, "Use the standalone music layout", "Interface");
    BOOL(autoMusicMode, "Switch to music layout for audio-only media", "Interface");
    BOOL(pipAlwaysOnTop, "Keep picture in picture on top", "Interface");
    BOOL(pipPauseOnClose, "Pause when picture in picture closes", "Interface");
    BOOL(showChapterMarkers, "Show chapter markers on the timeline", "Interface");
    BOOL(showTimelineThumbnails, "Show video thumbnails when seeking", "Interface");
    BOOL(fullscreenAutoHide, "Hide controls automatically during playback", "Interface");
    NUM(controlsHideSeconds, "Control hide delay (seconds)", "Interface", .25, 60);
    NUM(seekSeconds, "Arrow-key seek amount (seconds)", "Interface", .1, 600);
    NUM(volumeStep, "Volume step", "Interface", 1, 50);
    INT(sidebarWidth, "Sidebar width (DIP)", "Interface", 180, 600);
    INT(timelineThumbnailWidth, "Timeline thumbnail width (pixels)", "Interface", 100, 640);
    BOOL(rememberWindowPosition, "Remember window position", "Interface");
    INT(windowWidth, "Window width (DIP)", "Interface", 640, 7680);
    INT(windowHeight, "Window height (DIP)", "Interface", 420, 4320);
    INT(windowX, "Saved window X (-1 uses Windows placement)", "Interface", -32768, 32767);
    INT(windowY, "Saved window Y (-1 uses Windows placement)", "Interface", -32768, 32767);
    INT(historyWidth, "History window width (DIP)", "Interface", 640, 3840);
    INT(historyHeight, "History window height (DIP)", "Interface", 420, 2160);
    CHOICE(hardwareDecoding, "Hardware decoding", "Video", "auto|auto-safe|auto-copy|no|d3d11va|d3d11va-copy|dxva2|dxva2-copy|nvdec|nvdec-copy|vulkan");
    BOOL(hdrEnabled, "Enable HDR output", "Video");
    BOOL(iccProfileAuto, "Use the display ICC profile", "Video");
    PREF_TEXT(iccProfile, "Explicit ICC profile path", "Video");
    CHOICE(videoScaler, "Upscaling filter", "Video", "spline36|bilinear|bicubic|lanczos|ewa_lanczossharp|ewa_lanczos|oversample");
    CHOICE(downScaler, "Downscaling filter", "Video", "mitchell|bilinear|bicubic|lanczos|spline36|ewa_lanczossharp");
    CHOICE(interpolation, "Video interpolation", "Video", "no|yes");
    BOOL(deinterlace, "Deinterlace", "Video");
    BOOL(deband, "Reduce color banding", "Video");
    PREF_TEXT(videoOutput, "mpv video outputs (comma separated)", "Video");
    NUM(volume, "Volume", "Audio", 0, 100);
    BOOL(muted, "Mute audio", "Audio");
    NUM(speed, "Playback speed", "Audio", .25, 4);
    PREF_TEXT(audioDevice, "Audio device (auto or mpv device identifier)", "Audio");
    PREF_TEXT(audioPassthrough, "Passthrough codecs (ac3,dts,dts-hd,eac3,truehd)", "Audio");
    BOOL(audioNormalization, "Normalize audio loudness", "Audio");
    CHOICE(replayGain, "ReplayGain", "Audio", "no|track|album");
    PREF_TEXT(audioLanguages, "Preferred audio languages (comma separated)", "Audio");
    NUM(audioDelay, "Audio delay (seconds)", "Audio", -600, 600);
    PREF_TEXT(subtitleFont, "Subtitle font", "Subtitles");
    NUM(subtitleSize, "Subtitle font size", "Subtitles", 8, 200);
    PREF_TEXT(subtitleColor, "Text color (#AARRGGBB)", "Subtitles");
    NUM(subtitleBorderSize, "Border width", "Subtitles", 0, 20);
    PREF_TEXT(subtitleBorderColor, "Border color (#AARRGGBB)", "Subtitles");
    NUM(subtitleShadowSize, "Shadow offset", "Subtitles", 0, 20);
    PREF_TEXT(subtitleShadowColor, "Shadow color (#AARRGGBB)", "Subtitles");
    NUM(subtitlePosition, "Vertical position (percent)", "Subtitles", 0, 150);
    NUM(subtitleScale, "Subtitle scale", "Subtitles", .1, 10);
    PREF_TEXT(subtitleLanguages, "Preferred subtitle languages", "Subtitles");
    PREF_TEXT(subtitleSearchPaths, "Subtitle search paths (mpv path-list syntax)", "Subtitles");
    NUM(subtitleDelay, "Subtitle delay (seconds)", "Subtitles", -600, 600);
    BOOL(subtitleVisible, "Show subtitles", "Subtitles");
    CHOICE(subtitleAutoLoad, "Automatic subtitle matching", "Subtitles", "no|exact|fuzzy|all");
    BOOL(subtitleOverrideAss, "Override ASS subtitle styling", "Subtitles");
    PREF_TEXT(secondarySubtitle, "Secondary subtitle track (no, auto, or track ID)", "Subtitles");
    NUM(secondarySubtitleDelay, "Secondary subtitle delay (seconds)", "Subtitles", -600, 600);
    NUM(secondarySubtitlePosition, "Secondary subtitle vertical position", "Subtitles", 0, 150);
    CHOICE(subtitleProvider, "Online subtitle provider", "Subtitles", "opensubtitles|assrt");
    PREF_TEXT(subtitleProviderApiKey, "Subtitle provider API key", "Subtitles");
    PREF_TEXT(subtitleProviderToken, "Subtitle provider access token", "Subtitles");
    PREF_TEXT(networkProxy, "HTTP proxy URL", "Network");
    INT(networkCacheSeconds, "Read-ahead cache (seconds)", "Network", 0, 3600);
    INT(networkCacheMegabytes, "Maximum cache (MiB)", "Network", 1, 32768);
    INT(networkTimeoutSeconds, "Network timeout (seconds)", "Network", 1, 600);
    PREF_TEXT(networkUserAgent, "HTTP User-Agent (empty uses mpv default)", "Network");
    PREF_TEXT(updateManifestUrl, "HTTPS release manifest URL", "Network");
    BOOL(checkForUpdates, "Check for updates when a release URL is configured", "Network");
    BOOL(youtubeDlEnabled, "Enable yt-dlp media extraction", "Network");
    PREF_TEXT(youtubeDlPath, "yt-dlp executable path", "Network");
    PREF_TEXT(youtubeDlFormat, "yt-dlp format selector", "Network");
    BOOL(loadMpvConfig, "Load a custom mpv configuration", "Advanced");
    PREF_TEXT(mpvConfigDirectory, "mpv configuration directory", "Advanced");
    BOOL(scriptsEnabled, "Load user mpv scripts", "Advanced");
    PREF_TEXT(scriptsDirectory, "User scripts directory", "Advanced");
    BOOL(pluginsEnabled, "Enable Velin JavaScript plugins", "Advanced");
    PREF_TEXT(pluginsDirectory, "Velin plugins directory", "Advanced");
    list.push_back(makeBinding("extraMpvOptions", "Additional mpv options (name=value, one per line)", "Advanced", &Preferences::extraMpvOptions, K::Multiline));
#undef BOOL
#undef NUM
#undef INT
#undef PREF_TEXT
#undef CHOICE
    Binding equalizer;
    equalizer.field = {"audioEqualizer", "Equalizer: 31,63,125,250,500,1k,2k,4k,8k,16k Hz (dB)", "Audio", K::Text, -24, 24, ""};
    equalizer.get = [](const Preferences& preferences) {
      std::ostringstream out; out.imbue(std::locale::classic()); out << std::setprecision(17);
      for (std::size_t i=0; i<10; ++i) { if (i) out << ','; out << preferences.audioEqualizer[i]; }
      return out.str();
    };
    equalizer.set = [](Preferences& preferences, std::string_view text, bool strict) {
      const auto parts = split(text, ','); if (parts.size()!=10) return false;
      std::array<double,10> gains{};
      for (std::size_t i=0;i<10;++i) if (!parseNumber(trim(parts[i]),gains[i]) || !std::isfinite(gains[i]) || (strict && (gains[i]<-24 || gains[i]>24))) return false;
      preferences.audioEqualizer=gains; return true;
    };
    equalizer.normalize = [](Preferences& preferences) { for (auto& gain:preferences.audioEqualizer) gain=std::isfinite(gain)?std::clamp(gain,-24.,24.):0.; };
    list.push_back(std::move(equalizer));
    for (bool keys : {true,false}) {
      const auto member = keys ? &Preferences::keyBindings : &Preferences::mpvOptions;
      Binding mapping;
      mapping.field = {keys?"keyBindings":"mpvOptions", keys?"Custom keys: Ctrl+O=open-files or Space=cycle pause":"mpv option overrides (name=value, one per line)", keys?"Keys":"Advanced", K::Multiline,0,0,""};
      mapping.get = [member](const Preferences& preferences) {
        std::string out;
        for (const auto& [key,value] : preferences.*member) { if (!out.empty()) out+='\n'; out+=key+'='+value; }
        return out;
      };
      mapping.set = [member,keys](Preferences& preferences,std::string_view text,bool strict) {
        std::map<std::string,std::string> output;
        for (const auto& raw:split(text,'\n')) {
          const auto line=trim(raw); if (line.empty() || line.front()=='#') continue;
          const auto separator=line.find('='); if (separator==std::string::npos) return false;
          auto key=trim(std::string_view(line).substr(0,separator)); const auto value=trim(std::string_view(line).substr(separator+1));
          if (keys) {
            key=normalizeKeyBinding(key);
            if (strict) {
              // The native player dispatches keyboard events, not mouse, Win, or
              // system-key combinations. Do not silently accept unusable bindings.
              if (!supportedShortcut(key)) return false;
            }
          }
          else if (key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_/.")!=std::string::npos) return false;
          if (key.empty() || key.size()>128 || value.empty() || value.size()>32768 || value.find('\0')!=std::string::npos || fromUtf8(value).empty() || output.contains(key)) return false;
          output.emplace(std::move(key),value);
        }
        preferences.*member=std::move(output); return true;
      };
      mapping.normalize = [member](Preferences& preferences) {
        std::erase_if(preferences.*member,[](const auto& item) { return item.first.empty() || item.second.empty() || item.first.find_first_of("\r\n\0",0,3)!=std::string::npos || item.second.find_first_of("\r\n\0",0,3)!=std::string::npos; });
      };
      list.push_back(std::move(mapping));
    }
    for (const auto& option : std::vector<PreferenceField>{
        {"mpv.brightness","Brightness","Video adjustments",K::Number,-100,100,""},
        {"mpv.contrast","Contrast","Video adjustments",K::Number,-100,100,""},
        {"mpv.gamma","Gamma","Video adjustments",K::Number,-100,100,""},
        {"mpv.saturation","Saturation","Video adjustments",K::Number,-100,100,""},
        {"mpv.hue","Hue","Video adjustments",K::Number,-100,100,""},
        {"mpv.video-zoom","Zoom (log2)","Video adjustments",K::Number,-3,3,""},
        {"mpv.video-pan-x","Horizontal pan","Video adjustments",K::Number,-2,2,""},
        {"mpv.video-pan-y","Vertical pan","Video adjustments",K::Number,-2,2,""},
        {"mpv.video-rotate","Rotation","Video adjustments",K::Choice,0,0,"0|90|180|270"},
        {"mpv.video-aspect-override","Aspect ratio (-1 restores original)","Video adjustments",K::Text,0,0,""},
        {"mpv.video-crop","Crop (widthxheight+x+y, empty disables)","Video adjustments",K::Text,0,0,""},
        {"mpv.vf","Video filter chain (mpv vf syntax)","Video adjustments",K::Multiline,0,0,""},
        {"mpv.af","Audio filter chain (mpv af syntax)","Audio",K::Multiline,0,0,""}}) {
      const auto name=option.key.substr(4);
      Binding value;value.field=option;
      value.get=[name](const Preferences& preferences){const auto found=preferences.mpvOptions.find(name);if(found!=preferences.mpvOptions.end())return found->second;if(name=="video-aspect-override")return std::string("-1");if(name=="vf"||name=="af"||name=="video-crop")return std::string{};return std::string("0");};
      value.set=[name,option](Preferences& preferences,std::string_view text,bool strict){
        if(text.size()>131072||text.find('\0')!=std::string_view::npos||(!text.empty()&&fromUtf8(text).empty()))return false;
        if(option.kind==K::Number){double parsed=0;if(!parseNumber(text,parsed)||!std::isfinite(parsed)||(strict&&(parsed<option.minimum||parsed>option.maximum)))return false;}
        if(option.kind==K::Choice){const auto options=split(option.choices,'|');if(std::find(options.begin(),options.end(),text)==options.end())return false;}
        if(text.empty())preferences.mpvOptions.erase(name);else preferences.mpvOptions[name]=text;return true;};
      value.normalize=[](Preferences&){};
      list.push_back(std::move(value));
    }
    for(std::size_t i=0;i<10;++i){
      constexpr const char* frequencies[]={"31 Hz","63 Hz","125 Hz","250 Hz","500 Hz","1 kHz","2 kHz","4 kHz","8 kHz","16 kHz"};
      Binding value;value.field={"audioEqualizer"+std::to_string(i),std::string("Equalizer ")+frequencies[i]+" (dB)","Audio",K::Number,-24,24,""};
      value.get=[i](const Preferences& preferences){std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17)<<preferences.audioEqualizer[i];return out.str();};
      value.set=[i](Preferences& preferences,std::string_view text,bool strict){double parsed=0;if(!parseNumber(text,parsed)||!std::isfinite(parsed)||(strict&&(parsed<-24||parsed>24)))return false;preferences.audioEqualizer[i]=parsed;return true;};
      value.normalize=[](Preferences&){};list.push_back(std::move(value));
    }
    return list;
  }();
  return value;
}

Preferences normalized(Preferences value) {
  for (const auto& binding : bindings()) binding.normalize(value);
  value.volume = std::isfinite(value.volume) ? std::clamp(value.volume, 0.0, 100.0) : 70.0;
  value.speed = std::isfinite(value.speed) ? std::clamp(value.speed, 0.25, 4.0) : 1.0;
  value.windowWidth = std::clamp(value.windowWidth, 640, 7680);
  value.windowHeight = std::clamp(value.windowHeight, 420, 4320);
  std::vector<std::string> recent;
  for (const auto& entry : value.recentFiles) {
    if (entry.empty() || entry.size() > 32768 || entry.find('\0')!=std::string::npos || fromUtf8(entry).empty()) continue;
    if (std::none_of(recent.begin(), recent.end(), [&](const auto& path) { return samePath(path, entry); })) {
      recent.push_back(entry);
      if (recent.size() == maxRecentFiles) break;
    }
  }
  value.recentFiles = std::move(recent);
  return value;
}

}  // namespace

bool naturalLess(std::string_view left, std::string_view right) {
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < left.size() && j < right.size()) {
    const bool leftDigit = left[i] >= '0' && left[i] <= '9';
    const bool rightDigit = right[j] >= '0' && right[j] <= '9';
    if (leftDigit && rightDigit) {
      const auto leftStart = i;
      const auto rightStart = j;
      while (i < left.size() && left[i] >= '0' && left[i] <= '9') ++i;
      while (j < right.size() && right[j] >= '0' && right[j] <= '9') ++j;
      auto significantLeft = leftStart;
      auto significantRight = rightStart;
      while (significantLeft < i && left[significantLeft] == '0') ++significantLeft;
      while (significantRight < j && right[significantRight] == '0') ++significantRight;
      const auto leftDigits = i - significantLeft;
      const auto rightDigits = j - significantRight;
      if (leftDigits != rightDigits) return leftDigits < rightDigits;
      const auto comparison = left.substr(significantLeft, leftDigits)
                                  .compare(right.substr(significantRight, rightDigits));
      if (comparison != 0) return comparison < 0;
      if (i - leftStart != j - rightStart) return i - leftStart < j - rightStart;
    } else {
      const auto a = static_cast<unsigned char>(asciiLower(left[i]));
      const auto b = static_cast<unsigned char>(asciiLower(right[j]));
      if (a != b) return a < b;
      ++i;
      ++j;
    }
  }
  if (i != left.size() || j != right.size()) return i == left.size();
  return left < right;
}

const std::vector<PreferenceField>& preferenceFields() {
  static const std::vector<PreferenceField> fields = [] {
    std::vector<PreferenceField> result;
    for (const auto& binding : bindings()) result.push_back(binding.field);
    return result;
  }();
  return fields;
}

std::string preferenceValue(const Preferences& preferences, std::string_view key) {
  for (const auto& binding : bindings()) if (binding.field.key == key) return binding.get(preferences);
  return {};
}

bool setPreferenceValue(Preferences& preferences, std::string_view key,
                        std::string_view value, std::string& error) {
  error.clear();
  if (key.ends_with("Color")) {
    if ((value.size()!=7 && value.size()!=9) || value.front()!='#' ||
        !std::all_of(value.begin()+1,value.end(),[](char ch) { return hexValue(ch)>=0; })) {
      error="Colors must use #RRGGBB or #AARRGGBB hexadecimal notation."; return false;
    }
  }
  for (const auto& binding : bindings()) {
    if (binding.field.key != key) continue;
    if (binding.set(preferences,value,true)) return true;
    error="Invalid value for "+binding.field.label;
    if (binding.field.key=="keyBindings") error+=". Use unique supported keys with Ctrl and/or Shift, one key=command per line; Alt, Windows, and mouse bindings are not supported.";
    if (binding.field.kind==PreferenceKind::Integer || binding.field.kind==PreferenceKind::Number)
      error+=" (allowed range "+std::to_string(binding.field.minimum)+" to "+std::to_string(binding.field.maximum)+")";
    else if (binding.field.kind==PreferenceKind::Choice) error+=" (choose "+binding.field.choices+")";
    return false;
  }
  error="Unknown preference: "+std::string(key); return false;
}

Preferences normalizePreferences(Preferences preferences) { return normalized(std::move(preferences)); }

std::string normalizeKeyBinding(std::string_view key) {
  const auto parts=split(trim(key),'+');
  if (parts.empty()) return {};
  bool ctrl=false,alt=false,shift=false,win=false;
  std::string primary;
  for (const auto& raw:parts) {
    auto part=trim(raw); std::transform(part.begin(),part.end(),part.begin(),asciiLower);
    if (part.empty()) return {};
    if (part=="ctrl" || part=="control" || part=="cmd" || part=="command" || part=="meta") { if(ctrl)return{};ctrl=true; }
    else if (part=="alt" || part=="option") { if(alt)return{};alt=true; }
    else if (part=="shift") { if(shift)return{};shift=true; }
    else if (part=="win" || part=="super") { if(win)return{};win=true; }
    else {
      if (!primary.empty() || part.size()>32 || part.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_[];'/.,\\`-=+")!=std::string::npos) return {};
      static const std::map<std::string,std::string> aliases{{"space","Space"},{"enter","Enter"},{"return","Enter"},{"esc","Escape"},{"escape","Escape"},{"tab","Tab"},{"backspace","Backspace"},{"left","Left"},{"right","Right"},{"up","Up"},{"down","Down"},{"home","Home"},{"end","End"},{"pgup","PageUp"},{"pageup","PageUp"},{"pgdwn","PageDown"},{"pgdown","PageDown"},{"pagedown","PageDown"},{"delete","Delete"},{"insert","Insert"},{"plus","Plus"},{"minus","Minus"},{"mouseleft","MouseLeft"},{"mouseright","MouseRight"},{"mousemiddle","MouseMiddle"},{"wheelup","WheelUp"},{"wheeldown","WheelDown"}};
      const auto alias=aliases.find(part);
      if (alias!=aliases.end()) primary=alias->second;
      else { primary=part; if(primary[0]>='a' && primary[0]<='z') primary[0]=static_cast<char>(primary[0]-32); }
    }
  }
  if (primary.empty()) return {};
  return std::string(ctrl?"Ctrl+":"")+(alt?"Alt+":"")+(shift?"Shift+":"")+(win?"Win+":"")+primary;
}

std::vector<std::string> shortcutKeys(std::string_view defaultKey, std::string_view command,
                                      const std::map<std::string, std::string>& overrides) {
  std::vector<std::string> keys;
  const auto matches=[&](std::string_view value) {
    if (value.starts_with("velin:")) value.remove_prefix(6);
    return value==command;
  };
  const auto original=normalizeKeyBinding(defaultKey);
  if (!original.empty()) {
    const auto replacement=overrides.find(original);
    if (replacement==overrides.end() || matches(replacement->second)) keys.push_back(original);
  }
  for (const auto& [key,value]:overrides)
    if (key!=original && supportedShortcut(key) && matches(value)) keys.push_back(key);
  return keys;
}

std::string formatTime(double seconds) {
  if (!std::isfinite(seconds) || seconds < 0) return "00:00";
  // A conservative bound keeps the double-to-integer conversion well defined.
  constexpr double maximum = 9000000000000000.0;
  const auto total = static_cast<std::uint64_t>(std::min(seconds, maximum));
  const auto hours = total / 3600;
  const auto minutes = (total / 60) % 60;
  const auto remainder = total % 60;
  std::ostringstream out;
  out.imbue(std::locale::classic());
  if (hours > 0) out << hours << ':';
  out << std::setfill('0') << std::setw(2) << minutes << ':' << std::setw(2) << remainder;
  return out.str();
}

std::wstring fromUtf8(std::string_view text) {
  std::wstring result;
  result.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    const auto first = static_cast<unsigned char>(text[i++]);
    std::uint32_t code = 0;
    int following = 0;
    std::uint32_t minimum = 0;
    if (first < 0x80) {
      code = first;
    } else if (first >= 0xc2 && first <= 0xdf) {
      code = first & 0x1f;
      following = 1;
      minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      code = first & 0x0f;
      following = 2;
      minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      code = first & 0x07;
      following = 3;
      minimum = 0x10000;
    } else {
      return {};
    }
    for (int n = 0; n < following; ++n) {
      if (i >= text.size()) return {};
      const auto next = static_cast<unsigned char>(text[i++]);
      if ((next & 0xc0) != 0x80) return {};
      code = (code << 6) | (next & 0x3f);
    }
    if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return {};
    if constexpr (sizeof(wchar_t) == 2) {
      if (code > 0xffff) {
        code -= 0x10000;
        result.push_back(static_cast<wchar_t>(0xd800 + (code >> 10)));
        result.push_back(static_cast<wchar_t>(0xdc00 + (code & 0x3ff)));
      } else {
        result.push_back(static_cast<wchar_t>(code));
      }
    } else {
      result.push_back(static_cast<wchar_t>(code));
    }
  }
  return result;
}

std::string toUtf8(std::wstring_view text) {
  std::string result;
  result.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    auto code = static_cast<std::uint32_t>(text[i]);
    // UTF-16 surrogate pairs are combined for 32-bit wchar_t too, so callers
    // that assemble UTF-16 code units (file decoding) work on every platform.
    if (code >= 0xd800 && code <= 0xdbff) {
      if (++i >= text.size()) return {};
      const auto low = static_cast<std::uint32_t>(text[i]);
      if (low < 0xdc00 || low > 0xdfff) return {};
      code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
    } else if (code >= 0xdc00 && code <= 0xdfff) {
      return {};
    }
    if (code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return {};
    if (code < 0x80) {
      result.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      result.push_back(static_cast<char>(0xc0 | (code >> 6)));
      result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else if (code < 0x10000) {
      result.push_back(static_cast<char>(0xe0 | (code >> 12)));
      result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
      result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else {
      result.push_back(static_cast<char>(0xf0 | (code >> 18)));
      result.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
      result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
      result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    }
  }
  return result;
}

std::string mediaTitle(std::string_view pathOrUrl) {
  if (pathOrUrl.empty()) return "Untitled";
  const bool url = isUrl(pathOrUrl);
  auto path = url ? pathOrUrl.substr(0, pathOrUrl.find_first_of("?#")) : pathOrUrl;
  while (!path.empty() && (path.back() == '/' || path.back() == '\\')) path.remove_suffix(1);
  const auto slash = path.find_last_of("/\\");
  auto title = std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
  if (url) title = decodeEscapes(title);
  return title.empty() ? "Untitled" : title;
}

bool isSubtitleFile(std::string_view path) {
  constexpr std::array suffixes{".srt", ".ass", ".ssa", ".sub", ".idx", ".vtt", ".sup", ".smi", ".sami", ".lrc", ".ttml", ".dfxp"};
  const auto suffix = extension(path);
  return std::find(suffixes.begin(), suffixes.end(), suffix) != suffixes.end();
}

bool isMediaFile(std::string_view path) {
  constexpr std::array suffixes{
      ".3g2", ".3gp", ".aac", ".ac3", ".aif", ".aiff", ".alac", ".ape", ".asf", ".avi", ".divx",
      ".dts", ".dv", ".f4v", ".flac", ".flv", ".m2t", ".m2ts", ".m2v", ".m3u", ".m3u8", ".m4a",
      ".m4b", ".m4v", ".mid", ".mka", ".mkv", ".mov", ".mp2", ".mp3", ".mp4", ".mpe", ".mpeg",
      ".mpg", ".mts", ".mxf", ".oga", ".ogg", ".ogm", ".ogv", ".opus", ".pls", ".rm", ".rmvb",
      ".tak", ".ts", ".tta", ".vob", ".wav", ".webm", ".wma", ".wmv", ".wv"};
  const auto suffix = extension(path);
  return std::find(suffixes.begin(), suffixes.end(), suffix) != suffixes.end();
}

std::vector<std::string> relatedMediaPaths(const std::string& selected) {
  std::vector<std::string> result{selected};
  const auto path = std::filesystem::u8path(selected);
  std::error_code error;
  if (!isMediaFile(selected) || !std::filesystem::is_regular_file(path, error)) return result;
  auto signature = [](const std::filesystem::path& file) {
    auto text = file.stem().u8string();
    std::string key;
    bool digit = false, numbered = false;
    for (auto c : text) {
      if (c >= '0' && c <= '9') { if (!digit) key += '#'; digit = numbered = true; }
      else { digit = false; key += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); }
    }
    return numbered ? key : std::string{};
  };
  const auto key = signature(path);
  if (key.empty()) return result;
  auto parent = path.parent_path();
  if (parent.empty()) parent = ".";
  std::vector<std::string> siblings;
  for (std::filesystem::directory_iterator it(parent, error), end; !error && it != end; it.increment(error)) {
    if (!it->is_regular_file(error) || signature(it->path()) != key) continue;
    if (std::filesystem::equivalent(path, it->path(), error)) continue;
    const auto utf8 = it->path().u8string();
    std::string name(utf8.begin(), utf8.end());
    if (isMediaFile(name)) siblings.push_back(std::move(name));
  }
  std::sort(siblings.begin(), siblings.end(), naturalLess);
  result.insert(result.end(), siblings.begin(), siblings.end());
  return result;
}

std::vector<std::string> expandMediaPaths(const std::vector<std::string>& input) {
  std::vector<std::string> result;
  for (const auto& item : input) {
    if (item.empty()) continue;
    if (isUrl(item)) {
      result.push_back(item);
      continue;
    }
    try {
      const auto path = pathFromUtf8(item);
      std::error_code error;
      if (!std::filesystem::is_directory(path, error)) {
        result.push_back(item);
        continue;
      }
      std::vector<std::string> children;
      const auto options = std::filesystem::directory_options::skip_permission_denied;
      for (std::filesystem::directory_iterator it(path, options, error), end; it != end; it.increment(error)) {
        if (error) break;
        const auto& entry = *it;
        if (!entry.is_regular_file(error) || error) {
          error.clear();
          continue;
        }
        auto child = pathToUtf8(entry.path());
        if (isMediaFile(child)) children.push_back(std::move(child));
      }
      std::sort(children.begin(), children.end(), naturalLess);
      result.insert(result.end(), children.begin(), children.end());
    } catch (const std::filesystem::filesystem_error&) {
      result.push_back(item);
    }
  }
  return result;
}

SettingsStore::SettingsStore(std::filesystem::path directory) : directory_(std::move(directory)) {}

Preferences SettingsStore::load() const {
  Preferences result;
  const auto path = directory_ / "preferences.ini";
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > maxSettingsBytes) return result;
  std::ifstream stream(path, std::ios::binary);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line.front() == '#' || line.front() == ';') continue;
    const auto separator = line.find('=');
    if (separator == std::string::npos) continue;
    const std::string_view key(line.data(), separator);
    const std::string_view value(line.data() + separator + 1, line.size() - separator - 1);
    if (key == "recent" && result.recentFiles.size() < maxRecentFiles * 2) result.recentFiles.push_back(decodeEscapes(value));
    else for (const auto& binding:bindings()) if (binding.field.key==key) { binding.set(result,decodeEscapes(value),false); break; }
  }
  return normalized(std::move(result));
}

bool SettingsStore::save(const Preferences& preferences) const {
  const auto value = normalized(preferences);
  std::ostringstream stream; stream.imbue(std::locale::classic());
  stream << "# Velin preferences\nversion=2\n";
  for (const auto& binding:bindings()) {
    // Synthetic editors use the canonical mpvOptions map / equalizer array.
    if(binding.field.key.starts_with("mpv.") || (binding.field.key.starts_with("audioEqualizer")&&binding.field.key!="audioEqualizer"))continue;
    stream << binding.field.key << '=' << encodeEscapes(binding.get(value)) << '\n';
  }
  for (const auto& entry:value.recentFiles) stream << "recent=" << encodeEscapes(entry) << '\n';
  std::string error;
  return writeTextFileAtomic(directory_/"preferences.ini",stream.str(),error);
}

void SettingsStore::rememberFile(Preferences& preferences, const std::string& path) const {
  if (path.empty() || path.size() > 32768 || fromUtf8(path).empty()) return;
  auto& recent = preferences.recentFiles;
  std::erase_if(recent, [&](const auto& entry) { return samePath(entry, path); });
  recent.insert(recent.begin(), path);
  if (recent.size() > maxRecentFiles) recent.resize(maxRecentFiles);
}

namespace {
struct PlistValue {
  enum class Kind { Scalar, Array, Dictionary } kind = Kind::Scalar;
  std::string text;
  std::vector<PlistValue> array;
  std::map<std::string,PlistValue> dictionary;
};

std::string xmlUnescape(std::string_view input) {
  std::string output;
  for(std::size_t i=0;i<input.size();++i) {
    if(input[i]!='&'){output+=input[i];continue;}
    const auto end=input.find(';',i+1);if(end==std::string_view::npos||end-i>16)throw std::runtime_error("Malformed XML character entity.");
    const auto entity=input.substr(i+1,end-i-1);
    if(entity=="amp")output+='&';else if(entity=="lt")output+='<';else if(entity=="gt")output+='>';else if(entity=="quot")output+='"';else if(entity=="apos")output+='\'';
    else if(entity.starts_with('#')) {
      const bool hexadecimal=entity.size()>1&&(entity[1]=='x'||entity[1]=='X');const auto digits=entity.substr(hexadecimal?2:1);std::uint32_t point=0;
      const auto parsed=std::from_chars(digits.data(),digits.data()+digits.size(),point,hexadecimal?16:10);
      if(parsed.ec!=std::errc{}||parsed.ptr!=digits.data()+digits.size()||point==0||point>0x10ffff||(point>=0xd800&&point<=0xdfff))throw std::runtime_error("Invalid XML character entity.");
      std::wstring wide;
      if constexpr(sizeof(wchar_t)==2) {if(point>0xffff){point-=0x10000;wide+=static_cast<wchar_t>(0xd800+(point>>10));wide+=static_cast<wchar_t>(0xdc00+(point&1023));}else wide+=static_cast<wchar_t>(point);}
      else wide+=static_cast<wchar_t>(point);
      output+=toUtf8(wide);
    } else throw std::runtime_error("External or unknown XML entities are not supported.");
    i=end;
  }
  if(!output.empty()&&fromUtf8(output).empty())throw std::runtime_error("Property list contains invalid UTF-8.");
  return output;
}

class XmlPlist {
 public:
  explicit XmlPlist(std::string_view input):input_(input){}
  PlistValue parse(){skip();auto result=node(0);skip();if(position_!=input_.size())throw std::runtime_error("Unexpected content after the property list.");return result;}
 private:
  void skip(){
    for(;;){while(position_<input_.size()&&(input_[position_]==' '||input_[position_]=='\r'||input_[position_]=='\n'||input_[position_]=='\t'))++position_;
      const auto rest=input_.substr(position_);std::string_view ending;
      if(rest.starts_with("<?"))ending="?>";else if(rest.starts_with("<!--"))ending="-->";
      else if(rest.starts_with("<!DOCTYPE")){const auto end=input_.find('>',position_);if(end==std::string_view::npos||input_.substr(position_,end-position_).find('[')!=std::string_view::npos)throw std::runtime_error("Property-list DTD subsets are not supported.");position_=end+1;continue;}
      else break;
      const auto end=input_.find(ending,position_);if(end==std::string_view::npos)throw std::runtime_error("Unterminated XML declaration or comment.");position_=end+ending.size();
    }
  }
  bool close(std::string_view name){skip();const auto closing="</"+std::string(name)+">";if(input_.substr(position_).starts_with(closing)){position_+=closing.size();return true;}return false;}
  PlistValue node(int depth){
    if(depth>64||++nodes_>100000)throw std::runtime_error("Property-list nesting or object limit exceeded.");
    skip();if(position_>=input_.size()||input_[position_]!='<')throw std::runtime_error("Expected a property-list element.");
    const auto end=input_.find('>',position_);if(end==std::string_view::npos)throw std::runtime_error("Unterminated XML element.");
    auto tag=trim(input_.substr(position_+1,end-position_-1));position_=end+1;const bool empty=tag.ends_with('/');if(empty)tag=trim(std::string_view(tag).substr(0,tag.size()-1));
    const auto separator=tag.find_first_of(" \t\r\n");const auto name=tag.substr(0,separator);PlistValue result;
    if(name=="true"||name=="false"){result.text=name=="true"?"1":"0";if(!empty&&!close(name))throw std::runtime_error("Malformed Boolean property.");return result;}
    if(name=="plist"){if(empty)throw std::runtime_error("Empty property-list wrapper.");result=node(depth+1);if(!close("plist"))throw std::runtime_error("Missing property-list closing tag.");return result;}
    if(name=="dict"){
      result.kind=PlistValue::Kind::Dictionary;if(empty)return result;
      while(!close("dict")){auto key=node(depth+1);if(key.kind!=PlistValue::Kind::Scalar||key.text.empty())throw std::runtime_error("Invalid dictionary key.");auto value=node(depth+1);result.dictionary.insert_or_assign(std::move(key.text),std::move(value));}return result;
    }
    if(name=="array"){
      result.kind=PlistValue::Kind::Array;if(empty)return result;
      while(!close("array"))result.array.push_back(node(depth+1));
      return result;
    }
    if(name!="key"&&name!="string"&&name!="integer"&&name!="real"&&name!="data"&&name!="date")throw std::runtime_error("Unsupported XML property-list element: "+name);
    if(empty)return result;
    const auto closing="</"+name+">";const auto finish=input_.find(closing,position_);if(finish==std::string_view::npos)throw std::runtime_error("Missing property-list element closing tag.");
    const auto contents=input_.substr(position_,finish-position_);if(contents.find('<')!=std::string_view::npos)throw std::runtime_error("Nested text markup is not valid in a property-list value.");
    result.text=xmlUnescape(contents);position_=finish+closing.size();return result;
  }
  std::string_view input_;std::size_t position_=0,nodes_=0;
};

class BinaryPlist {
 public:
  explicit BinaryPlist(std::string_view input):input_(input){}
  PlistValue parse(){
    if(input_.size()<40||!input_.starts_with("bplist00"))throw std::runtime_error("Unsupported binary property-list header.");
    const auto trailer=input_.size()-32;offsetSize_=byte(trailer+6);referenceSize_=byte(trailer+7);count_=integer(trailer+8,8);const auto root=integer(trailer+16,8);table_=integer(trailer+24,8);
    if(offsetSize_<1||offsetSize_>8||referenceSize_<1||referenceSize_>8||count_==0||count_>100000||root>=count_||table_<8||table_>trailer||count_>(trailer-table_)/offsetSize_)throw std::runtime_error("Invalid binary property-list object table.");
    return object(root,0);
  }
 private:
  unsigned byte(std::uint64_t offset)const{if(offset>=input_.size())throw std::runtime_error("Truncated binary property list.");return static_cast<unsigned char>(input_[static_cast<std::size_t>(offset)]);}
  std::uint64_t integer(std::uint64_t offset,unsigned bytes)const{if(bytes>8||offset>input_.size()||bytes>input_.size()-offset)throw std::runtime_error("Binary property-list integer is out of bounds.");std::uint64_t result=0;for(unsigned i=0;i<bytes;++i)result=(result<<8)|byte(offset+i);return result;}
  std::uint64_t length(unsigned small,std::uint64_t& position){if(small<15)return small;const auto marker=byte(position++);if((marker>>4)!=1||(marker&15)>3)throw std::runtime_error("Invalid binary property-list length.");const unsigned bytes=1u<<(marker&15);const auto value=integer(position,bytes);position+=bytes;return value;}
  void bounds(std::uint64_t position,std::uint64_t count,std::uint64_t width)const{if(position>table_||width==0||count>(table_-position)/width)throw std::runtime_error("Binary property-list object data is out of bounds.");}
  PlistValue object(std::uint64_t index,int depth){
    if(index>=count_||depth>64)throw std::runtime_error("Invalid binary property-list reference or nesting.");
    if(const auto found=cache_.find(index);found!=cache_.end())return found->second;
    if(!active_.insert(index).second)throw std::runtime_error("Cyclic binary property list.");
    auto position=integer(table_+index*offsetSize_,offsetSize_);if(position<8||position>=table_)throw std::runtime_error("Invalid binary property-list object offset.");
    const auto marker=byte(position++),type=marker>>4,small=marker&15;PlistValue result;
    if(type==0){if(marker==8)result.text="0";else if(marker==9)result.text="1";else if(marker!=0)throw std::runtime_error("Unsupported binary property-list simple value.");}
    else if(type==1){if(small>3)throw std::runtime_error("Property-list integers wider than 64 bits are not supported.");const unsigned bytes=1u<<small;bounds(position,1,bytes);const auto bits=integer(position,bytes);if(bytes==8)result.text=std::to_string(std::bit_cast<std::int64_t>(bits));else result.text=std::to_string(bits);}
    else if(type==2||type==3){const unsigned bytes=1u<<small;bounds(position,1,bytes);double value=0;if(bytes==8)value=std::bit_cast<double>(integer(position,8));else if(bytes==4)value=std::bit_cast<float>(static_cast<std::uint32_t>(integer(position,4)));else throw std::runtime_error("Unsupported binary real number.");if(!std::isfinite(value))throw std::runtime_error("Non-finite property-list number.");std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17)<<value;result.text=out.str();}
    else if(type==4||type==5||type==6||type==7){const auto size=length(small,position);bounds(position,size,type==6?2:1);if(size>1024*1024)throw std::runtime_error("Property-list scalar exceeds the size limit.");if(type==6){std::wstring wide;for(std::uint64_t i=0;i<size;++i){const auto unit=static_cast<std::uint32_t>(integer(position+i*2,2));if constexpr(sizeof(wchar_t)==2)wide+=static_cast<wchar_t>(unit);else if(unit>=0xd800&&unit<=0xdbff){if(++i>=size)throw std::runtime_error("Unpaired UTF-16 surrogate.");const auto low=integer(position+i*2,2);if(low<0xdc00||low>0xdfff)throw std::runtime_error("Unpaired UTF-16 surrogate.");wide+=static_cast<wchar_t>(0x10000+((unit-0xd800)<<10)+(low-0xdc00));}else wide+=static_cast<wchar_t>(unit);}result.text=toUtf8(wide);if(size&&!wide.empty()&&result.text.empty())throw std::runtime_error("Invalid UTF-16 property-list string.");}else if(type!=4){result.text=std::string(input_.substr(static_cast<std::size_t>(position),static_cast<std::size_t>(size)));if(!result.text.empty()&&fromUtf8(result.text).empty())throw std::runtime_error("Invalid property-list string encoding.");}}
    else if(type==10||type==13){const auto size=length(small,position);bounds(position,size*(type==13?2:1),referenceSize_);if(size>100000)throw std::runtime_error("Property-list collection exceeds the object limit.");if(type==10){result.kind=PlistValue::Kind::Array;for(std::uint64_t i=0;i<size;++i)result.array.push_back(object(integer(position+i*referenceSize_,referenceSize_),depth+1));}else{result.kind=PlistValue::Kind::Dictionary;for(std::uint64_t i=0;i<size;++i){auto key=object(integer(position+i*referenceSize_,referenceSize_),depth+1);if(key.kind!=PlistValue::Kind::Scalar)throw std::runtime_error("Invalid binary dictionary key.");result.dictionary.insert_or_assign(key.text,object(integer(position+(size+i)*referenceSize_,referenceSize_),depth+1));}}}
    else throw std::runtime_error("Unsupported binary property-list object type.");
    active_.erase(index);cache_[index]=result;return result;
  }
  std::string_view input_;unsigned offsetSize_=0,referenceSize_=0;std::uint64_t count_=0,table_=0;
  std::set<std::uint64_t> active_;std::map<std::uint64_t,PlistValue> cache_;
};
}

bool importIinaPreferences(const std::filesystem::path& file,Preferences& preferences,std::string& report) {
  report.clear();std::error_code status;const auto size=std::filesystem::file_size(file,status);
  if(status||size>16*1024*1024){report=status?"Cannot read the IINA preferences file: "+status.message():"The property list exceeds the 16 MiB import safety limit.";return false;}
  std::ifstream input(file,std::ios::binary);if(!input){report="Cannot open the IINA preferences file.";return false;}
  std::string text(std::istreambuf_iterator<char>(input),{});if(text.starts_with("\xef\xbb\xbf"))text.erase(0,3);
  try {
    const auto root=text.starts_with("bplist")?BinaryPlist(text).parse():XmlPlist(text).parse();
    if(root.kind!=PlistValue::Kind::Dictionary){report="IINA preferences must be a property-list dictionary.";return false;}
    auto result=preferences;std::size_t imported=0;std::set<std::string> consumed;
    auto apply=[&](const std::string& source,const std::string& target){const auto found=root.dictionary.find(source);if(found==root.dictionary.end()||found->second.kind!=PlistValue::Kind::Scalar)return;std::string error;if(setPreferenceValue(result,target,trim(found->second.text),error)){++imported;consumed.insert(source);}};
    for(const auto& [source,target]:std::vector<std::pair<std::string,std::string>>{
      {"alwaysFloatOnTop","alwaysOnTop"},{"resumeLastPosition","rememberPosition"},
      {"recordPlaybackHistory","recordHistory"},{"recordRecentFiles","recordRecentFiles"},{"pauseWhenOpen","pauseOnOpen"},{"pauseWhenMinimized","pauseWhenMinimized"},
      {"pauseWhenInactive","pauseWhenInactive"},{"pauseWhenGoesToSleep","pauseOnSleep"},{"keepOpenOnFileEnd","pauseAtEnd"},{"playlistAutoPlayNext","autoPlayNext"},
      {"playlistAutoAdd","autoAddRelatedFiles"},{"preventScreenSaver","preventSleep"},{"fullScreenWhenOpen","fullscreenOnOpen"},{"screenShotFolder","screenshotDirectory"},
      {"screenShotTemplate","screenshotTemplate"},{"screenShotIncludeSubtitle","screenshotIncludeSubtitles"},{"screenshotCopyToClipboard","screenshotCopyToClipboard"},
      {"autoSwitchToMusicMode","autoMusicMode"},{"showChapterPos","showChapterMarkers"},{"enableThumbnailPreview","showTimelineThumbnails"},
      {"enableControlBarAutoHide","fullscreenAutoHide"},{"controlBarAutoHideTimeout","controlsHideSeconds"},{"trailingSidebarWidth","sidebarWidth"},
      {"thumbnailWidth","timelineThumbnailWidth"},{"enableHdrSupport","hdrEnabled"},{"loadIccProfile","iccProfileAuto"},{"audioDevice","audioDevice"},
      {"audioLanguage","audioLanguages"},{"subTextFont","subtitleFont"},{"subTextSize","subtitleSize"},{"subTextColorString","subtitleColor"},
      {"subBorderSize","subtitleBorderSize"},{"subBorderColorString","subtitleBorderColor"},{"subShadowSize","subtitleShadowSize"},{"subShadowColorString","subtitleShadowColor"},
      {"subPos","subtitlePosition"},{"subLang","subtitleLanguages"},{"subAutoLoadSearchPath","subtitleSearchPaths"},{"ignoreAssStyles","subtitleOverrideAss"},
      {"userAgent","networkUserAgent"},{"httpProxy","networkProxy"},{"secPrefech","networkCacheSeconds"},{"ytdlEnabled","youtubeDlEnabled"},{"ytdlSearchPath","youtubeDlPath"}})apply(source,target);
    // IINA uses the initial volume only when its separate switch is enabled.
    // An absent switch has IINA's default value, false.
    const auto initialVolumeEnabled=root.dictionary.find("enableInitialVolume");
    if(initialVolumeEnabled==root.dictionary.end()) apply("softVolume","volume");
    else if(initialVolumeEnabled->second.kind==PlistValue::Kind::Scalar) {
      const auto value=trim(initialVolumeEnabled->second.text);
      if(value=="1" || value=="0") {
        consumed.insert("enableInitialVolume");
        apply(value=="1"?"initialVolume":"softVolume","volume");
      }
    }
    auto enumerated=[&](const char* source,const char* target,const std::vector<std::string>& options){const auto found=root.dictionary.find(source);int index=-1;if(found==root.dictionary.end()||!parseNumber(trim(found->second.text),index)||index<0||static_cast<std::size_t>(index)>=options.size()||options[static_cast<std::size_t>(index)].empty())return;std::string error;if(setPreferenceValue(result,target,options[static_cast<std::size_t>(index)],error)){++imported;consumed.insert(source);}};
    enumerated("hardwareDecoder","hardwareDecoding",{"no","auto","auto-copy"});
    enumerated("screenShotFormat","screenshotFormat",{"png","jpg","jpeg","webp","jxl","avif"});
    enumerated("themeMaterial","theme",{"dark","","light","","system"});
    enumerated("actionAfterLaunch","launchAction",{"welcome","open-files",""});
    enumerated("replayGain","replayGain",{"no","track","album"});
    // IINA's filename matching and intelligent matching are not mpv's
    // sub-auto modes. Only the disabled mode has an equivalent here.
    enumerated("subAutoLoadIINA","subtitleAutoLoad",{"no","",""});
    for(const auto& [source,option]:std::vector<std::pair<std::string,std::string>>{{"videoThreads","vd-lavc-threads"},{"audioThreads","ad-lavc-threads"},{"replayGainPreamp","replaygain-preamp"},{"replayGainClip","replaygain-clip"},{"replayGainFallback","replaygain-fallback"},{"subBold","sub-bold"},{"subItalic","sub-italic"},{"subBlur","sub-blur"},{"subSpacing","sub-spacing"},{"subMarginX","sub-margin-x"},{"subMarginY","sub-margin-y"},{"defaultEncoding","sub-codepage"},{"ytdlRawOptions","ytdl-raw-options"}}){
      const auto found=root.dictionary.find(source);if(found==root.dictionary.end()||found->second.kind!=PlistValue::Kind::Scalar)continue;auto value=trim(found->second.text);if(option=="replaygain-clip"||option=="sub-bold"||option=="sub-italic"){if(value!="0"&&value!="1")continue;value=value=="1"?"yes":"no";}if(!value.empty()){result.mpvOptions[option]=value;++imported;consumed.insert(source);}
    }
    std::vector<std::string> passthrough;
    { std::istringstream current(result.audioPassthrough); std::string codec;
      while(std::getline(current,codec,',')) if(!codec.empty()) passthrough.push_back(codec); }
    bool passthroughChanged=false;
    for(const auto& [key,codec]:std::vector<std::pair<std::string,std::string>>{{"spdifAC3","ac3"},{"spdifDTS","dts"},{"spdifDTSHD","dts-hd"},{"spdifEAC3","eac3"},{"spdifTRUEHD","truehd"}}){
      const auto found=root.dictionary.find(key);
      if(found==root.dictionary.end() || found->second.kind!=PlistValue::Kind::Scalar) continue;
      const auto value=trim(found->second.text); if(value!="0" && value!="1") continue;
      passthrough.erase(std::remove(passthrough.begin(),passthrough.end(),codec),passthrough.end());
      if(value=="1") passthrough.push_back(codec);
      consumed.insert(key); passthroughChanged=true;
    }
    if(passthroughChanged){result.audioPassthrough.clear();for(const auto& codec:passthrough){if(!result.audioPassthrough.empty())result.audioPassthrough+=',';result.audioPassthrough+=codec;}++imported;}
    if(const auto found=root.dictionary.find("userOptions");found!=root.dictionary.end()&&found->second.kind==PlistValue::Kind::Array){for(const auto& item:found->second.array)if(item.kind==PlistValue::Kind::Array&&item.array.size()==2&&!item.array[0].text.empty()){result.mpvOptions[item.array[0].text]=item.array[1].text;++imported;}consumed.insert("userOptions");}
    if(imported==0){report="The property list was read successfully but contains no supported IINA preferences.";return false;}
    preferences=normalizePreferences(std::move(result));report="Imported "+std::to_string(imported)+" IINA preferences from "+(text.starts_with("bplist")?"binary":"XML")+" plist. "+std::to_string(root.dictionary.size()-consumed.size())+" platform-specific or unrecognized entries were left unchanged. Review macOS file paths before applying.";return true;
  } catch(const std::exception& error){report="Cannot import IINA preferences: "+std::string(error.what());return false;}
}

}  // namespace velin
