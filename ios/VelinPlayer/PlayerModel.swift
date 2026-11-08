// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
import Foundation
import QuartzCore
import VelinCore

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
    @Published private(set) var documents: [URL] = []

    private let player = VLNPlayer()
    private var scopedURL: URL?
    private var lastLog = Date.distantPast

    override init() {
        super.init()
        player.delegate = self
        refreshDocuments()
    }

    // Called once the video layer exists.
    func attach(layer: CAMetalLayer) {
        guard !player.isRunning else { return }
        do {
            try player.start(with: layer, options: launchOptions())
            running = true
            openLaunchArgument()
        } catch {
            errorMessage = error.localizedDescription
            print("VELIN error \(errorMessage)")
        }
    }

    func open(_ url: URL) {
        scopedURL?.stopAccessingSecurityScopedResource()
        scopedURL = url.startAccessingSecurityScopedResource() ? url : nil
        errorMessage = ""
        print("VELIN open \(url.isFileURL ? url.lastPathComponent : url.absoluteString)")
        player.open([url], append: false)
    }

    func togglePause() { player.togglePause() }
    func seek(by seconds: Double) { player.seek(by: seconds) }
    func seek(to seconds: Double) { player.seek(to: seconds) }
    func select(_ track: VLNTrack) { player.selectTrack(track) }

    func refreshDocuments() {
        let folder = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        let files = (try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)) ?? []
        documents = files.filter { !$0.hasDirectoryPath }.sorted { $0.lastPathComponent < $1.lastPathComponent }
    }

    // Each "-mpv name=value" launch argument sets an mpv option (for testing).
    private func launchOptions() -> [String: String] {
        var options: [String: String] = [:]
        let arguments = CommandLine.arguments
        for (index, argument) in arguments.enumerated() where argument == "-mpv" && index + 1 < arguments.count {
            let pair = arguments[index + 1].split(separator: "=", maxSplits: 1).map(String.init)
            if pair.count == 2 { options[pair[0]] = pair[1] }
        }
        return options
    }

    // "-open name" plays Documents/name, or a URL, at launch (for testing).
    private func openLaunchArgument() {
        let arguments = CommandLine.arguments
        guard let index = arguments.firstIndex(of: "-open"), index + 1 < arguments.count else { return }
        let value = arguments[index + 1]
        if let url = URL(string: value), url.scheme == "http" || url.scheme == "https" {
            open(url)
        } else {
            let folder = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            open(folder.appendingPathComponent(value))
        }
    }

    func playerDidUpdate(_ player: VLNPlayer) {
        loaded = player.isLoaded
        paused = player.isPaused
        position = player.position
        duration = player.duration
        title = player.title
        tracks = player.tracks
        let message = player.errorMessage
        if !message.isEmpty { errorMessage = message }
        var parts: [String] = []
        if player.videoWidth > 0 { parts.append("\(player.videoWidth)×\(player.videoHeight)") }
        if !player.videoCodec.isEmpty { parts.append(player.videoCodec) }
        parts.append(player.hardwareDecoder.isEmpty ? "software decoding" : "hwdec \(player.hardwareDecoder)")
        if !player.audioCodec.isEmpty { parts.append(player.audioCodec) }
        details = parts.joined(separator: " · ")
        if Date().timeIntervalSince(lastLog) >= 1 {
            lastLog = Date()
            print(String(format: "VELIN state loaded=%d paused=%d position=%.2f duration=%.2f %@ %@",
                         loaded ? 1 : 0, paused ? 1 : 0, position, duration, details, errorMessage))
        }
    }
}
