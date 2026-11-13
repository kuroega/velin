// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
import AVFoundation
import SwiftUI
import VelinCore

@main
struct VelinPlayerApp: App {
    @UIApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @StateObject private var settings: AppSettings
    @StateObject private var model: PlayerModel
    @State private var path = NavigationPath()

    init() {
        // Keep playing with the ringer switch off and in the background.
        try? AVAudioSession.sharedInstance().setCategory(.playback, mode: .moviePlayback)
        try? AVAudioSession.sharedInstance().setActive(true)
        if CommandLine.arguments.contains("-diagnostics") {
            for line in VLNDiagnostics.runSecretStoreCheck() { print("VELIN diagnostics \(line)") }
        }
        let settings = AppSettings()
        _settings = StateObject(wrappedValue: settings)
        _model = StateObject(wrappedValue: PlayerModel(settings: settings))
    }

    var body: some Scene {
        WindowGroup {
            NavigationStack(path: $path) {
                HomeView(path: $path)
                    .navigationDestination(for: Route.self) { route in
                        switch route {
                        case .alist(let folder): AListView(initialPath: folder)
                        case .settings: SettingsView()
                        }
                    }
            }
            .fullScreenCover(isPresented: $model.showPlayer) {
                PlayerView()
                    .environmentObject(model)
                    .environmentObject(settings)
            }
            .environmentObject(model)
            .environmentObject(settings)
            .environmentObject(model.downloads)
            .preferredColorScheme(.dark)
            .tint(Color(red: 0.42, green: 0.45, blue: 1.0))
            .onOpenURL { url in
                // velin://alist?path=...: play a file, or browse a folder.
                if let folder = model.handleLink(url) { path.append(Route.alist(folder)) }
            }
            .task { launchForTesting() }
        }
    }

    // Debug builds accept "-route settings" and "-alistSignIn <server> <user> <password>" so the
    // AList flow can be tested from the command line; then "-open" runs.
    private func launchForTesting() {
        #if DEBUG
        let arguments = CommandLine.arguments
        // "-route settings" opens the Settings screen.
        if let index = arguments.firstIndex(of: "-route"), index + 1 < arguments.count, arguments[index + 1] == "settings" {
            path.append(Route.settings)
        }
        if let index = arguments.firstIndex(of: "-alistSignIn"), index + 3 < arguments.count {
            model.alist.signIn(toServer: arguments[index + 1], username: arguments[index + 2],
                               password: arguments[index + 3], otpCode: nil) { error in
                print("VELIN alist sign-in \(error.map { "failed: \($0.localizedDescription)" } ?? "ok")")
                model.openLaunchArgument()
            }
            return
        }
        #endif
        model.openLaunchArgument()
    }
}
