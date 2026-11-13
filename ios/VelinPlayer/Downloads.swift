// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
import Foundation
import VelinCore

// Saves remote media (AList files, URLs) into Documents/Downloads, where it
// plays offline and shows up in the Files app. Runs on a background URL
// session, so downloads carry on after the app leaves the screen and pick
// back up after a relaunch. AList subtitles come along, named after the video
// so the engine's fuzzy sub-auto loads them. Everything runs on the main thread.
final class DownloadManager: NSObject, ObservableObject, URLSessionDownloadDelegate {
    static let shared = DownloadManager()
    static let sessionIdentifier = "app.velin.player.downloads"

    struct Job: Identifiable, Equatable {
        enum State: Equatable { case waiting, running, failed(String) }
        let id: String
        var name: String
        var source: String          // History key of what is being downloaded.
        var received: Int64 = 0
        var expected: Int64 = 0     // 0 when unknown.
        var state: State = .waiting
        var fraction: Double? { expected > 0 ? min(Double(received) / Double(expected), 1) : nil }
    }

    // What each task is for, kept in its taskDescription so it survives relaunches.
    private struct Meta: Codable {
        var job: String
        var kind: String            // "video" or "subtitle".
        var name: String            // File name to save as.
        var source: String
        var url: String
        var expected: Int64
    }

    @Published private(set) var jobs: [Job] = []
    // Goes up each time a video lands in Downloads, for views to reload.
    @Published private(set) var finishedCount = 0
    // Set by the app delegate when iOS relaunches Velin for session events.
    var backgroundCompletion: (() -> Void)?
    // A finished video: its new file and the history key it came from.
    var onFinished: ((URL, String) -> Void)?

    private var session: URLSession!
    private var resumeData: [String: Data] = [:]
    private var metas: [String: Meta] = [:]     // Job id → its video's Meta, for retries.
    private var lastPublish: [String: CFTimeInterval] = [:]
    private let sourcesKey = "downloadedSources" // History key → "Documents/..." key.

    static var folder: URL {
        let url = MediaSources.documents.appendingPathComponent("Downloads", isDirectory: true)
        if !FileManager.default.fileExists(atPath: url.path) {
            try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
            var values = URLResourceValues()
            values.isExcludedFromBackup = true  // Large and re-downloadable.
            var folder = url
            try? folder.setResourceValues(values)
        }
        return url
    }

    private override init() {
        super.init()
        let configuration = URLSessionConfiguration.background(withIdentifier: Self.sessionIdentifier)
        configuration.sessionSendsLaunchEvents = true
        configuration.isDiscretionary = false
        configuration.allowsCellularAccess = true
        session = URLSession(configuration: configuration, delegate: self, delegateQueue: .main)
        restore()
    }

    // Picks up tasks still running from an earlier launch.
    private func restore() {
        session.getAllTasks { [weak self] tasks in
            DispatchQueue.main.async {
                guard let self else { return }
                for task in tasks {
                    guard let meta = self.meta(of: task) else { task.cancel(); continue }
                    guard meta.kind == "video" else { continue }
                    self.metas[meta.job] = meta
                    if !self.jobs.contains(where: { $0.id == meta.job }) {
                        var job = Job(id: meta.job, name: meta.name, source: meta.source, expected: meta.expected)
                        job.received = task.countOfBytesReceived
                        if task.countOfBytesExpectedToReceive > 0 { job.expected = task.countOfBytesExpectedToReceive }
                        job.state = task.state == .running ? .running : .waiting
                        self.jobs.append(job)
                    }
                }
            }
        }
    }

    // MARK: Starting

    // Queues an item with its subtitles. expectedSize (when known) is checked
    // against free space. Returns a message when it cannot start.
    @discardableResult
    func download(_ item: VLNMediaItem, expectedSize: Int64 = 0) -> String? {
        guard item.url.scheme == "http" || item.url.scheme == "https" else { return "\(item.title) is already on this iPhone." }
        if jobs.contains(where: { $0.source == item.historyKey && !isFailed($0) }) {
            return "\(item.title) is already downloading."
        }
        if downloadedFile(for: item.historyKey) != nil { return "\(item.title) is already downloaded." }
        if expectedSize > 0, let free = Self.freeSpace, expectedSize > free {
            return "Not enough space for \(item.title): it needs \(Self.bytes(expectedSize)), \(Self.bytes(free)) is free."
        }
        let name = Self.safeName(item.title, fallback: item.url.lastPathComponent)
        let id = UUID().uuidString
        let meta = Meta(job: id, kind: "video", name: name, source: item.historyKey, url: item.url.absoluteString, expected: expectedSize)
        metas[id] = meta
        jobs.append(Job(id: id, name: name, source: item.historyKey, expected: expectedSize))
        start(meta)
        // Subtitles: "<video name>.<subtitle file name>" unless already named after it.
        let base = (name as NSString).deletingPathExtension
        for subtitle in item.subtitleURLs {
            let file = Self.safeName(subtitle.lastPathComponent.removingPercentEncoding ?? subtitle.lastPathComponent, fallback: "subtitle.srt")
            let subName = file.hasPrefix(base) ? file : base + "." + file
            start(Meta(job: id, kind: "subtitle", name: subName, source: item.historyKey, url: subtitle.absoluteString, expected: 0))
        }
        return nil
    }

    // Every item, skipping those already here or on their way. Returns a
    // summary when something was skipped or nothing was queued.
    func downloadAll(_ items: [VLNMediaItem], sizes: [String: Int64]) -> String? {
        let pending = items.filter { item in
            downloadedFile(for: item.historyKey) == nil && !jobs.contains { $0.source == item.historyKey && !isFailed($0) }
        }
        guard !pending.isEmpty else { return "Everything here is already downloaded." }
        let total = pending.reduce(Int64(0)) { $0 + (sizes[$1.historyKey] ?? 0) }
        if total > 0, let free = Self.freeSpace, total > free {
            return "Not enough space: these \(pending.count) files need \(Self.bytes(total)), \(Self.bytes(free)) is free."
        }
        for item in pending { download(item, expectedSize: sizes[item.historyKey] ?? 0) }
        return nil
    }

    private func start(_ meta: Meta, resumingWith data: Data? = nil) {
        guard let url = URL(string: meta.url) else { return }
        let task = data.map { session.downloadTask(withResumeData: $0) } ?? session.downloadTask(with: url)
        task.taskDescription = (try? JSONEncoder().encode(meta)).flatMap { String(data: $0, encoding: .utf8) }
        task.resume()
    }

    // MARK: Managing

    func cancel(_ job: Job) {
        session.getAllTasks { [weak self] tasks in
            DispatchQueue.main.async {
                guard let self else { return }
                for task in tasks where self.meta(of: task)?.job == job.id { task.cancel() }
                self.remove(job.id)
            }
        }
    }

    func retry(_ job: Job) {
        guard let meta = metas[job.id] else { remove(job.id); return }
        update(job.id) { $0.state = .waiting }
        start(meta, resumingWith: resumeData.removeValue(forKey: job.id))
    }

    func dismiss(_ job: Job) { remove(job.id) }

    private func remove(_ id: String) {
        jobs.removeAll { $0.id == id }
        metas[id] = nil
        resumeData[id] = nil
        lastPublish[id] = nil
    }

    private func isFailed(_ job: Job) -> Bool { if case .failed = job.state { return true }; return false }

    func job(for source: String) -> Job? { jobs.first { $0.source == source } }

    // The downloaded copy of something, while it is still there.
    func downloadedFile(for source: String) -> URL? {
        guard let key = (UserDefaults.standard.dictionary(forKey: sourcesKey) as? [String: String])?[source] else { return nil }
        return MediaSources.documentURL(forKey: key)
    }

    // Deletes a file in Documents with the subtitles saved for it.
    func deleteFile(_ url: URL) {
        let base = url.deletingPathExtension().lastPathComponent + "."
        let folder = url.deletingLastPathComponent()
        let siblings = (try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)) ?? []
        for sibling in siblings where sibling.lastPathComponent.hasPrefix(base)
            && Self.subtitleExtensions.contains(sibling.pathExtension.lowercased()) {
            try? FileManager.default.removeItem(at: sibling)
        }
        try? FileManager.default.removeItem(at: url)
        let key = MediaSources.documentKey(url)
        var sources = UserDefaults.standard.dictionary(forKey: sourcesKey) as? [String: String] ?? [:]
        sources = sources.filter { $0.value != key }
        UserDefaults.standard.set(sources, forKey: sourcesKey)
    }

    // MARK: URLSessionDownloadDelegate

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData bytesWritten: Int64,
                    totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        guard let meta = meta(of: downloadTask), meta.kind == "video" else { return }
        let now = CACurrentMediaTime()
        if let last = lastPublish[meta.job], now - last < 0.5 { return }
        lastPublish[meta.job] = now
        update(meta.job) {
            $0.state = .running
            $0.received = totalBytesWritten
            if totalBytesExpectedToWrite > 0 { $0.expected = totalBytesExpectedToWrite }
        }
    }

    // The file must be moved before this returns.
    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        guard let meta = meta(of: downloadTask) else { return }
        if let http = downloadTask.response as? HTTPURLResponse, !(200..<300).contains(http.statusCode) {
            if meta.kind == "video" { update(meta.job) { $0.state = .failed("The server answered \(http.statusCode).") } }
            return
        }
        var name = meta.name
        if meta.kind == "video", !MediaSources.isMedia(URL(fileURLWithPath: name)),
           let suggested = downloadTask.response?.suggestedFilename, MediaSources.isMedia(URL(fileURLWithPath: suggested)) {
            name = Self.safeName(suggested, fallback: name)
        }
        let destination = Self.uniqueURL(in: Self.folder, name: name)
        do {
            try FileManager.default.moveItem(at: location, to: destination)
        } catch {
            if meta.kind == "video" { update(meta.job) { $0.state = .failed(error.localizedDescription) } }
            return
        }
        guard meta.kind == "video" else { return }
        var sources = UserDefaults.standard.dictionary(forKey: sourcesKey) as? [String: String] ?? [:]
        sources[meta.source] = MediaSources.documentKey(destination)
        UserDefaults.standard.set(sources, forKey: sourcesKey)
        remove(meta.job)
        finishedCount += 1
        onFinished?(destination, meta.source)
        print("VELIN download finished \(destination.lastPathComponent)")
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        guard let error, let meta = meta(of: task), meta.kind == "video" else { return }
        let nsError = error as NSError
        if nsError.domain == NSURLErrorDomain && nsError.code == NSURLErrorCancelled
            && nsError.userInfo[NSURLSessionDownloadTaskResumeData] == nil { return }  // Cancelled by the user.
        if let data = nsError.userInfo[NSURLSessionDownloadTaskResumeData] as? Data { resumeData[meta.job] = data }
        if metas[meta.job] == nil { metas[meta.job] = meta }
        if !jobs.contains(where: { $0.id == meta.job }) {
            jobs.append(Job(id: meta.job, name: meta.name, source: meta.source, expected: meta.expected))
        }
        update(meta.job) { $0.state = .failed(error.localizedDescription) }
        print("VELIN download failed \(meta.name): \(error.localizedDescription)")
    }

    func urlSessionDidFinishEvents(forBackgroundURLSession session: URLSession) {
        DispatchQueue.main.async { [weak self] in
            self?.backgroundCompletion?()
            self?.backgroundCompletion = nil
        }
    }

    // MARK: Helpers

    private func meta(of task: URLSessionTask) -> Meta? {
        task.taskDescription.flatMap { $0.data(using: .utf8) }.flatMap { try? JSONDecoder().decode(Meta.self, from: $0) }
    }

    private func update(_ id: String, _ change: (inout Job) -> Void) {
        guard let index = jobs.firstIndex(where: { $0.id == id }) else { return }
        var job = jobs[index]
        change(&job)
        if job != jobs[index] { jobs[index] = job }
    }

    static let subtitleExtensions: Set<String> = ["srt", "ass", "ssa", "vtt", "sub", "idx", "sup", "smi"]

    static var freeSpace: Int64? {
        let values = try? MediaSources.documents.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
        return values?.volumeAvailableCapacityForImportantUsage
    }

    static func bytes(_ count: Int64) -> String { ByteCountFormatter.string(fromByteCount: count, countStyle: .file) }

    private static func safeName(_ name: String, fallback: String) -> String {
        let cleaned = name.components(separatedBy: CharacterSet(charactersIn: "/\\:\0")).joined(separator: "_")
            .trimmingCharacters(in: .whitespacesAndNewlines)
        if cleaned.isEmpty || cleaned == "." || cleaned == ".." { return fallback.isEmpty ? "download" : fallback }
        return cleaned
    }

    // "name.mkv", or "name (2).mkv" when that is taken.
    private static func uniqueURL(in folder: URL, name: String) -> URL {
        var url = folder.appendingPathComponent(name)
        let base = (name as NSString).deletingPathExtension
        let ext = (name as NSString).pathExtension
        var n = 2
        while FileManager.default.fileExists(atPath: url.path) {
            url = folder.appendingPathComponent(ext.isEmpty ? "\(base) (\(n))" : "\(base) (\(n)).\(ext)")
            n += 1
        }
        return url
    }
}
