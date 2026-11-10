// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
import UIKit

// The player's fullscreen button: turns the interface to landscape (or back to
// portrait) even when the user's rotation lock is on. The chosen orientation
// is held until the phone is physically turned that way, after which normal
// auto-rotation takes over again.
final class Orientation {
    static let shared = Orientation()

    // What the app allows right now. nil means everything but upside down.
    private(set) var forced: UIInterfaceOrientationMask?
    private var observer: NSObjectProtocol?

    var supported: UIInterfaceOrientationMask { forced ?? .allButUpsideDown }

    private init() {
        UIDevice.current.beginGeneratingDeviceOrientationNotifications()
        observer = NotificationCenter.default.addObserver(forName: UIDevice.orientationDidChangeNotification,
                                                          object: nil, queue: .main) { [weak self] _ in
            self?.releaseIfDeviceMatches()
        }
    }

    func enterLandscape() {
        // Turn the way the phone is already tilted, if it is.
        let target: UIInterfaceOrientationMask = UIDevice.current.orientation == .landscapeRight ? .landscapeLeft : .landscapeRight
        force(target, keep: .landscape)
    }

    func enterPortrait() { force(.portrait, keep: .portrait) }

    // The player is closing: back to auto-rotation, with the screens behind it
    // following the rotation lock. iOS does not expose the lock, but with it on
    // the device keeps reporting portrait, so only then is portrait requested;
    // without the request a forced landscape would linger on the home screen.
    // With the lock off (held sideways, or lying flat) the current orientation
    // is kept and normal auto-rotation carries on from there.
    func leavePlayer() {
        forced = nil
        update(request: UIDevice.current.orientation == .portrait ? .portrait : nil)
    }

    // Back to auto-rotation.
    func release() {
        guard forced != nil else { return }
        forced = nil
        update(request: nil)
    }

    private func force(_ request: UIInterfaceOrientationMask, keep: UIInterfaceOrientationMask) {
        forced = keep
        update(request: request)
    }

    // With rotation lock off, once the phone is held the forced way, rotating
    // it should work as usual again. With the lock on the device keeps
    // reporting portrait, so a forced landscape stays until the button is used.
    private func releaseIfDeviceMatches() {
        guard let forced else { return }
        let device = UIDevice.current.orientation
        let matches = forced == .portrait ? device == .portrait : device.isLandscape
        if matches { release() }
    }

    private func update(request: UIInterfaceOrientationMask?) {
        for case let scene as UIWindowScene in UIApplication.shared.connectedScenes {
            for window in scene.windows {
                var controller = window.rootViewController
                while let next = controller?.presentedViewController { controller = next }
                controller?.setNeedsUpdateOfSupportedInterfaceOrientations()
                window.rootViewController?.setNeedsUpdateOfSupportedInterfaceOrientations()
            }
            if let request { scene.requestGeometryUpdate(.iOS(interfaceOrientations: request)) }
        }
    }
}

final class AppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication, supportedInterfaceOrientationsFor window: UIWindow?) -> UIInterfaceOrientationMask {
        Orientation.shared.supported
    }
}
