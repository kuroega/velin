// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
import SwiftUI

struct SettingsView: View {
    @EnvironmentObject private var model: PlayerModel
    @EnvironmentObject private var settings: AppSettings
    @State private var confirmingClear = false

    var body: some View {
        Form {
            Section("Playback") {
                Toggle("Resume where I left off", isOn: $settings.resumePlayback)
                Toggle("Play the next file automatically", isOn: $settings.autoPlayNext)
                Picker("Skip interval", selection: $settings.seekStep) {
                    ForEach([5, 10, 15, 30], id: \.self) { Text("\($0) seconds").tag($0) }
                }
            }
            Section {
                Toggle("Keep playing audio in the background", isOn: $settings.playInBackground)
                Toggle("Pause videos when I leave the app", isOn: $settings.pauseVideoInBackground)
                    .disabled(!settings.playInBackground)
            } header: { Text("Background") } footer: {
                Text(settings.playInBackground
                     ? "Leaving the app means going to the Home Screen, switching apps or locking the phone. Audio files keep playing either way."
                     : "Everything pauses when you leave the app.")
            }
            Section {
                TextField("Languages", text: $settings.subtitleLanguages)
                    .textInputAutocapitalization(.never).autocorrectionDisabled()
            } header: { Text("Subtitle languages") } footer: {
                Text("In order of preference, comma-separated, e.g. zh-Hans,chi,eng. Used for embedded tracks and for AList subtitle files.")
            }
            Section {
                Toggle("Hardware decoding", isOn: $settings.hardwareDecoding)
            } header: { Text("Video") } footer: {
                Text("Decodes H.264, HEVC and other common formats with VideoToolbox, using much less battery.")
            }
            Section("AList") {
                if model.alist.isSignedIn {
                    LabeledContent("Signed in", value: "\(model.alist.username) · \(model.alist.server)")
                    Button("Sign Out", role: .destructive) { model.alist.signOut(); model.objectWillChange.send() }
                } else {
                    Text("Not signed in").foregroundStyle(.secondary)
                }
            }
            Section {
                Button("Clear Playback History", role: .destructive) { confirmingClear = true }
            }
            Section("About") {
                LabeledContent("Version", value: Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "")
                Text("Velin is free software under the GNU GPL v3. Playback by mpv and FFmpeg.")
                    .font(.footnote).foregroundStyle(.secondary)
            }
        }
        .navigationTitle("Settings")
        .confirmationDialog("Clear all playback history?", isPresented: $confirmingClear, titleVisibility: .visible) {
            Button("Clear History", role: .destructive) { model.clearHistory() }
        }
    }
}
