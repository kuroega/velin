// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace velin {

std::string formatTime(double seconds);

// Returns a centered mpv crop geometry for an aspect ratio such as "16:9".
std::optional<std::string> centeredCropGeometry(std::int64_t width, std::int64_t height, std::string_view aspect);

// Invalid UTF-8 or unpaired UTF-16 surrogates produce an empty result.
std::wstring fromUtf8(std::string_view text);
std::string toUtf8(std::wstring_view text);

std::string mediaTitle(std::string_view pathOrUrl);
bool isSubtitleFile(std::string_view path);
bool isMediaFile(std::string_view path);

// Expands directories one level, filtering their contents to supported media.
// Explicit file paths and URLs retain their input order and are not filtered.
std::vector<std::string> expandMediaPaths(const std::vector<std::string>& input);

// Adds numbered siblings in natural order after the selected local file.
std::vector<std::string> relatedMediaPaths(const std::string& selected);

enum class Theme { System, Dark, Light };
enum class LaunchAction { Welcome, RestoreSession, OpenFiles };

struct Preferences {
  double volume = 70;
  bool muted = false;
  double speed = 1;
  bool alwaysOnTop = false;
  bool lockWindowAspect = false;
  bool rememberPosition = true;
  int windowWidth = 1120;
  int windowHeight = 740;
  int windowX = -1;
  int windowY = -1;
  bool rememberWindowPosition = true;
  int historyWidth = 980;
  int historyHeight = 620;
  int timelineThumbnailWidth = 240;
  std::vector<std::string> recentFiles;
  LaunchAction launchAction = LaunchAction::Welcome;
  bool restorePlaylist = true;
  bool recordHistory = true;
  bool recordRecentFiles = true;
  bool pauseOnOpen = false;
  bool pauseWhenMinimized = false;
  bool pauseWhenInactive = false;
  bool pauseAtEnd = true;
  bool pauseOnSleep = true;
  bool autoPlayNext = true;
  bool autoAddRelatedFiles = false;
  bool preventSleep = true;
  bool fullscreenOnOpen = false;
  std::string screenshotFormat = "png";
  std::string screenshotDirectory;
  std::string screenshotTemplate = "%F-%n";
  bool screenshotIncludeSubtitles = true;
  bool screenshotCopyToClipboard = false;
  Theme theme = Theme::Dark;
  bool showPlaylist = true;
  bool musicMode = false;
  bool autoMusicMode = true;
  bool pipAlwaysOnTop = true;
  bool pipPauseOnClose = false;
  bool showChapterMarkers = true;
  bool showTimelineThumbnails = true;
  bool fullscreenAutoHide = true;
  double controlsHideSeconds = 3;
  double seekSeconds = 5;
  double volumeStep = 5;
  int sidebarWidth = 286;
  std::string hardwareDecoding = "auto";
  bool hdrEnabled = true;
  bool iccProfileAuto = true;
  std::string iccProfile;
  std::string videoScaler = "spline36";
  std::string downScaler = "mitchell";
  std::string interpolation = "no";
  bool deinterlace = false;
  bool deband = false;
  std::string videoOutput = "gpu-next,gpu";
  std::string audioDevice = "auto";
  std::string audioPassthrough;
  bool audioNormalization = false;
  std::string replayGain = "no";
  std::string audioLanguages;
  double audioDelay = 0;
  std::array<double, 10> audioEqualizer{};
  std::string subtitleFont = "Segoe UI";
  double subtitleSize = 55;
  std::string subtitleColor = "#FFFFFFFF";
  double subtitleBorderSize = 3;
  std::string subtitleBorderColor = "#FF000000";
  double subtitleShadowSize = 0;
  std::string subtitleShadowColor = "#80000000";
  double subtitlePosition = 100;
  double subtitleScale = 1;
  std::string subtitleLanguages;
  std::string subtitleSearchPaths;
  double subtitleDelay = 0;
  bool subtitleVisible = true;
  std::string subtitleAutoLoad = "fuzzy";
  bool subtitleOverrideAss = false;
  std::string secondarySubtitle = "no";
  double secondarySubtitleDelay = 0;
  double secondarySubtitlePosition = 0;
  std::string subtitleProvider = "opensubtitles";
  std::string subtitleProviderApiKey;
  std::string subtitleProviderToken;
  std::string networkProxy;
  int networkCacheSeconds = 30;
  int networkCacheMegabytes = 150;
  std::string networkUserAgent;
  int networkTimeoutSeconds = 30;
  std::string updateManifestUrl;
  bool checkForUpdates = false;
  bool youtubeDlEnabled = true;
  std::string youtubeDlPath;
  std::string youtubeDlFormat = "bestvideo+bestaudio/best";
  bool loadMpvConfig = false;
  std::string mpvConfigDirectory;
  bool scriptsEnabled = true;
  std::string scriptsDirectory;
  bool pluginsEnabled = true;
  std::string pluginsDirectory;
  std::string extraMpvOptions;
  std::map<std::string, std::string> keyBindings;
  std::map<std::string, std::string> mpvOptions;
};

enum class PreferenceKind { Boolean, Integer, Number, Text, Choice, Multiline };
struct PreferenceField {
  std::string key;
  std::string label;
  std::string section;
  PreferenceKind kind = PreferenceKind::Text;
  double minimum = 0;
  double maximum = 0;
  // Choice values separated by '|'. Empty strings are permitted for text fields.
  std::string choices;
};

const std::vector<PreferenceField>& preferenceFields();
std::string preferenceValue(const Preferences& preferences, std::string_view key);
bool setPreferenceValue(Preferences& preferences, std::string_view key,
                        std::string_view value, std::string& error);
Preferences normalizePreferences(Preferences preferences);
std::string normalizeKeyBinding(std::string_view key);
std::vector<std::string> shortcutKeys(std::string_view defaultKey, std::string_view command,
                                      const std::map<std::string, std::string>& overrides);
bool importIinaPreferences(const std::filesystem::path& file, Preferences& preferences,
                           std::string& report);

class SettingsStore {
 public:
  explicit SettingsStore(std::filesystem::path directory);
  [[nodiscard]] Preferences load() const;
  [[nodiscard]] bool save(const Preferences& preferences) const;
  void rememberFile(Preferences& preferences, const std::string& path) const;

 private:
  std::filesystem::path directory_;
};

}  // namespace velin
