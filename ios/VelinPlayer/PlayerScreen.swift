// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
import SwiftUI
import UniformTypeIdentifiers
import VelinCore

struct PlayerScreen: View {
    @EnvironmentObject private var model: PlayerModel
    @State private var importing = false
    @State private var showingFiles = false
    @State private var scrubbing: Double?

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            VideoSurface(model: model).ignoresSafeArea()
            VStack(spacing: 0) {
                header
                Spacer()
                if !model.loaded { emptyState }
                Spacer()
                controls
            }
            .padding(16)
        }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.movie, .audio, .data]) { result in
            if case .success(let url) = result { model.open(url) }
        }
        .sheet(isPresented: $showingFiles) { documentList }
    }

    private var header: some View {
        HStack(alignment: .top) {
            VStack(alignment: .leading, spacing: 4) {
                Text(model.title.isEmpty ? "Velin" : model.title).font(.headline).lineLimit(1)
                if !model.details.isEmpty { Text(model.details).font(.caption).foregroundStyle(.secondary) }
                if !model.errorMessage.isEmpty { Text(model.errorMessage).font(.caption).foregroundStyle(.red) }
            }
            Spacer()
            Menu {
                Button("Open from Files…", systemImage: "folder") { importing = true }
                Button("Velin Documents", systemImage: "doc.on.doc") { model.refreshDocuments(); showingFiles = true }
                trackMenu("Audio", type: "audio")
                trackMenu("Subtitles", type: "sub")
            } label: {
                Image(systemName: "ellipsis.circle").font(.title2)
            }
        }
        .padding(12)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
    }

    @ViewBuilder private func trackMenu(_ title: String, type: String) -> some View {
        let tracks = model.tracks.filter { $0.type == type }
        if !tracks.isEmpty {
            Menu(title) {
                ForEach(tracks, id: \.identifier) { track in
                    Button {
                        model.select(track)
                    } label: {
                        let name = [track.title, track.language, track.codec].filter { !$0.isEmpty }.joined(separator: " · ")
                        if track.isSelected { Label(name.isEmpty ? "Track \(track.identifier)" : name, systemImage: "checkmark") }
                        else { Text(name.isEmpty ? "Track \(track.identifier)" : name) }
                    }
                }
            }
        }
    }

    private var emptyState: some View {
        VStack(spacing: 12) {
            Text("Open a video to start").font(.title3)
            Button("Open from Files…") { importing = true }.buttonStyle(.borderedProminent)
        }
    }

    private var controls: some View {
        VStack(spacing: 8) {
            Slider(value: Binding(get: { scrubbing ?? model.position }, set: { scrubbing = $0 }),
                   in: 0...max(model.duration, 1)) { editing in
                if !editing, let target = scrubbing { model.seek(to: target); scrubbing = nil }
            }
            HStack {
                Text(timeText(scrubbing ?? model.position)).monospacedDigit()
                Spacer()
                Button { model.seek(by: -10) } label: { Image(systemName: "gobackward.10") }
                Button { model.togglePause() } label: {
                    Image(systemName: model.paused ? "play.fill" : "pause.fill").font(.title)
                }
                .padding(.horizontal, 24)
                Button { model.seek(by: 10) } label: { Image(systemName: "goforward.10") }
                Spacer()
                Text(timeText(model.duration)).monospacedDigit()
            }
            .font(.title3)
        }
        .padding(12)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
        .disabled(!model.loaded)
    }

    private var documentList: some View {
        NavigationStack {
            List(model.documents, id: \.self) { url in
                Button(url.lastPathComponent) { showingFiles = false; model.open(url) }
            }
            .overlay { if model.documents.isEmpty { Text("Copy videos into Velin with the Files app.").foregroundStyle(.secondary) } }
            .navigationTitle("Velin Documents")
            .toolbar { Button("Done") { showingFiles = false } }
        }
    }

    private func timeText(_ seconds: Double) -> String {
        let total = Int(max(seconds, 0).rounded(.down))
        return total >= 3600 ? String(format: "%d:%02d:%02d", total / 3600, total / 60 % 60, total % 60)
                             : String(format: "%d:%02d", total / 60, total % 60)
    }
}
