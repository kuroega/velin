// SPDX-License-Identifier: GPL-3.0-or-later
#include "velin/core.hpp"
#include "velin/stream_stats.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>

namespace {

void check(bool condition, const char* description) {
  if (!condition) throw std::runtime_error(description);
}

std::string utf8Path(const std::filesystem::path& path) {
  const auto text = path.u8string();
  return {reinterpret_cast<const char*>(text.data()), text.size()};
}

struct TestDirectory {
  std::filesystem::path root;
  std::filesystem::path path;
  bool owned = false;

  TestDirectory() {
    root = std::filesystem::current_path();
    path = root / ("velin-core-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    owned = std::filesystem::create_directory(path);
    if (!owned) throw std::runtime_error("Cannot create isolated test directory");
  }

  ~TestDirectory() {
    // Only remove this newly created direct child of the test working directory.
    if (owned && path.parent_path() == root &&
        path.filename().string().starts_with("velin-core-tests-")) {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  }
};

void testTime() {
  check(velin::formatTime(0) == "00:00", "Zero time");
  check(velin::formatTime(65.999) == "01:05", "Time truncates fractional seconds");
  check(velin::formatTime(3599) == "59:59", "Minute boundary");
  check(velin::formatTime(3600) == "1:00:00", "Hour boundary");
  check(velin::formatTime(360000.5) == "100:00:00", "Long duration");
  check(velin::formatTime(-1) == "00:00", "Negative time");
  check(velin::formatTime(std::numeric_limits<double>::infinity()) == "00:00", "Infinite time");
  check(velin::formatTime(std::numeric_limits<double>::quiet_NaN()) == "00:00", "NaN time");
  check(!velin::formatTime(std::numeric_limits<double>::max()).empty(), "Huge duration is safe");
}

void testCenteredCrop() {
  using velin::centeredCropGeometry;
  check(centeredCropGeometry(1920, 1080, "4:3") == "1440x1080+240+0", "Center a narrower crop");
  check(centeredCropGeometry(1920, 1080, "2.35:1") == "1920x816+0+132", "Center a wider crop");
  check(centeredCropGeometry(1920, 1080, "16:9") == "1920x1080+0+0", "Matching aspect keeps the frame");
  check(centeredCropGeometry(1001, 1001, "1") == "1000x1000+0+0", "Odd dimensions remain in bounds");
  check(!centeredCropGeometry(1920, 1080, "16:0") && !centeredCropGeometry(1920, 1080, "-1") &&
        !centeredCropGeometry(1920, 1080, "16:9 garbage") && !centeredCropGeometry(1, 1080, "16:9") &&
        !centeredCropGeometry(1920, 1080, "1e-300"), "Reject invalid or unusable geometry");
}

void testUnicode() {
  const std::string sample = "C:\\Media\\\xe6\x98\xa0\xe7\x94\xbb \xf0\x9f\x8e\xac.mkv";
  const auto wide = velin::fromUtf8(sample);
  check(!wide.empty() && velin::toUtf8(wide) == sample, "Unicode path round trip");
  const std::string withNull("one\0two", 7);
  check(velin::toUtf8(velin::fromUtf8(withNull)) == withNull, "Explicit string lengths preserve NUL");
  check(velin::fromUtf8("\xc0\xaf").empty(), "Reject overlong UTF-8");
  check(velin::fromUtf8("\xed\xa0\x80").empty(), "Reject UTF-8 surrogate");
  check(velin::fromUtf8("\xf4\x90\x80\x80").empty(), "Reject code points above Unicode range");
  check(velin::fromUtf8("\xe2\x82").empty(), "Reject truncated UTF-8");
  check(velin::toUtf8(std::wstring(1, static_cast<wchar_t>(0xd800))).empty(), "Reject unpaired surrogate");
}

void testTitlesAndExtensions() {
  check(velin::mediaTitle("C:\\Movies\\A film.mkv") == "A film.mkv", "Windows title");
  check(velin::mediaTitle("/media/A film.mkv") == "A film.mkv", "POSIX title");
  check(velin::mediaTitle("https://example.org/Film%20One.mkv?token=secret#chapter") == "Film One.mkv", "URL title");
  check(velin::mediaTitle("https://example.org/") == "example.org", "URL host title");
  check(velin::mediaTitle("a#b?.mkv") == "a#b?.mkv", "Local title does not strip URL punctuation");
  check(velin::mediaTitle("") == "Untitled", "Empty title");
  check(velin::isSubtitleFile("Caption.ASS"), "Case-insensitive subtitle extension");
  check(velin::isSubtitleFile("https://host/sub.vtt?token=1"), "Subtitle URL extension");
  check(!velin::isSubtitleFile("video.mkv"), "Media is not a subtitle");
  check(velin::isMediaFile("movie.MKV") && velin::isMediaFile("music.FLAC"), "Video and audio extensions");
  check(!velin::isMediaFile("notes.txt") && !velin::isMediaFile("sub.srt"), "Folder filtering rejects unrelated files");
}

void testDirectories(const std::filesystem::path& root) {
  const auto media = root / "media";
  std::filesystem::create_directory(media);
  for (const char* filename : {"Episode 10.mkv", "Episode 2.mkv", "Episode 1.mkv", "notes.txt", "Episode 1.srt"}) {
    std::ofstream(media / filename) << "test";
  }
  std::filesystem::create_directory(media / "nested");
  std::ofstream(media / "nested" / "hidden.mp4") << "test";
  const auto result = velin::expandMediaPaths({"https://example.org/stream", utf8Path(media), "explicit.unknown", ""});
  check(result.size() == 5, "Folder expansion filters and remains one level deep");
  check(result.front() == "https://example.org/stream" && result.back() == "explicit.unknown", "Explicit input order is preserved");
  check(velin::mediaTitle(result[1]) == "Episode 1.mkv" &&
        velin::mediaTitle(result[2]) == "Episode 2.mkv" &&
        velin::mediaTitle(result[3]) == "Episode 10.mkv", "Natural episode order");
  const auto empty = root / "empty";
  std::filesystem::create_directory(empty);
  check(velin::expandMediaPaths({utf8Path(empty)}).empty(), "Empty directory does not become a media path");
  const auto unicode = media / std::filesystem::path(std::u8string(u8"Unicode \u96fb\u5f71.mp4"));
  std::ofstream(unicode) << "test";
  const auto expanded = velin::expandMediaPaths({utf8Path(media)});
  check(expanded.size() == 4, "Unicode media filename is discovered");
  check(velin::toUtf8(velin::fromUtf8(expanded.back())) == expanded.back(), "Expanded path has valid UTF-8");
}

void testRelated(const std::filesystem::path& root) {
  const auto folder = root / "series";
  std::filesystem::create_directories(folder);
  for (const auto* name : {"Show 2.mkv", "Show 10.mkv", "Show 1.mp4", "Other 3.mkv", "Show 3.srt", "Movie.mkv"})
    std::ofstream(folder / name) << "fixture";
  const auto selected = utf8Path(folder / "Show 2.mkv");
  const auto related = velin::relatedMediaPaths(selected);
  check(related.size() == 3 && related.front() == selected, "Related files preserve selected playback and exclude unrelated files");
  check(related[1] == utf8Path(folder / "Show 1.mp4") && related[2] == utf8Path(folder / "Show 10.mkv"), "Related siblings use natural order");
  check(velin::relatedMediaPaths(utf8Path(folder / "Movie.mkv")).size() == 1, "Unnumbered movies do not expand");
  check(velin::relatedMediaPaths("https://example.com/Show2.mp4").size() == 1, "Remote media does not scan directories");
}

void testSettings(const std::filesystem::path& root) {
  velin::SettingsStore settings(root / "settings");
  const auto defaults = settings.load();
  check(defaults.volume == 70 && defaults.speed == 1 && defaults.rememberPosition, "Missing settings use defaults");
  velin::Preferences value;
  value.volume = 84.5;
  value.muted = true;
  value.speed = 1.75;
  value.alwaysOnTop = true;
  value.rememberPosition = false;
  value.windowWidth = 1440;
  value.windowHeight = 900;
  const std::string escaped = "https://host/a%20b?x=1\nrecent=not-another-entry";
  value.recentFiles = {"C:\\Media\\\xe6\x98\xa0\xe7\x94\xbb.mkv", escaped};
  check(settings.save(value), "Save preferences");
  const auto read = settings.load();
  check(read.volume == value.volume && read.speed == value.speed && read.muted && read.alwaysOnTop &&
        !read.rememberPosition && read.windowWidth == 1440 && read.windowHeight == 900, "Preference values round trip");
  check(read.recentFiles == value.recentFiles, "Unicode and escaped recent paths round trip without injection");
  value.volume = 23;
  check(settings.save(value) && settings.load().volume == 23, "Atomic replacement of existing settings");

  {
    std::ofstream malformed(root / "settings" / "preferences.ini", std::ios::trunc);
    malformed << "garbage\nunknown=ignored\nvolume=900\nspeed=-3\nwindowWidth=-1\nwindowHeight=99999\n"
              << "muted=not-a-bool\nrememberPosition=false\nrecent=one.mp4\nrecent=one.mp4\nrecent=\n";
  }
  const auto clamped = settings.load();
  check(clamped.volume == 100 && clamped.speed == 0.25 && clamped.windowWidth == 640 &&
        clamped.windowHeight == 4320, "Out-of-range settings are clamped");
  check(!clamped.muted && !clamped.rememberPosition && clamped.recentFiles.size() == 1, "Malformed settings are ignored and recent files deduplicated");
  {
    std::ofstream malformed(root / "settings" / "preferences.ini", std::ios::trunc);
    malformed << "volume=nan\nspeed=inf\nwindowWidth=1200garbage\nvolume=bad\n";
  }
  const auto invalid = settings.load();
  check(invalid.volume == 70 && invalid.speed == 1 && invalid.windowWidth == 1120, "Non-finite and malformed numeric settings use defaults");

  velin::Preferences recent;
  for (int i = 0; i < 20; ++i) settings.rememberFile(recent, "film" + std::to_string(i) + ".mkv");
  check(recent.recentFiles.size() == 12 && recent.recentFiles.front() == "film19.mkv", "Recent files are capped in most-recent order");
  settings.rememberFile(recent, "film10.mkv");
  check(recent.recentFiles.size() == 12 && recent.recentFiles.front() == "film10.mkv", "Reopened file is promoted without duplication");
  settings.rememberFile(recent, "");
  check(recent.recentFiles.size() == 12, "Empty recent entries are ignored");
  value.volume = std::numeric_limits<double>::quiet_NaN();
  value.speed = std::numeric_limits<double>::infinity();
  check(settings.save(value), "Invalid in-memory values can be normalized and saved");
  check(settings.load().volume == 70 && settings.load().speed == 1, "Save normalizes non-finite values");

  const auto blocked = root / "regular-file";
  std::ofstream(blocked) << "not a directory";
  check(!velin::SettingsStore(blocked).save(value), "Unwritable settings location returns failure");
}

void testIinaImport(const std::filesystem::path& root) {
  const auto file = root / "iina-import.plist";
  auto write = [&](const std::string& entries) {
    std::ofstream output(file, std::ios::trunc);
    output << "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>" << entries << "</dict></plist>";
  };
  velin::Preferences preferences;
  std::string report;
  write("<key>softVolume</key><integer>23</integer><key>initialVolume</key><integer>81</integer>"
        "<key>enableInitialVolume</key><false/>");
  check(velin::importIinaPreferences(file, preferences, report) && preferences.volume == 23,
        "Disabled initial volume uses IINA's remembered volume");
  write("<key>softVolume</key><integer>23</integer><key>initialVolume</key><integer>81</integer>"
        "<key>enableInitialVolume</key><true/>");
  check(velin::importIinaPreferences(file, preferences, report) && preferences.volume == 81,
        "Enabled initial volume overrides remembered volume");
  write("<key>softVolume</key><integer>42</integer><key>initialVolume</key><integer>91</integer>");
  check(velin::importIinaPreferences(file, preferences, report) && preferences.volume == 42,
        "Missing initial volume switch uses IINA's disabled default");
  preferences.audioPassthrough = "ac3,dts";
  write("<key>spdifAC3</key><false/>");
  check(velin::importIinaPreferences(file, preferences, report) && preferences.audioPassthrough == "dts",
        "Disabled passthrough codec is removed without changing omitted codecs");
  write("<key>spdifDTS</key><false/>");
  check(velin::importIinaPreferences(file, preferences, report) && preferences.audioPassthrough.empty(),
        "Disabling the last passthrough codec clears the old value");
  write("<key>spdifAC3</key><true/><key>spdifDTS</key><true/>");
  check(velin::importIinaPreferences(file, preferences, report) && preferences.audioPassthrough == "ac3,dts",
        "Enabled passthrough codecs are imported");
  preferences.subtitleAutoLoad = "all";
  preferences.mpvOptions["sub-bold"] = "yes";
  preferences.launchAction = velin::LaunchAction::RestoreSession;
  write("<key>softVolume</key><integer>55</integer>"
        "<key>subAutoLoadIINA</key><integer>1</integer>"
        "<key>actionAfterLaunch</key><integer>2</integer>"
        "<key>subBold</key><string>invalid</string>");
  check(velin::importIinaPreferences(file, preferences, report) && preferences.volume == 55 &&
        preferences.subtitleAutoLoad == "all" && preferences.launchAction == velin::LaunchAction::RestoreSession &&
        preferences.mpvOptions["sub-bold"] == "yes" && report.find("3 platform-specific or unrecognized") != std::string::npos,
        "Unsupported IINA modes and malformed booleans leave existing preferences unchanged");
  write("<key>subAutoLoadIINA</key><integer>0</integer>");
  check(velin::importIinaPreferences(file, preferences, report) && preferences.subtitleAutoLoad == "no",
        "Disabled IINA subtitle autoload has an equivalent mpv setting");
}

}  // namespace

void testStreamStats() {
  check(velin::formatByteSize(512) == "512 B", "Bytes");
  check(velin::formatByteSize(1536) == "1.50 KiB", "KiB");
  check(velin::formatByteSize(150.0 * 1024 * 1024) == "150 MiB", "MiB");
  check(velin::formatByteSize(-1).empty(), "Negative size");
  check(velin::formatBitRate(12.34e6) == "12.3 Mbps", "Mbps");
  check(velin::formatBitRate(640e3) == "640 kbps", "kbps");
  check(velin::streamSourceLabel("https://user:pw@alist.example.com:5244/d/Movies/a.mkv?sign=secret") == "alist.example.com:5244 (https)",
        "Source shows only the host");
  check(velin::streamSourceLabel("C:\\Videos\\a.mkv") == "Local file", "Windows path is local");
  check(velin::streamSourceLabel("/var/mobile/a.mkv") == "Local file", "POSIX path is local");

  velin::StreamStatsSampler sampler;
  std::map<std::string, std::string> values{
    {"path", "\"https://alist.example.com/d/a.mkv?sign=secret\""},
    {"demuxer-via-network", "true"},
    {"cache-speed", "1500000"},
    {"cache-secs", "30.0"},
    {"demuxer-max-bytes", "157286400"},
    {"demuxer-cache-state", R"({"cache-duration":12.5,"fw-bytes":10485760,"total-bytes":20971520,"eof":false,"underrun":false,"idle":false})"},
    {"video-codec", "\"hevc (HEVC (High Efficiency Video Coding))\""},
    {"video-params", R"({"w":3840,"h":2160,"pixelformat":"yuv420p10","primaries":"bt.2020","gamma":"pq"})"},
    {"video-out-params", R"({"dw":1920,"dh":1080})"},
    {"hwdec-current", "\"videotoolbox\""},
    {"frame-drop-count", "3"},
    {"decoder-frame-drop-count", "0"},
    {"avsync", "0.004"},
    {"audio-codec-name", "\"eac3\""},
    {"audio-params", R"({"samplerate":48000,"hr-channels":"5.1","format":"floatp"})"},
    {"file-size", "null"}};
  auto stats = sampler.sample(values);
  auto find = [&](const std::string& label) {
    for (const auto& section : stats.sections)
      for (const auto& row : section.rows)
        if (row.label == label) return row.value;
    return std::string{};
  };
  check(stats.network, "Network stream");
  check(find("Source") == "alist.example.com (https)", "Source row hides the signature");
  check(find("Download") == "12.0 Mbps  (1.43 MiB/s)", "Download speed");
  check(find("Buffer ahead") == "12.5 s of 30 s", "Buffer ahead");
  check(find("Buffered") == "10.0 MiB of 150 MiB", "Buffered bytes");
  check(find("State") == "Reading", "Cache state");
  check(find("Resolution") == "3840\u00d72160  \u2192  1920\u00d71080", "Resolution");
  check(find("Decoder") == "videotoolbox (hardware)", "Hardware decoder");
  check(find("Codec") == "hevc", "Short codec");
  check(find("Dropped frames") == "3 output \u00b7 0 decoder", "Dropped frames");
  check(find("File size").empty(), "Null values are skipped");
  check(stats.speedHistory.size() == 1 && stats.speedHistory[0] == 1500000, "Speed history");

  values["paused-for-cache"] = "true";
  values["cache-buffering-state"] = "45";
  stats = sampler.sample(values);
  check(find("State") == "Buffering 45%", "Buffering state");
  check(stats.bufferHistory.size() == 2, "History grows");
  for (std::size_t i = 0; i < 100; ++i) stats = sampler.sample(values);
  check(stats.speedHistory.size() == velin::StreamStatsSampler::historyLength, "History is capped");
  values["path"] = "\"/local/b.mkv\"";
  values["demuxer-max-bytes"] = "\"150MiB\"";
  stats = sampler.sample(values);
  check(stats.speedHistory.size() == 1, "History restarts for a new file");
  check(find("Buffered") == "10.0 MiB of 150 MiB", "Byte-size option as text");
  check(find("Source") == "Local file", "Local source");
}

int main() {
  try {
    TestDirectory temporary;
    testTime();
    testStreamStats();
    testCenteredCrop();
    testUnicode();
    testTitlesAndExtensions();
    testDirectories(temporary.path);
    testRelated(temporary.path);
    testSettings(temporary.path);
    testIinaImport(temporary.path);
    std::cout << "All core tests passed.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Core test failure: " << error.what() << '\n';
    return 1;
  }
}
