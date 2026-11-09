// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
import SwiftUI
import UIKit
import VelinCore

struct PlayerView: View {
    @EnvironmentObject private var model: PlayerModel
    @EnvironmentObject private var settings: AppSettings
    @State private var controlsVisible = true
    @State private var hideTask: DispatchWorkItem?
    @State private var scrubbing: Double?           // Target while dragging the slider or swiping.
    @State private var gestureMode: GestureMode = .none
    @State private var gestureStartValue = 0.0
    @State private var feedback: String?            // Brief centre message: "+10s", "Volume 60%".
    @State private var feedbackTask: DispatchWorkItem?
    @State private var showingPlaylist = false
    @State private var landscape = false

    private enum GestureMode { case none, seek, volume, brightness }

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            VideoSurface(model: model).ignoresSafeArea()
            GestureLayer(onTap: toggleControls, onDoubleTap: doubleTap, onPan: pan, onPinch: pinch)
                .ignoresSafeArea()
            if let feedback {
                Text(feedback)
                    .font(.title2.weight(.semibold)).monospacedDigit()
                    .padding(.horizontal, 20).padding(.vertical, 12)
                    .background(.ultraThinMaterial, in: Capsule())
                    .transition(.opacity)
                    .allowsHitTesting(false)
            }
            if controlsVisible { controls.transition(.opacity) }
        }
        .statusBarHidden(!controlsVisible)
        .persistentSystemOverlays(controlsVisible ? .automatic : .hidden)
        .onGeometryChange(for: Bool.self) { $0.size.width > $0.size.height } action: { landscape = $0 }
        .onAppear { scheduleHide() }
        .onDisappear { Orientation.shared.release() }
        .onChange(of: model.paused) { _, paused in if paused { showControls(autoHide: false) } else { scheduleHide() } }
        .sheet(isPresented: $showingPlaylist) { playlistSheet }
        .animation(.easeInOut(duration: 0.2), value: controlsVisible)
        .animation(.easeInOut(duration: 0.15), value: feedback)
    }

    // MARK: Controls

    private var controls: some View {
        VStack(spacing: 0) {
            topBar
            Spacer()
            if model.loaded { centreButtons }
            Spacer()
            bottomBar
        }
        .padding(.horizontal, 16).padding(.vertical, 8)
        .background {
            LinearGradient(colors: [.black.opacity(0.6), .clear, .clear, .black.opacity(0.7)], startPoint: .top, endPoint: .bottom)
                .ignoresSafeArea().allowsHitTesting(false)
        }
    }

    private var topBar: some View {
        HStack(alignment: .top, spacing: 12) {
            Button { model.closePlayer() } label: {
                Image(systemName: "chevron.down").font(.title2.weight(.semibold)).frame(width: 44, height: 44)
            }
            VStack(alignment: .leading, spacing: 2) {
                Text(model.title).font(.headline).lineLimit(1)
                if !model.details.isEmpty { Text(model.details).font(.caption).foregroundStyle(.secondary).lineLimit(1) }
                if !model.errorMessage.isEmpty { Text(model.errorMessage).font(.caption).foregroundStyle(.red).lineLimit(2) }
            }
            .padding(.top, 4)
            Spacer()
            if model.playlist.count > 1 {
                Button { showingPlaylist = true } label: { Image(systemName: "list.bullet").font(.title3).frame(width: 44, height: 44) }
            }
            optionsMenu
        }
        .foregroundStyle(.white)
    }

    private var optionsMenu: some View {
        Menu {
            trackMenu("Audio", systemImage: "speaker.wave.2", type: "audio", allowOff: false)
            trackMenu("Subtitles", systemImage: "captions.bubble", type: "sub", allowOff: true)
            Menu {
                ForEach([0.5, 0.75, 1.0, 1.25, 1.5, 2.0], id: \.self) { value in
                    Button { model.setSpeed(value) } label: {
                        if abs(model.speed - value) < 0.01 { Label(speedText(value), systemImage: "checkmark") } else { Text(speedText(value)) }
                    }
                }
            } label: { Label("Speed \(speedText(model.speed))", systemImage: "gauge.with.dots.needle.67percent") }
            Button { model.zoomToFill.toggle() } label: {
                Label(model.zoomToFill ? "Fit to Screen" : "Fill Screen", systemImage: model.zoomToFill ? "minus.magnifyingglass" : "plus.magnifyingglass")
            }
        } label: {
            Image(systemName: "ellipsis.circle").font(.title3).frame(width: 44, height: 44)
        }
        .onTapGesture { showControls(autoHide: false) }
    }

    @ViewBuilder private func trackMenu(_ title: String, systemImage: String, type: String, allowOff: Bool) -> some View {
        let tracks = model.tracks.filter { $0.type == type }
        if !tracks.isEmpty {
            Menu {
                if allowOff {
                    Button { model.setProperty(type == "sub" ? "sid" : "aid", "no") } label: {
                        if !tracks.contains(where: \.isSelected) { Label("Off", systemImage: "checkmark") } else { Text("Off") }
                    }
                }
                ForEach(tracks, id: \.identifier) { track in
                    Button { model.select(track) } label: {
                        if track.isSelected { Label(trackName(track), systemImage: "checkmark") } else { Text(trackName(track)) }
                    }
                }
            } label: { Label(title, systemImage: systemImage) }
        }
    }

    private var centreButtons: some View {
        HStack(spacing: 48) {
            Button { model.playPrevious() } label: { Image(systemName: "backward.end.fill").font(.title2) }
                .opacity(model.playlistPosition > 0 ? 1 : 0).disabled(model.playlistPosition <= 0)
            Button { seekBy(-Double(settings.seekStep)) } label: { Image(systemName: "gobackward.\(skipSymbol)").font(.title) }
            Button { model.togglePause() } label: {
                Image(systemName: model.paused ? "play.fill" : "pause.fill").font(.system(size: 44)).frame(width: 64, height: 64)
            }
            Button { seekBy(Double(settings.seekStep)) } label: { Image(systemName: "goforward.\(skipSymbol)").font(.title) }
            Button { model.playNext() } label: { Image(systemName: "forward.end.fill").font(.title2) }
                .opacity(model.playlistPosition + 1 < model.playlist.count ? 1 : 0)
                .disabled(model.playlistPosition + 1 >= model.playlist.count)
        }
        .foregroundStyle(.white)
        .shadow(radius: 6)
    }

    private var bottomBar: some View {
        VStack(spacing: 4) {
            Slider(value: Binding(get: { scrubbing ?? model.position }, set: { scrubbing = $0; showControls(autoHide: false) }),
                   in: 0...max(model.duration, 1)) { editing in
                if !editing, let target = scrubbing { model.seek(to: target); scrubbing = nil; scheduleHide() }
            }
            .tint(.white)
            HStack(spacing: 12) {
                Text(timeText(scrubbing ?? model.position))
                Spacer()
                Text("-" + timeText(max(model.duration - (scrubbing ?? model.position), 0)))
                fullscreenButton
            }
            .font(.caption.monospacedDigit()).foregroundStyle(.white.opacity(0.85))
        }
        .disabled(!model.loaded)
    }

    // Turns the player to landscape, or back to portrait, even with the
    // rotation lock on.
    private var fullscreenButton: some View {
        Button {
            if landscape { Orientation.shared.enterPortrait() } else { Orientation.shared.enterLandscape() }
            scheduleHide()
        } label: {
            Image(systemName: landscape ? "arrow.down.right.and.arrow.up.left" : "arrow.up.left.and.arrow.down.right")
                .font(.body.weight(.semibold)).foregroundStyle(.white)
                .frame(width: 36, height: 36).contentShape(Rectangle())
        }
        .accessibilityLabel(landscape ? "Exit Full Screen" : "Full Screen")
    }

    private var playlistSheet: some View {
        NavigationStack {
            List(Array(model.playlist.enumerated()), id: \.offset) { index, entry in
                Button {
                    model.selectPlaylist(index)
                    showingPlaylist = false
                } label: {
                    HStack {
                        Text(entry.title).foregroundStyle(.primary).lineLimit(2)
                        Spacer()
                        if entry.isCurrent { Image(systemName: "speaker.wave.2.fill").foregroundStyle(.tint) }
                    }
                }
            }
            .navigationTitle("Playlist")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { Button("Done") { showingPlaylist = false } }
        }
        .presentationDetents([.medium, .large])
    }

    // MARK: Gestures

    private func toggleControls() {
        if controlsVisible { controlsVisible = false; hideTask?.cancel() } else { showControls(autoHide: true) }
    }

    private func doubleTap(_ x: CGFloat) {
        guard model.loaded else { return }
        if x < 0.35 { seekBy(-Double(settings.seekStep)) }
        else if x > 0.65 { seekBy(Double(settings.seekStep)) }
        else { model.togglePause() }
    }

    private func seekBy(_ seconds: Double) {
        model.seek(by: seconds)
        flash(seconds > 0 ? "+\(Int(seconds))s" : "\(Int(seconds))s")
    }

    // Horizontal swipes seek; vertical swipes set volume (right) or
    // brightness (left).
    private func pan(_ state: UIGestureRecognizer.State, _ start: CGPoint, _ translation: CGPoint, _ size: CGSize) {
        guard model.loaded, size.width > 0, size.height > 0 else { return }
        switch state {
        case .changed where gestureMode == .none:
            guard hypot(translation.x, translation.y) > 12 else { return }
            if abs(translation.x) > abs(translation.y) {
                gestureMode = .seek; gestureStartValue = model.position
            } else if start.x > size.width / 2 {
                gestureMode = .volume; gestureStartValue = model.volume
            } else {
                gestureMode = .brightness; gestureStartValue = UIScreen.main.brightness
            }
        case .changed:
            switch gestureMode {
            case .seek:
                // A full-width swipe covers 90 seconds, or the whole of a short file.
                let span = min(90, max(model.duration, 1))
                let target = min(max(gestureStartValue + Double(translation.x / size.width) * span, 0), model.duration)
                scrubbing = target
                showFeedback("\(timeText(target)) / \(timeText(model.duration))")
            case .volume:
                let value = min(max(gestureStartValue - Double(translation.y / size.height) * 120, 0), 100)
                model.setVolume(value)
                showFeedback("Volume \(Int(value))%")
            case .brightness:
                let value = min(max(gestureStartValue - Double(translation.y / size.height) * 1.2, 0), 1)
                UIScreen.main.brightness = value
                showFeedback("Brightness \(Int(value * 100))%")
            case .none: break
            }
        case .ended, .cancelled, .failed:
            if gestureMode == .seek, let target = scrubbing { model.seek(to: target) }
            scrubbing = nil
            gestureMode = .none
            hideFeedback(after: 0.6)
        default: break
        }
    }

    private func pinch(_ scale: CGFloat) {
        if scale > 1.15 && !model.zoomToFill { model.zoomToFill = true; flash("Fill Screen") }
        else if scale < 0.87 && model.zoomToFill { model.zoomToFill = false; flash("Fit to Screen") }
    }

    // MARK: Helpers

    private func showControls(autoHide: Bool) {
        controlsVisible = true
        if autoHide { scheduleHide() } else { hideTask?.cancel() }
    }

    private func scheduleHide() {
        hideTask?.cancel()
        guard !model.paused else { return }
        let task = DispatchWorkItem { controlsVisible = false }
        hideTask = task
        DispatchQueue.main.asyncAfter(deadline: .now() + 3, execute: task)
    }

    private func showFeedback(_ text: String) {
        feedbackTask?.cancel()
        feedback = text
    }

    private func hideFeedback(after delay: Double) {
        feedbackTask?.cancel()
        let task = DispatchWorkItem { feedback = nil }
        feedbackTask = task
        DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: task)
    }

    private func flash(_ text: String) {
        showFeedback(text)
        hideFeedback(after: 0.7)
    }

    private var skipSymbol: Int { [5, 10, 15, 30, 45, 60, 75, 90].contains(settings.seekStep) ? settings.seekStep : 10 }

    private func trackName(_ track: VLNTrack) -> String {
        let name = [track.title, track.language, track.codec].filter { !$0.isEmpty }.joined(separator: " · ")
        return name.isEmpty ? "Track \(track.identifier)" : name
    }

    private func speedText(_ value: Double) -> String {
        value == value.rounded() ? String(format: "%.0f×", value) : String(format: "%g×", value)
    }
}

func timeText(_ seconds: Double) -> String {
    let total = Int(max(seconds.isFinite ? seconds : 0, 0).rounded(.down))
    return total >= 3600 ? String(format: "%d:%02d:%02d", total / 3600, total / 60 % 60, total % 60)
                         : String(format: "%d:%02d", total / 60, total % 60)
}
