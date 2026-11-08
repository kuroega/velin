// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
import AVFoundation
import SwiftUI
import VelinCore

@main
struct VelinPlayerApp: App {
    @StateObject private var model = PlayerModel()

    init() {
        // Keep playing with the ringer switch off and in the background.
        try? AVAudioSession.sharedInstance().setCategory(.playback, mode: .moviePlayback)
        try? AVAudioSession.sharedInstance().setActive(true)
        if CommandLine.arguments.contains("-diagnostics") {
            for line in VLNDiagnostics.runSecretStoreCheck() { print("VELIN diagnostics \(line)") }
        }
    }

    var body: some Scene {
        WindowGroup {
            PlayerScreen()
                .environmentObject(model)
                .preferredColorScheme(.dark)
        }
    }
}
