// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
import MediaPlayer

// Lock-screen, Control Center and headphone controls.
final class NowPlaying {
    private unowned let model: PlayerModel
    private var lastTitle = ""
    // What the system was last told, and when.
    private var sent: Sent?

    private struct Sent: Equatable {
        var title: String, duration: Double, rate: Double, queueIndex: Int, queueCount: Int
        var position = 0.0, time = 0.0

        // The system advances the elapsed time by itself from the rate, so a
        // new position is only worth sending after a seek or a stall.
        func covers(_ other: Sent) -> Bool {
            title == other.title && duration == other.duration && rate == other.rate
                && queueIndex == other.queueIndex && queueCount == other.queueCount
                && abs(position + (other.time - time) * rate - other.position) < 1
        }
    }

    init(model: PlayerModel) { self.model = model }

    func activate() {
        let center = MPRemoteCommandCenter.shared()
        center.playCommand.addTarget { [weak self] _ in self?.model.play(); return .success }
        center.pauseCommand.addTarget { [weak self] _ in self?.model.pause(); return .success }
        center.togglePlayPauseCommand.addTarget { [weak self] _ in self?.model.togglePause(); return .success }
        center.nextTrackCommand.addTarget { [weak self] _ in self?.model.playNext(); return .success }
        center.previousTrackCommand.addTarget { [weak self] _ in self?.model.playPrevious(); return .success }
        center.skipForwardCommand.addTarget { [weak self] _ in
            guard let self else { return .commandFailed }
            self.model.seek(by: Double(self.model.settings.seekStep)); return .success
        }
        center.skipBackwardCommand.addTarget { [weak self] _ in
            guard let self else { return .commandFailed }
            self.model.seek(by: -Double(self.model.settings.seekStep)); return .success
        }
        center.changePlaybackPositionCommand.addTarget { [weak self] event in
            guard let event = event as? MPChangePlaybackPositionCommandEvent else { return .commandFailed }
            self?.model.seek(to: event.positionTime); return .success
        }
        updateSkipInterval()
    }

    func updateSkipInterval() {
        let interval = [NSNumber(value: model.settings.seekStep)]
        MPRemoteCommandCenter.shared().skipForwardCommand.preferredIntervals = interval
        MPRemoteCommandCenter.shared().skipBackwardCommand.preferredIntervals = interval
    }

    func update() {
        guard model.loaded else { return }
        let state = Sent(title: model.title, duration: model.duration, rate: model.paused ? 0.0 : model.speed,
                         queueIndex: model.playlistPosition, queueCount: model.playlist.count,
                         position: model.position, time: CACurrentMediaTime())
        if let sent, sent.covers(state) { return }
        sent = state
        let center = MPRemoteCommandCenter.shared()
        center.nextTrackCommand.isEnabled = model.playlistPosition + 1 < model.playlist.count
        center.previousTrackCommand.isEnabled = model.playlistPosition > 0
        var info = MPNowPlayingInfoCenter.default().nowPlayingInfo ?? [:]
        if model.title != lastTitle { info = [:]; lastTitle = model.title }
        info[MPMediaItemPropertyTitle] = model.title
        info[MPMediaItemPropertyPlaybackDuration] = model.duration
        info[MPNowPlayingInfoPropertyElapsedPlaybackTime] = model.position
        info[MPNowPlayingInfoPropertyPlaybackRate] = model.paused ? 0.0 : model.speed
        info[MPNowPlayingInfoPropertyDefaultPlaybackRate] = 1.0
        info[MPNowPlayingInfoPropertyMediaType] = MPNowPlayingInfoMediaType.video.rawValue
        if model.playlist.count > 1 {
            info[MPNowPlayingInfoPropertyPlaybackQueueIndex] = max(model.playlistPosition, 0)
            info[MPNowPlayingInfoPropertyPlaybackQueueCount] = model.playlist.count
        }
        MPNowPlayingInfoCenter.default().nowPlayingInfo = info
        MPNowPlayingInfoCenter.default().playbackState = model.paused ? .paused : .playing
    }

    func clear() {
        lastTitle = ""
        sent = nil
        MPNowPlayingInfoCenter.default().nowPlayingInfo = nil
        MPNowPlayingInfoCenter.default().playbackState = .stopped
    }
}
