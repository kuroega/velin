// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
import Foundation

// User preferences, stored in UserDefaults.
final class AppSettings: ObservableObject {
    private let defaults = UserDefaults.standard

    // mpv's slang list: which external or embedded subtitle to pick first.
    @Published var subtitleLanguages: String { didSet { defaults.set(subtitleLanguages, forKey: "subtitleLanguages") } }
    @Published var hardwareDecoding: Bool { didSet { defaults.set(hardwareDecoding, forKey: "hardwareDecoding") } }
    @Published var seekStep: Int { didSet { defaults.set(seekStep, forKey: "seekStep") } }
    @Published var resumePlayback: Bool { didSet { defaults.set(resumePlayback, forKey: "resumePlayback") } }
    @Published var autoPlayNext: Bool { didSet { defaults.set(autoPlayNext, forKey: "autoPlayNext") } }
    @Published var playInBackground: Bool { didSet { defaults.set(playInBackground, forKey: "playInBackground") } }
    // Leaving the app pauses files with video; audio-only files keep playing.
    @Published var pauseVideoInBackground: Bool { didSet { defaults.set(pauseVideoInBackground, forKey: "pauseVideoInBackground") } }

    init() {
        defaults.register(defaults: [
            "subtitleLanguages": "zh-Hans,zh,chi,eng,en",
            "hardwareDecoding": true,
            "seekStep": 10,
            "resumePlayback": true,
            "autoPlayNext": true,
            "playInBackground": true,
            "pauseVideoInBackground": true,
        ])
        subtitleLanguages = defaults.string(forKey: "subtitleLanguages") ?? ""
        hardwareDecoding = defaults.bool(forKey: "hardwareDecoding")
        seekStep = defaults.integer(forKey: "seekStep")
        resumePlayback = defaults.bool(forKey: "resumePlayback")
        autoPlayNext = defaults.bool(forKey: "autoPlayNext")
        playInBackground = defaults.bool(forKey: "playInBackground")
        pauseVideoInBackground = defaults.bool(forKey: "pauseVideoInBackground")
    }

    // mpv options matching these settings.
    var mpvOptions: [String: String] {
        [
            "slang": subtitleLanguages,
            "hwdec": hardwareDecoding ? "videotoolbox" : "no",
            // "always" stops at the end of each file instead of moving on.
            "keep-open": autoPlayNext ? "yes" : "always",
        ]
    }
}
