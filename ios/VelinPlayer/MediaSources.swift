// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
import Foundation
import VelinCore

// Where things play from, and the stable keys Velin's history uses for them:
//   "Documents/<path>"        a file in Velin's own Documents folder
//   "bookmark:<uuid>"         a file opened in place from the Files app
//   "velin://alist?path=..."  an AList file (the same link Windows opens)
//   "https://..."             any other URL
enum MediaSources {
    static let mediaExtensions: Set<String> = [
        "mkv", "mp4", "m4v", "mov", "avi", "webm", "ts", "m2ts", "mts", "flv", "wmv", "mpg", "mpeg", "3gp", "ogv", "rmvb",
        "mp3", "m4a", "aac", "flac", "wav", "ogg", "opus", "wma", "ape", "alac",
    ]

    static var documents: URL { FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0] }

    static func isMedia(_ url: URL) -> Bool { mediaExtensions.contains(url.pathExtension.lowercased()) }

    // Media files in Velin's Documents folder (and its subfolders), by name.
    static func documentFiles() -> [URL] {
        guard let walker = FileManager.default.enumerator(at: documents, includingPropertiesForKeys: [.isRegularFileKey],
                                                          options: [.skipsHiddenFiles]) else { return [] }
        return walker.compactMap { $0 as? URL }.filter(isMedia)
            .sorted { $0.lastPathComponent.localizedStandardCompare($1.lastPathComponent) == .orderedAscending }
    }

    static func documentKey(_ url: URL) -> String {
        let base = documents.standardizedFileURL.path
        let path = url.standardizedFileURL.path
        return path.hasPrefix(base) ? "Documents" + path.dropFirst(base.count) : url.absoluteString
    }

    static func documentURL(forKey key: String) -> URL? {
        guard key.hasPrefix("Documents/") else { return nil }
        let url = documents.appendingPathComponent(String(key.dropFirst("Documents/".count)))
        return FileManager.default.fileExists(atPath: url.path) ? url : nil
    }

    // Files opened in place from the Files app are remembered as bookmarks.
    private static let bookmarksKey = "fileBookmarks"

    static func bookmarkKey(for url: URL) -> String? {
        guard let data = try? url.bookmarkData(options: .minimalBookmark) else { return nil }
        var bookmarks = UserDefaults.standard.dictionary(forKey: bookmarksKey) as? [String: Data] ?? [:]
        if let existing = bookmarks.first(where: { $0.value == data })?.key { return existing }
        let key = "bookmark:" + UUID().uuidString
        bookmarks[key] = data
        UserDefaults.standard.set(bookmarks, forKey: bookmarksKey)
        return key
    }

    static func bookmarkURL(forKey key: String) -> URL? {
        guard let data = (UserDefaults.standard.dictionary(forKey: bookmarksKey) as? [String: Data])?[key] else { return nil }
        var stale = false
        return try? URL(resolvingBookmarkData: data, bookmarkDataIsStale: &stale)
    }

    static func forgetBookmark(_ key: String) {
        var bookmarks = UserDefaults.standard.dictionary(forKey: bookmarksKey) as? [String: Data] ?? [:]
        bookmarks[key] = nil
        UserDefaults.standard.set(bookmarks, forKey: bookmarksKey)
    }

    // The AList path inside "velin://alist?path=...", if key is one.
    static func alistPath(fromKey key: String) -> String? {
        guard let components = URLComponents(string: key), components.scheme == "velin", components.host == "alist" else { return nil }
        return components.queryItems?.first(where: { $0.name == "path" })?.value
    }
}
