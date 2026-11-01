# IINA to Velin parity matrix

Reference: IINA `1b05edd0f7524ecc95286af75529007d4cfe1124`; target: Velin
0.2.0. Source paths in the reference column are relative to the sibling IINA
checkout; target paths are relative to Velin. This is a working inventory, not
an exhaustive or reviewed acceptance of every menu item, preference, shortcut,
error path, plugin, or platform configuration. A row may cover several behaviors;
its status applies only to the behavior and evidence described in that row.

Status: **T** = implemented with the stated test evidence; **U** = implementation
found but behavior or parity is unverified; **M** = a specific reference workflow
is missing or materially incomplete; **A** = intentional Windows adaptation whose
equivalent still needs acceptance; **E** = verification needs an external environment.
Evidence in this matrix is the previously recorded evidence in HANDOFF.md unless
otherwise stated. Source inspection and a test file's existence do not establish
that all related behavior passed. The recorded GUI and playback runs predate the
last process/file changes; they are not final release acceptance.

| Behavior and reference | Velin implementation / Windows behavior | Status and evidence | Gap and acceptance criterion |
| --- | --- | --- | --- |
| Open files, folders and URLs; initialize/shut down mpv (`PlayerCore.swift` open/start/shutdown, `MPVController.swift`) | `player_window.cpp` file dialog/openPaths, `player_features.cpp` acceptLaunch, `media_engine.cpp` | T: real playback fixture and GUI smoke on both toolchains; missing-runtime GUI path exercised | Compare open failures, cancellation, network and reopen/restart sequences; play a variety of local and network media without hangs or leaked workers. |
| Pause, seek, frame step, speed, volume, mute and stop (`PlayerCore.swift` playback methods; `MenuController.swift` playback bindings) | `player_window.cpp` execute/keyDown; `media_engine.cpp` | T: playback fixture and selected GUI shortcuts | Exercise all menu and keyboard paths, bounds and end-of-file states; confirm actual resulting mpv state and restart persistence. |
| A-B, file and playlist loops, shuffle (`PlayerCore.swift` loop and shuffle methods) | `player_window.cpp`, `player_actions.cpp`, `media_engine.cpp` | U: source paths found; broad smoke does not establish each loop transition | Verify set/clear A-B, file/playlist loop and original order after shuffle, including playlist edits and restart. |
| Track selection and audio devices (`PlayerCore.swift` setTrack/getAudioDevices; `MenuController.swift`) | Dynamic track/device menus and mpv properties in `player_window.cpp`, `media_engine.cpp` | T: playback smoke covers tracks and subtitles; device-specific outcomes unverified | Test primary/secondary subtitles, track removal and device changes with real devices; check menu selection and fallback. |
| Playlist navigation, reorder, add/remove, save/load and chapters (`PlayerCore.swift` playlist methods; `PlaylistViewController.swift`) | `player_window.cpp`, `player_actions.cpp`, `library.cpp` | T: real playback navigation and library tests | Compare full selection, ordering, drag/context actions and chapter behavior; verify persistence and malformed/cancelled playlist load. |
| Related media discovery (`PlayerCore.swift` autoLoadFilesInCurrentFolder and associated matcher) | `core.cpp` numbered sibling matching, `player_window.cpp` | T: `tests/core_tests.cpp` related-file cases | Reference matching is broader than the documented digit-placeholder rule; compare ambiguous names, subtitle matching, ordering and cancellation against reference before treating as equivalent. |
| History, search, reopening and removal (`HistoryController.swift`, `HistoryWindowController.swift`) | `library.cpp`, `preferences_dialog.cpp` history dialog with search, sort, favorites, open/remove/clear and local-file context actions for Explorer and the Recycle Bin | U: `Test-HistoryActions.ps1` verified cancellation and confirmed recycling on both toolchains outside the sandbox on 2026-11-01. Several sandboxed MinGW runs timed out; one showed an additional Shell delete dialog. Explorer reveal is unverified | IINA also groups entries and offers play in a new window. Verify reveal, missing files, failure handling and restart, and provide or explicitly review the remaining workflows. |
| Preferences, live apply and IINA import (`Preference.swift`, `Pref*ViewController.swift`) | `core.cpp` bindings/import, `preferences_dialog.cpp`, `player_options.cpp`, `player_features.cpp` | U: preference field-consumer audit; core tests cover IINA initial-volume selection and partial SPDIF imports, not a reference preference-by-preference comparison. A source count finds 210 static IINA preference keys; Velin's generated bindings and platform-specific fields need manual semantic matching. | Inventory reference keys, defaults, controls and migration semantics; verify each effect, validation, cancel/apply and restart, including unsupported keys. |
| Configurable key bindings (`PrefKeyBindingViewController.swift`, `PlayerCore.swift` loadKeyBindings, `MenuController.swift` key equivalents) | Native dispatch in `player_window.cpp`; engine can receive an input configuration in `media_engine.cpp` | M: no equivalent binding editor was found; `input-conf` alone does not demonstrate application shortcut customization | Support inspect/edit/reset/conflict handling and persistence of relevant bindings, or establish and test a deliberate equivalent; verify shortcuts update menu labels and survive restart. |
| Main window, controls, fullscreen and scaling (`MainWindowController.swift`, `PlayerWindowController.swift`, `VideoView.swift`) | Native Win32/Direct2D rendering, fullscreen and DPI handling in `player_window.cpp` | T: GUI smoke checks responsiveness and captures; no visual comparison to macOS reference | Compare layout, focus, pointer/keyboard controls and transitions at supported sizes/DPI; test mixed DPI and display removal. |
| Mini player and picture in picture (`PlayerCore.swift` mini-player transitions; `VideoPIPViewController.swift`) | `player_features.cpp` music mode and PiP, `player_window.cpp` | A: native Windows window modes; GUI smoke is not a full transition test | Check play/pause, resize, topmost, close/restore, fullscreen and multi-window interaction; document platform differences. |
| Window size presets, aspect locking and crop presets (`MenuController.swift` video menu; `MainWindowController.swift`) | `player_window.cpp` aspect menu/fullscreen and crop presets; `player_actions.cpp` custom ratio, interactive crop and clear; `core.cpp` centered geometry | M: size presets and aspect lock remain missing. Core tests exercise centered crop geometry and invalid ratios. On 2026-11-01 `Test-Gui.ps1` passed 51 assertions on each toolchain, including applying and clearing a 4:3 crop on a 320x180 fixture and reading mpv's crop property | Provide and test equivalent sizing and lock workflows; validate custom crop, rendered output, invalid values and DPI/video-size changes with real media. |
| Video adjustments, flip/mirror/deinterlace and screenshot (`PlayerCore.swift` video and screenshot methods) | `player_actions.cpp`, `player_window.cpp`, `preferences_dialog.cpp`, `media_engine.cpp` | U: code paths exist; individual visual outcomes are not established by menu labels | Validate resulting frames, rotation/crop, screenshot with/without subtitles, destination errors and cancellation. |
| Active and saved audio/video filters (`FilterWindowController.swift` add/remove/save/toggle/edit/delete; `MenuController.swift` saved filter menus) | `core.cpp` exposes raw `mpv.vf`/`mpv.af` preferences; `tool_windows.cpp` inspector displays filter properties; fixed video toggles in `player_actions.cpp` | M: no equivalent filter manager or saved-preset workflow found | Add/edit/remove active filters and save/edit/delete/toggle presets with mpv error feedback; verify actual chains, cancellation and persistence across restart. Raw configuration fields alone do not satisfy this. |
| Inspector and mpv property watches (`InspectorWindowController.swift` dynamic update and watch actions) | `tool_windows.cpp` displays fixed properties and user-added mpv property watches, refreshed every 500 ms and atomically saved under the application data directory | T: `Test-Gui.ps1` passed 48 assertions on each toolchain, including adding a watch, reading its live `time-pos` value, reporting an unavailable property, restoring a watch after process restart and removing it | Verify invalid input and persistence failures in the UI, resizing and screen-reader operation. A null mpv value and an unavailable property currently share the same display text. |
| External audio and subtitle files, subtitle visibility and styling (`PlayerCore.swift` loadExternalAudioFile/loadExternalSubFile; `MenuController.swift`) | `player_actions.cpp`, `player_window.cpp`, `core.cpp`, `player_options.cpp`; a separate menu action cycles secondary subtitle visibility | T: playback fixture covers subtitle loading; `Test-Gui.ps1` verified the secondary visibility action changes and restores the mpv property independently of primary visibility on both toolchains on 2026-11-01 | Exercise multiple tracks and visible rendering, bad files, delay/style/encoding changes and restart. |
| Online subtitle search/download (`OnlineSubtitle.swift`, `SubChooseViewController.swift`) | OpenSubtitles/Assrt service clients in `extensions.cpp`, `player_features.cpp` | T: local service-contract fixtures; E: live account/service outcomes unverified | With configured credentials and network, verify authentication, search, selection, download, cancellation, error recovery and secret handling. |
| Timeline thumbnail previews (`PlayerCore.swift` generateThumbnails) | `thumbnails.cpp`, `player_window.cpp`; FFmpeg and WIC | T: `tests/thumbnail_tests.cpp` covers real generation/decode, cache, latest request, resize and cancellation | Compare hover interaction and responsiveness during seek, close and media changes; exercise long files and helper failure. |
| Frame text recognition (`MainWindowController.swift` Live Text) | Windows OCR in `platform_ocr.cpp`, `player_actions.cpp` | A: Windows OCR instead of Live Text; availability depends on installed OCR languages | Validate actual recognition, unavailable-language error, cancellation/close and text selection workflow on supported Windows versions. |
| Plugin lifecycle, JavaScript APIs and permissions (`JavascriptAPI*.swift`, `JavascriptPlugin*.swift`) | QuickJS runtime, `extensions.cpp`, plugin manager, `PLUGIN_COMPATIBILITY.md` | T: extension tests and local fixture scripts cover selected APIs, menu, filesystem, process and redirects | Test representative real packages through install, startup, interaction, update, disable/reload and uninstall; compare exact failures, permissions, encodings and cleanup. |
| Plugin WebView, input, messages and sockets (`JavascriptAPIOverlay.swift`, `JavascriptAPIInput.swift`, `JavascriptAPIWebSocket.swift`) | `plugin_webview.cpp`, `plugin_websocket.cpp`, `player_features.cpp`; restricted HTML views are refused because WebView2 WebSocket handshakes bypass the resource filter. Only network permission with an explicit `*` domain can create a view. | A/M: `Test-WebViewPermissions.ps1 -IncludeWebSockets` passed on both toolchains on 2026-11-01: restricted views were refused with no requests, while unrestricted document, iframe and worker sockets opened. `Test-PluginWindows.ps1` passed both. | Restore offline and domain-restricted views only with a proven fail-closed enforcement mechanism covering documents, frames and workers. Compare remaining AppKit event fields and popup needs with real plugins. |
| Global plugin instance (`PlayerCore.swift` plugin lifecycle, `JavascriptAPIGlobal.swift`) | `extensions.cpp`: global entry per independent parent, suppressed for child players | A: differs from an app-wide singleton | Establish intended multi-window semantics from reference; exercise independent players, reload and shutdown, then implement or document a verified adaptation. |
| Browser integration and URL scheme (IINA browser extension and `AppDelegate.swift`) | `browser/`, `platform.cpp`, `main.cpp`, `player_features.cpp` | U: assets and registration code present; E: installed browser workflow not verified | Install extension in supported browsers; verify send/open/enqueue, permissions and errors, protocol handling and single-instance forwarding. |
| Command line and multiple players (IINA CLI and `PlayerCore.swift` player selection) | `main.cpp`, `cli.cpp`, `platform.cpp`, `player_features.cpp` | U: platform tests cover sender-relative Unicode paths, quoting and option termination. `Test-LaunchPaths.ps1` passed on both toolchains on 2026-11-01, exercising CLI startup and forwarding from different directories; `--data-dir` is not applied to an already running player | Compare documented options and quoting with reference; test concurrent launches, new windows, isolated data directories and error exits. |
| Accessibility and system media integration (`MainWindowController.swift`, platform-specific macOS integrations) | `accessibility.cpp`, `platform.cpp`, `player_features.cpp` implement MSAA/UI Automation, SMTC and taskbar | A: Windows-native equivalents; platform tests do not prove real assistive-device interaction | Verify focus order, names, states, actions and screen-reader use; test media keys, taskbar and multiple players on Windows. |
| Portable update, installation and associations (IINA release integration; Sparkle is macOS-specific) | `Apply-Update.ps1`, `installer/Velin.nsi`, `platform.cpp`, `Package-Release.ps1`, `Test-InstalledLifecycle.ps1` | T: isolated portable updater success/failure/rollback fixtures; E: installed lifecycle script prepared but not run in a disposable environment | In a disposable Windows environment run the lifecycle script and verify optional integrations, running-player behavior, update rollback and locked files. See `INSTALLED_LIFECYCLE_TEST.md` for scope. |
| Release and bundled dependencies (`.github/workflows`, `THIRD_PARTY_NOTICES.md`) | Two toolchains, pinned runtimes, portable/source/NSIS packaging | U: local packages built and hashes checked; E: hosted clean CI, signing identity and publication unavailable | Run clean CI, validate extracted packages and corresponding-source/license obligations, then sign/publish only with supplied identity and endpoint. |
| Device and endurance acceptance (`MPVController.swift`, `DisplayController.swift`, window lifecycle) | libmpv and Windows display/device integration | E: hardware, display and duration coverage absent | Test sustained playback/open/close/seek with resource monitoring; verify HDR/SDR, hardware fallback, sleep/resume, multiple GPUs/monitors and display/device changes. |

## Audit continuation

### 2026-11-01 implementation and verification update

This update supersedes the missing-implementation descriptions for window sizing,
key bindings and filters above; their full acceptance criteria remain open.

- Window sizing now offers half, original, double and fit-to-screen presets,
  with a persistent aspect lock enforced during native window sizing. Status U:
  the MinGW GUI run passed the new double-size geometry and lock-setting
  assertions before timing out later at unavailable-watch removal. Dragging,
  rotation, video changes and mixed DPI remain unverified.
- Key preferences accept normalized overrides, reject duplicate and unsupported
  combinations, dispatch punctuation and update relevant menu labels. A native
  editor now lists overrides and supports add/update/remove with draft-only edits
  until Preferences Apply/OK. Status U: `Test-Gui.ps1` passed 55 assertions on
  both toolchains, including editor add/update/remove/cancel/apply and saved
  overrides. IINA also offers
  several built-in and user keymap configurations; Velin does not yet match that
  configuration workflow. Keyboard capture and accessibility are unverified.
  The preference-by-preference IINA comparison is still incomplete.
- Audio/video filter managers now add/edit/remove active filters and
  save/edit/delete/toggle presets in an atomic JSON file. Status U: on both
  toolchains the real playback fixture passed valid chains, GUID labels,
  invalid-filter rejection and preservation of the previous chain. MinGW's
  `Test-FilterManager.ps1` passed add/remove/save/toggle/restart on both builds. Preset
  edit/delete/cancel and malformed-file GUI acceptance remain open. The checked
  writer currently refuses chains containing disabled filters and accepts only
  named string parameters; review these restrictions against real filter chains.
- IINA import preserves unsupported launch/subtitle modes and rejects malformed
  boolean mpv overrides rather than silently changing their meaning. Core tests
  passed on both toolchains. This is still a partial preference audit.
- Both Release builds passed. MinGW CTest passed five tests with WebSocket and
  fixture-less engine tests skipped. MSVC core/library/platform/thumbnails passed;
  extensions failed with `weakly_canonical` access denied, reproduced in a fresh
  workspace test directory. Both separate headless playback fixtures passed.
  This is not complete compatibility or release acceptance.

### 2026-11-01 shortcut editor and GUI test follow-up

- `Test-Gui.ps1` now selects the intended inspector watch row and sends the
  Delete virtual key. The previous End-key input caused false removal timeouts.
- The dedicated editor and selected GUI regression passed on both release
  toolchains (55 assertions each). This verifies only the covered flows; it is
  not full preference or IINA compatibility acceptance.
- The extension script and full CTest passed outside the sandbox for both
  toolchains. Each CTest run had six passing tests; `engine_smoke` skipped
  without a configured fixture. The sandbox-only access-denied behavior is not
  a failure in the unsandboxed extension test run.

The reference menu binding inventory in `MenuController.swift:215-443` exposes
several concrete gaps above. Further work must enumerate the nib-defined menu
items and shortcuts, all preferences and defaults, sidebar/context actions,
error/cancel/restart behavior, and plugin API contracts rather than treating the
rows above as a complete checklist. Record source-level differences separately
from failures observed in a running application. Add precise test commands,
fixture scope and dated results to a row when its acceptance criterion is met;
do not infer full parity from a partial smoke test.
