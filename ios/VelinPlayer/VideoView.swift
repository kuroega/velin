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

// Shows the model's one video view. The engine keeps rendering into the same
// layer for the app's whole life, so the view is moved, never recreated.
struct VideoSurface: UIViewRepresentable {
    let model: PlayerModel

    func makeUIView(context: Context) -> UIView {
        let container = UIView()
        container.backgroundColor = .black
        return container
    }

    func updateUIView(_ container: UIView, context: Context) {
        let video = model.videoView
        guard video.superview !== container else { return }
        video.removeFromSuperview()
        video.frame = container.bounds
        video.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        container.addSubview(video)
    }
}

// Touch handling over the video: taps, double-taps, pans and pinches.
struct GestureLayer: UIViewRepresentable {
    var onTap: () -> Void
    var onDoubleTap: (CGFloat) -> Void                    // Horizontal position, 0...1.
    var onPan: (UIGestureRecognizer.State, CGPoint, CGPoint, CGSize) -> Void  // State, start, translation, size.
    var onPinch: (CGFloat) -> Void                        // Final scale.

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    func makeUIView(context: Context) -> UIView {
        let view = UIView()
        view.backgroundColor = .clear
        let double = UITapGestureRecognizer(target: context.coordinator, action: #selector(Coordinator.doubleTapped(_:)))
        double.numberOfTapsRequired = 2
        let single = UITapGestureRecognizer(target: context.coordinator, action: #selector(Coordinator.tapped(_:)))
        single.require(toFail: double)
        let pan = UIPanGestureRecognizer(target: context.coordinator, action: #selector(Coordinator.panned(_:)))
        pan.maximumNumberOfTouches = 1
        let pinch = UIPinchGestureRecognizer(target: context.coordinator, action: #selector(Coordinator.pinched(_:)))
        for recognizer in [double, single, pan, pinch] { view.addGestureRecognizer(recognizer) }
        return view
    }

    func updateUIView(_ view: UIView, context: Context) { context.coordinator.parent = self }

    final class Coordinator: NSObject {
        var parent: GestureLayer
        private var start = CGPoint.zero
        init(_ parent: GestureLayer) { self.parent = parent }

        @objc func tapped(_ recognizer: UITapGestureRecognizer) { parent.onTap() }

        @objc func doubleTapped(_ recognizer: UITapGestureRecognizer) {
            guard let view = recognizer.view, view.bounds.width > 0 else { return }
            parent.onDoubleTap(recognizer.location(in: view).x / view.bounds.width)
        }

        @objc func panned(_ recognizer: UIPanGestureRecognizer) {
            guard let view = recognizer.view else { return }
            if recognizer.state == .began { start = recognizer.location(in: view) }
            parent.onPan(recognizer.state, start, recognizer.translation(in: view), view.bounds.size)
        }

        @objc func pinched(_ recognizer: UIPinchGestureRecognizer) {
            if recognizer.state == .ended { parent.onPinch(recognizer.scale) }
        }
    }
}
