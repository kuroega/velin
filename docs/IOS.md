# Velin for iOS

A personal-use iPhone and iPad edition. It shares the C++ engine, platform
services, extension host and AList client with the Windows player, and adds
a SwiftUI interface.

## Architecture

```text
SwiftUI app (ios/VelinPlayer)
   │  import VelinCore            Objective-C API: include/velin/apple/VLNPlayer.h
   ▼
VelinCore.xcframework            built by CMake (scripts/build-ios-core.sh)
   velin_apple   VLNPlayer facade (src/apple/VLNPlayer.mm)
   velin_engine  MediaEngine, libmpv client
   velin_system  platform services (src/system_apple.mm: NSURLSession,
                 CommonCrypto, Keychain)
   velin_extensions, velin_core, QuickJS, miniz
   │
   ▼
MPVKit-GPL (Swift package)       libmpv, FFmpeg, libass, libplacebo, MoltenVK
```

Video: mpv's `gpu-next` output renders through Vulkan on MoltenVK into a
`CAMetalLayer` passed as `wid`, with VideoToolbox hardware decoding.
Swift never sees C++ types; `VLNPlayer` is plain Objective-C.

## Requirements

- Xcode 27 or newer with the iOS 26+ SDK. The minimum iOS version is 26.0,
  because the shared C++ code uses floating-point `std::from_chars`.
- `brew install cmake ninja xcodegen`

## Build and run

```sh
scripts/build-ios-core.sh            # build/ios/VelinCore.xcframework
cd ios && xcodegen                   # ios/VelinPlayer.xcodeproj
open VelinPlayer.xcodeproj
```

The first build downloads MPVKit's binaries (several hundred MB). Rerun
`scripts/build-ios-core.sh` after changing any C++ code.

On the simulator, copy a video into the app's Documents folder and start it
with `-open`:

```sh
xcrun simctl install booted build/ios/DerivedData/Build/Products/Debug-iphonesimulator/VelinPlayer.app
cp clip.mkv "$(xcrun simctl get_app_container booted app.velin.player data)/Documents/"
xcrun simctl launch --console-pty booted app.velin.player -open clip.mkv
```

The app logs `VELIN state …` once a second. `-diagnostics` also checks the
Keychain-backed secret store.

## On your iPhone

Set your team in Xcode's Signing & Capabilities tab (or `DEVELOPMENT_TEAM`
in `ios/project.yml`), connect the phone and run. With a free Apple ID the
app expires after 7 days; a paid developer account lasts a year. Copy videos
into Velin through the Files app, or open them with the Files picker.

## Not available on iOS

- Helper processes (`process.exec`, yt-dlp, ffmpeg thumbnails): iOS apps
  cannot start child processes. `sys::ChildProcess` throws there.
- The Windows player UI. The iOS interface is new SwiftUI code.
