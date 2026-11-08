// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
import SwiftUI
import UIKit

// The layer mpv renders into through MoltenVK.
final class VideoLayer: CAMetalLayer {
    // MoltenVK briefly sets a 1x1 drawable to finish a presentation, which
    // makes the picture flicker. Ignore those sizes.
    override var drawableSize: CGSize {
        get { super.drawableSize }
        set { if newValue.width > 1 && newValue.height > 1 { super.drawableSize = newValue } }
    }
}

final class VideoView: UIView {
    override class var layerClass: AnyClass { VideoLayer.self }
    var videoLayer: VideoLayer { layer as! VideoLayer }

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .black
        videoLayer.pixelFormat = .bgra8Unorm
        videoLayer.framebufferOnly = true
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    override func layoutSubviews() {
        super.layoutSubviews()
        let scale = window?.screen.scale ?? UIScreen.main.scale
        videoLayer.contentsScale = scale
        videoLayer.drawableSize = CGSize(width: bounds.width * scale, height: bounds.height * scale)
    }
}

struct VideoSurface: UIViewRepresentable {
    let model: PlayerModel

    func makeUIView(context: Context) -> VideoView {
        let view = VideoView(frame: .zero)
        model.attach(layer: view.videoLayer)
        return view
    }

    func updateUIView(_ view: VideoView, context: Context) {}
}
