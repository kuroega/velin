// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace velin {

struct HistoryEntry {
  std::string path;
  std::string title;
  std::int64_t lastPlayed = 0;  // Seconds since Unix epoch, UTC.
  double position = 0;
  double duration = 0;
  std::uint64_t playCount = 0;
  bool favorite = false;
};
enum class HistorySort { Recent, Title, MostPlayed, Duration };

class HistoryStore {
 public:
  explicit HistoryStore(std::filesystem::path directory);
  std::vector<HistoryEntry> load() const;
  bool save(const std::vector<HistoryEntry>& entries) const;
  bool record(const HistoryEntry& entry, bool countPlay = true);
  std::vector<HistoryEntry> search(std::string_view query,
      HistorySort sort = HistorySort::Recent, bool favoritesOnly = false) const;
  bool remove(std::string_view path);
  bool setFavorite(std::string_view path, bool favorite);
  bool clear(bool keepFavorites = false);
  bool pruneBefore(std::int64_t timestamp, bool keepFavorites = true);
  const std::string& error() const;
 private:
  std::filesystem::path directory_;
  mutable std::string error_;
};

struct PlaylistEntry {
  std::string path;
  std::string title;
  double duration = -1;
};
struct PlaylistDocument {
  std::vector<PlaylistEntry> entries;
  std::int64_t currentIndex = -1;
  double position = 0;
};

class PlaylistStore {
 public:
  explicit PlaylistStore(std::filesystem::path directory);
  PlaylistDocument load(const std::filesystem::path& file) const;
  bool save(const std::filesystem::path& file, const PlaylistDocument& document) const;
  PlaylistDocument loadSession() const;
  bool saveSession(const PlaylistDocument& document) const;
  const std::string& error() const;
 private:
  std::filesystem::path directory_;
  mutable std::string error_;
};

// Same-directory atomic replacement; Windows flushes contents before replacement.
bool writeTextFileAtomic(const std::filesystem::path& file, std::string_view text,
                         std::string& error);

}  // namespace velin
