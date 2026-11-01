# Porting IINA to Windows

Velin starts from the IINA checkout at commit
`1b05edd0f7524ecc95286af75529007d4cfe1124`. IINA uses Swift, AppKit, and several
Apple-only frameworks. Its macOS application cannot be rebuilt for Windows by
changing a compiler flag. Velin reimplements the application layer in C++20 and
Win32 while retaining libmpv as the media engine.

This is the 0.2.0 Windows development implementation. It is not a claim of complete
IINA feature parity.

## Architecture

| IINA reference | Velin implementation | Responsibility |
| --- | --- | --- |
| `PlayerCore.swift` | `MediaEngine` and `PlaybackState` | Commands, observed playback state, tracks, chapters, and playlist |
| `MPVController.swift` | `src/media_engine.cpp` | libmpv lifecycle, client API binding, event queue, and properties |
| `PlayerWindowController.swift`, `MainWindowController.swift` | `PlayerWindow` | Native window, player controls, commands, and layout |
| `VideoView.swift` | Native child window passed to libmpv | Embedded video output and Windows GPU presentation |
| `Preference.swift`, history utilities | Portable core and Windows application state | Preferences and recent media |
| `iina-default-input.conf` | Window keyboard command dispatch | Familiar playback shortcuts adapted to Windows |

The source boundary between the portable core, playback engine, and window
allows model behavior to be tested without a video device or a running GUI.
The main GUI uses native controls and Direct2D. Plugin web interfaces use
WebView2; JavaScript plugins run in QuickJS.

## Playback lifecycle

1. Create the application and the child window that will host video.
2. Load `libmpv-2.dll` or `mpv-2.dll` from the executable directory.
3. Validate client API major version 2, configure embedded output, and initialize mpv.
4. Observe playback properties and post a private Windows message from mpv's
   wakeup callback.
5. Drain queued events without blocking the Windows message loop and update
   the application snapshot.
6. Unregister the callback and destroy the mpv instance before destroying the
   containing window or unloading the library.

The application owns keyboard input and controls. mpv's standalone on-screen
controller and default input bindings are disabled. Playlist and media paths
are passed as client API arguments rather than interpolated into a shell.

## Platform decisions

- C++20 and CMake support both Visual Studio 2022 and MinGW-w64.
- Win32 supplies file dialogs, drag and drop, window management, and keyboard input.
- Direct2D and DirectWrite draw the native interface at the monitor's DPI.
- The application manifest requests per-monitor v2 DPI awareness, ordinary user
  privileges, and long-path awareness.
- The runtime is loaded from the application directory rather than an arbitrary
  DLL found on the current directory or `PATH`.
- Hardware decoding and media demuxing are delegated to libmpv. Support for a
  given codec or stream depends on the installed libmpv build and the device.
- The runtime download is explicit, version-pinned, and hash-verified. A source
  build can compile without downloading libmpv and display an actionable error
  when playback is requested without the runtime.

The build also handles the
[WinLibs GCC 16 default-manifest collision](https://github.com/brechtsanders/winlibs_mingw/issues/299).
For affected GNU toolchains, CMake generates a target-local GCC specs file that
removes the automatic `default-manifest.o` insertion while preserving the rest
of the compiler's endfile specification. This keeps Velin's DPI and application
manifest intact without changing the installed compiler. MSVC uses the
manifest embedded in the resource file and disables the linker's extra manifest.

## Remaining parity work

See [implementation status](IMPLEMENTATION_STATUS.md) for current verified
outcomes and remaining work. WebSockets, cross-player messaging, the JavaScript
runtime, plugin WebView2 UI, OCR and system media integration are implemented.
These implementations do not establish arbitrary macOS plugin compatibility.
Apple-specific UI and system APIs require Windows adaptations. HDR, multiple
GPU/monitor behavior, extended playback and live subtitle services require
additional environment-specific verification.

The detailed [plugin API audit](PLUGIN_COMPATIBILITY.md) records Windows behavior
and known differences, including process streams and directory listing paths.
