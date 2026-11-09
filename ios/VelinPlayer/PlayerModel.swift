// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
import Combine
import Foundation
import MediaPlayer
import UIKit
import VelinCore

// Owns the engine, the one video view it renders into, playback history and
// the AList connection. Everything runs on the main thread.
final class PlayerModel: NSObject, ObservableObject, VLNPlayerDelegate {
    @Published private(set) var running = false
    @Published private(set) var loaded = false
    @Published private(set) var paused = false
    @Published private(set) var position = 0.0
    @Published private(set) var duration = 0.0
    @Published private(set) var title = ""
    @Published private(set) var details = ""
    @Published private(set) var errorMessage = ""
    @Published private(set) var tracks: [VLNTrack] = []
    @Published private(set) var playlist: [VLNPlaylistEntry] = []
    @Published private(set) var playlistPosition = -1
    @Published private(set) var speed = 1.0
    @Published private(set) var recents: [VLNHistoryEntry] = []
    @Published var showPlayer = false { didSet { updateIdleTimer() } }
    @Published var zoomToFill = false { didSet { player.setProperty("panscan", value: zoomToFill ? "1.0" : "0.0") } }

    let settings: AppSettings
    let library = VLNLibrary()
    let alist = VLNAList()
    let videoView = VideoView(frame: .zero)

    private let player = VLNPlayer()
    private var scopedURLs: [URL] = []
    private var currentKey: String?
    private var lastRecorded = Date.distantPast
    private var lastLog = Date.distantPast
    private var settingsObserver: AnyCancellable?
    private var lifecycleObservers: [NSObjectProtocol] = []
    private lazy var nowPlaying = NowPlaying(model: self)

    init(settings: AppSettings) {
        self.settings = settings
        super.init()
        player.delegate = self
        videoView.onResize = { [weak self] in self?.player.layerDidResize() }
        player.subtitleLanguages = settings.subtitleLanguages
        var options = settings.mpvOptions
        options.merge(Self.launchOptions()) { _, launch in launch }
        do {
            try player.start(with: videoView.videoLayer, options: options)
            running = true
        } catch {
            errorMessage = error.localizedDescription
        }
        // Apply later changes to the running engine.
        settingsObserver = settings.objectWillChange.sink { [weak self] in
            DispatchQueue.main.async { self?.applySettings() }
        }
        refreshRecents()
        nowPlaying.activate()
        // GPU work from the background loses the Vulkan device, so drop the
        // video while there; audio and Now Playing carry on. Resume only once
        // active: during willEnterForeground the app is still in the
        // background state. Repeated calls (Control Center) do nothing.
        let center = NotificationCenter.default
        lifecycleObservers = [
            center.addObserver(forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: .main) { [weak self] _ in
                self?.enterBackground()
            },
            center.addObserver(forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main) { [weak self] _ in
                self?.player.isVideoSuspended = false
                print("VELIN video resumed")
            },
        ]
    }

    // Home Screen, app switcher or lock: pause first when the settings say so,
    // then stop drawing video.
    private func enterBackground() {
        let pausing = loaded && !paused && (!settings.playInBackground || (settings.pauseVideoInBackground && hasVideo))
        if pausing { pause() }
        player.isVideoSuspended = true
        print("VELIN video suspended for the background\(pausing ? ", paused" : "")")
    }

    // Whether the current file shows a picture; a cover image in a music
    // file does not count.
    var hasVideo: Bool { tracks.contains { $0.type == "video" && !$0.isAlbumArt } }

    private func applySettings() {
        player.subtitleLanguages = settings.subtitleLanguages
        for (name, value) in settings.mpvOptions { player.setProperty(name, value: value) }
        nowPlaying.updateSkipInterval()
    }

    // MARK: Opening

    func open(_ items: [VLNMediaItem], start: Int) {
        guard running, !items.isEmpty else { return }
        recordCurrent()
        for item in items {
            item.resumePosition = settings.resumePlayback ? resumePosition(for: item.historyKey) : 0
        }
        errorMessage = ""
        zoomToFill = false
        if player.open(items, start: start) { showPlayer = true }
    }

    // A file in Velin's Documents folder; its folder becomes the playlist.
    func openDocument(_ url: URL) {
        let folder = url.deletingLastPathComponent().standardizedFileURL
        let siblings = MediaSources.documentFiles().filter { $0.deletingLastPathComponent().standardizedFileURL == folder }
        let files = siblings.contains(where: { $0.standardizedFileURL == url.standardizedFileURL }) ? siblings : [url]
        let items = files.map { file -> VLNMediaItem in
            let item = VLNMediaItem(url: file, title: file.lastPathComponent)
            item.historyKey = MediaSources.documentKey(file)
            return item
        }
        open(items, start: files.firstIndex { $0.standardizedFileURL == url.standardizedFileURL } ?? 0)
    }

    // A file picked in the Files app, played in place.
    func openImported(_ url: URL) {
        if url.startAccessingSecurityScopedResource() { scopedURLs.append(url) }
        if url.standardizedFileURL.path.hasPrefix(MediaSources.documents.standardizedFileURL.path) { openDocument(url); return }
        let item = VLNMediaItem(url: url, title: url.lastPathComponent)
        item.historyKey = MediaSources.bookmarkKey(for: url) ?? url.absoluteString
        open([item], start: 0)
    }

    func openRemote(_ url: URL) {
        open([VLNMediaItem(url: url, title: url.lastPathComponent.removingPercentEncoding ?? url.absoluteString)], start: 0)
    }

    // AList: queue the file's folder, starting at the file.
    func openAList(path: String, completion: ((Error?) -> Void)? = nil) {
        let parent = (path as NSString).deletingLastPathComponent
        alist.listFolder(parent.isEmpty ? "/" : parent, refresh: false) { [weak self] folder, error in
            guard let self else { return }
            if let folder { self.openAList(folder: folder, path: path) }
            completion?(error)
        }
    }

    func openAList(folder: VLNAListFolder, path: String) {
        var start = 0
        guard let items = alist.playlist(for: folder, startingAt: path, start: &start) else {
            errorMessage = "That file is no longer in the folder."
            return
        }
        open(items, start: start)
    }

    // Reopens something from history. Returns false when it cannot be found.
    @discardableResult
    func openHistory(_ entry: VLNHistoryEntry, completion: ((Error?) -> Void)? = nil) -> Bool {
        let key = entry.key
        if let path = MediaSources.alistPath(fromKey: key) { openAList(path: path, completion: completion); return true }
        if let url = MediaSources.documentURL(forKey: key) { openDocument(url); return true }
        if key.hasPrefix("bookmark:"), let url = MediaSources.bookmarkURL(forKey: key) {
            if url.startAccessingSecurityScopedResource() { scopedURLs.append(url) }
            let item = VLNMediaItem(url: url, title: entry.title)
            item.historyKey = key
            open([item], start: 0)
            return true
        }
        if let url = URL(string: key), url.scheme == "http" || url.scheme == "https" { openRemote(url); return true }
        return false
    }

    func closePlayer() {
        recordCurrent()
        player.stop()
        currentKey = nil
        showPlayer = false
        for url in scopedURLs { url.stopAccessingSecurityScopedResource() }
        scopedURLs.removeAll()
        refreshRecents()
        nowPlaying.clear()
    }

    // MARK: Controls

    func togglePause() { player.togglePause() }
    func play() { if paused { player.togglePause() } }
    func pause() { if !paused { player.togglePause() } }
    func seek(by seconds: Double) { player.seek(by: seconds) }
    func seek(to seconds: Double) { player.seek(to: seconds) }
    func select(_ track: VLNTrack) { player.selectTrack(track) }
    func selectPlaylist(_ index: Int) { player.selectPlaylistIndex(index) }
    func playNext() { player.playNext() }
    func playPrevious() { player.playPrevious() }
    func setProperty(_ name: String, _ value: String) { player.setProperty(name, value: value) }
    func setSpeed(_ value: Double) { player.setProperty("speed", value: String(value)) }
    var volume: Double { player.volume }
    func setVolume(_ value: Double) { player.setProperty("volume", value: String(Int(min(max(value, 0), 100)))) }

    // MARK: History

    func refreshRecents() { recents = library.recentEntries() }

    func forget(_ entry: VLNHistoryEntry) {
        library.removeKey(entry.key)
        if entry.key.hasPrefix("bookmark:") { MediaSources.forgetBookmark(entry.key) }
        refreshRecents()
    }

    func clearHistory() { library.clear(); refreshRecents() }

    // Resume unless the file was barely started or already finished.
    private func resumePosition(for key: String) -> Double {
        guard let entry = library.entry(forKey: key), entry.position > 5 else { return 0 }
        if isFinished(position: entry.position, duration: entry.duration) { return 0 }
        return entry.position
    }

    private func recordCurrent() {
        guard let key = currentKey, let item = player.currentItem, item.historyKey == key, duration > 0 else { return }
        library.recordKey(key, title: item.title, position: position, duration: duration, countPlay: false)
    }

    // MARK: VLNPlayerDelegate

    func playerDidUpdate(_ player: VLNPlayer) {
        loaded = player.isLoaded
        paused = player.isPaused
        position = player.position
        duration = player.duration
        speed = player.speed
        title = player.currentItem?.title ?? player.title
        tracks = player.tracks
        playlist = player.playlist
        playlistPosition = player.playlistPosition
        if !player.errorMessage.isEmpty { errorMessage = player.errorMessage }
        var parts: [String] = []
        if player.videoWidth > 0 { parts.append("\(player.videoWidth)×\(player.videoHeight)") }
        if !player.videoCodec.isEmpty { parts.append(player.videoCodec.components(separatedBy: " (").first ?? player.videoCodec) }
        parts.append(player.hardwareDecoder.isEmpty ? "software decoding" : "hwdec \(player.hardwareDecoder)")
        if !player.audioCodec.isEmpty { parts.append(player.audioCodec) }
        details = parts.joined(separator: " · ")
        trackHistory()
        nowPlaying.update()
        updateIdleTimer()
        if Date().timeIntervalSince(lastLog) >= 1 {
            lastLog = Date()
            print(String(format: "VELIN state loaded=%d paused=%d position=%.2f duration=%.2f playlist=%d/%d %@ %@",
                         loaded ? 1 : 0, paused ? 1 : 0, position, duration, playlistPosition + 1, playlist.count,
                         details, errorMessage))
        }
    }

    // Keep the screen from dimming and locking while a video plays on the
    // player screen. Paused video, audio in the background and the home
    // screen let the phone lock as usual.
    private func updateIdleTimer() {
        let keepAwake = showPlayer && loaded && !paused && player.videoWidth > 0
        if UIApplication.shared.isIdleTimerDisabled != keepAwake {
            UIApplication.shared.isIdleTimerDisabled = keepAwake
        }
    }

    // Record each file once when it starts, then its position every few seconds.
    private func trackHistory() {
        guard loaded, duration > 0, let item = player.currentItem else { return }
        if item.historyKey != currentKey {
            currentKey = item.historyKey
            library.recordKey(item.historyKey, title: item.title, position: position, duration: duration, countPlay: true)
            lastRecorded = Date()
        } else if Date().timeIntervalSince(lastRecorded) >= 5 {
            library.recordKey(item.historyKey, title: item.title, position: position, duration: duration, countPlay: false)
            lastRecorded = Date()
        }
    }

    // MARK: Testing

    // "-mpv name=value" launch arguments set mpv options; "-open name" plays
    // Documents/name or a URL once the app starts.
    private static func launchOptions() -> [String: String] {
        var options: [String: String] = [:]
        let arguments = CommandLine.arguments
        for (index, argument) in arguments.enumerated() where argument == "-mpv" && index + 1 < arguments.count {
            let pair = arguments[index + 1].split(separator: "=", maxSplits: 1).map(String.init)
            if pair.count == 2 { options[pair[0]] = pair[1] }
        }
        return options
    }

    func openLaunchArgument() {
        let arguments = CommandLine.arguments
        guard let index = arguments.firstIndex(of: "-open"), index + 1 < arguments.count else { return }
        let value = arguments[index + 1]
        if let url = URL(string: value), url.scheme == "http" || url.scheme == "https" { openRemote(url) }
        else if value.hasPrefix("velin://"), let url = URL(string: value) { handleLink(url) }
        else { openDocument(MediaSources.documents.appendingPathComponent(value)) }
    }

    // velin://alist?path=<AList path> plays a file with its folder queued.
    // Returns the folder path when the link names a folder to browse.
    @discardableResult
    func handleLink(_ url: URL) -> String? {
        guard let path = MediaSources.alistPath(fromKey: url.absoluteString) else { return nil }
        if MediaSources.isMedia(URL(fileURLWithPath: path)) { openAList(path: path); return nil }
        return path
    }
}
