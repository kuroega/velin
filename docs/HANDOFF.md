# Velin end-to-end transplant handoff

Updated: 2026-11-01 (Asia/Taipei). Development version: 0.2.0.
Implementation and test evidence below was collected on 2026-10-31.
This document supersedes earlier handoff snapshots in this chat.

## 2026-11-01 further continuation

### 2026-11-01 session follow-up

- Added a native keyboard shortcut editor to Preferences. It lists existing
  overrides and supports add/update/remove; changes remain a draft until the
  parent preferences are applied or saved. Unsupported keys and empty commands
  are rejected. `Test-Gui.ps1` exercises add/update/remove/cancel/apply and
  confirms persisted overrides on both toolchains; each run passed 55
  assertions. Reports:
  `.cache/gui-smoke/run-20261101-100810-204/summary.json` (MinGW) and
  `.cache/gui-smoke/run-20261101-100824-624/summary.json` (MSVC). The editor
  also lays out controls and columns for the active DPI.
- Core and library CTest checks passed on both toolchains. The GUI test timeout
  was caused by its script sending the End key instead of Delete and failing to
  select the target watch row. The script now selects each target row and sends
  the correct key; the full GUI suite passed on both toolchains.
- `Test-Playback.ps1 -Headless` also passed all eight real media integration
  checks on both toolchains, including Unicode media, audio/video decode,
  subtitles, playlists, filters, recovery and engine reinitialization. This does
  not cover HDR, display behavior, or hardware-specific acceleration paths.
- Rebuilt `velin_extensions_tests` successfully, then reran
  `Test-Extensions.ps1`. Inside the sandbox, `std::filesystem::weakly_canonical`
  returned access denied even for a system-temp fixture. The same extension
  script passed outside the sandbox. Full CTest passed on both toolchains outside
  the sandbox: 6/6 executed tests passed, and `engine_smoke` skipped because no
  engine fixture was configured. No test workaround was retained.
- The complete preference-by-preference and plugin-compatibility audits remain
  open. Installed lifecycle, hardware, live-service, signing and publication
  acceptance also remain open. Host inspection found no Windows Sandbox,
  Hyper-V management command or VMConnect executable; CIM access to display
  hardware was denied. The current-user certificate store had no code-signing
  private key, no signing credential variables were configured, and no
  `ReleaseBaseUrl` was supplied. No subtitle-service account credentials were
  available. These are concrete prerequisites for the outstanding acceptance,
  not work completed by this session.
- Reference inventory scoping confirms that IINA's `Preference.swift` declares
  210 static keys. A full semantic comparison of those defaults, controls,
  import mappings and effects has not been performed. IINA also ships several
  built-in and user keymap configurations; Velin's override editor does not
  implement that separate configuration workflow.

### Latest implementation continuation

- Added window half/original/double/fit presets and persistent native aspect
  locking. New GUI geometry/persistence assertions passed before the broader
  MinGW script timed out at unavailable-watch removal; interactive drag/DPI
  acceptance remains open.
- Improved key override validation, default visibility, punctuation dispatch and
  effective menu labels. A dedicated shortcut editor is still incomplete.
- Added active audio/video filter management and persistent presets. Both
  toolchains passed `Test-FilterManager.ps1` add/remove/save/toggle/restart and
  `Test-Playback.ps1 -Headless`, including invalid-filter rejection preserving
  the old chain. Direct node-property writes do not reliably validate filters;
  checked writes use synchronous `vf set` / `af set` commands instead.
- Both Release builds passed. MinGW CTest passed five tests; WebSocket and
  fixture-less engine tests skipped. MSVC core/library/platform/thumbnails
  passed; extensions failed with access denied during `weakly_canonical`, also
  in a fresh workspace test directory. This remains unresolved.
- IINA import no longer equates incompatible subtitle modes or launch actions,
  and rejects malformed boolean overrides. Core tests passed on both builds.
- See `FILTERS.md` and the dated continuation in `PARITY_MATRIX.md` for scope.
  Full preference, compatibility, hardware and release acceptance remains open.

- The video menu offers centered crop presets and a custom aspect ratio; the
  playback menu exposes previous frame, go to time and jump to beginning.
  `Test-Gui.ps1` passed 51 assertions on each toolchain, including applying and clearing a
  4:3 crop and reading the mpv property. Geometry and invalid ratios are covered
  by core tests. Window size presets, aspect lock, custom-crop interaction and
  the new playback menu actions still need implementation or further testing.
- IINA preference import now honors `enableInitialVolume` and merges explicitly
  supplied SPDIF switches, including clearing the last enabled codec. Core tests
  exercise these cases; this is not a complete preference migration audit.
- Relative media paths are resolved against the launching process's working
  directory for CLI launches and single-instance forwarding. The `--` option
  terminator is supported. Platform tests cover Unicode, quoting and arguments;
  `Test-LaunchPaths.ps1` passed on both toolchains for launch and forwarding
  from different directories. Applying `--data-dir` to an already running
  instance remains unresolved.
- The updater accepts the default empty restart argument array in Windows
  PowerShell 5.1. Update and rollback fixtures passed on PowerShell 5.1 and 7;
  the CI workflow now runs both. Hosted CI has not run.

## 2026-11-01 security and parity continuation

- WebView2 does not filter WebSocket handshakes. Plugin HTML views now fail
  closed unless the plugin has `network-request` and an explicit unrestricted
  `*` domain. This disables offline and domain-restricted HTML views; it is a
  mitigation, not a full implementation of restricted WebSocket support.
  The installation prompt now displays the network scope. On both toolchains,
  `Test-WebViewPermissions.ps1 -IncludeWebSockets` confirmed restricted views
  were refused before any requests and unrestricted document, iframe and worker
  sockets connected. `Test-PluginWindows.ps1` passed on both toolchains.
- Secondary subtitle visibility has an independent menu action. `Test-Gui.ps1`
  passed 50 assertions on both toolchains, including toggling the secondary mpv
  property and restoring it without changing primary visibility. Reports are
  `.cache/gui-smoke/run-20261101-033445-031/summary.json` (MinGW) and
  `.cache/gui-smoke/run-20261101-033638-240/summary.json` (MSVC).
- The history context menu can reveal a local file or ask to move it to the
  Recycle Bin. Cancellation and confirmed removal passed on both toolchains in
  `Test-HistoryActions.ps1`. Several sandboxed MinGW runs timed out awaiting
  recycling; a later diagnostic captured an additional Windows Shell delete
  dialog. Both builds passed outside the sandbox. The earlier timeouts are not
  individually diagnosed. Explorer reveal and other history gaps remain unverified.
- `Test-InstalledLifecycle.ps1` and `INSTALLED_LIFECYCLE_TEST.md` prepare a
  disposable-profile installation, upgrade/reinstall and uninstall regression.
  Only syntax and safety refusal checks ran here; no isolated Windows instance was
  available, so this is not installed-product validation.
- Both Release builds passed. CTest passed on both outside the socket-restricted
  sandbox; `engine_smoke` remained skipped without a fixture. The GUI/WebView
  tests above predate the later installation-prompt wording change. The matrix
  remains a working inventory and numerous functional and environmental gaps
  remain; no end-to-end parity or release acceptance is claimed.

## 2026-11-01 continuation

- `docs/PARITY_MATRIX.md` now records a source-based working inventory, evidence,
  concrete gaps and acceptance criteria. It is not yet an exhaustive reviewed
  menu, preference or plugin API inventory. Continue using it to drive code work.
- The media inspector now supports persistent custom mpv property watches.
  `Test-Gui.ps1` passed 48 assertions for each toolchain, including add, live
  value, unavailable property, restart persistence and removal. Reports are under
  `.cache/gui-smoke/`.
- Native mouse plugin events now include bottom-left DPI-independent client
  coordinates, click count and basic mouse pressure. `Test-PluginWindows.ps1`
  passed for both toolchains with actual posted input messages.
- The installer requires an explicit version and protects shared registrations
  and shortcuts from an older uninstaller. Shortcut creation does not overwrite
  existing names; removal verifies the target. `Test-OwnedShortcuts.ps1` passed
  in an isolated fixture and NSIS compiled with a minimal payload. A different
  installation may retain an old shortcut by design; installed lifecycle and
  upgrade behavior still need an isolated Windows system.
- Both Release toolchains were rebuilt. CTest passed outside the socket-restricted
  sandbox on both; `engine_smoke` was skipped because no CTest fixture was set.
  The previous separate playback runs predate this continuation.
- WebView permission and resource filtering work is described in
  `PLUGIN_COMPATIBILITY.md`. Real tests confirmed that WebSocket handshakes bypass
  the WebView2 resource callback, including for plugins without network permission
  and destinations outside the allowlist. This remains an open security gap;
  `Test-WebViewPermissions.ps1 -IncludeWebSockets` reproduces it. A DevTools
  `Network.setBlockedURLs` experiment did not block these handshakes and was removed.
- Windows Sandbox and Hyper-V VM connection executables were not found on this
  host. No live installation or associations were modified. Hardware, live
  services, hosted CI, signing and publication prerequisites remain open.

The sections below preserve the earlier snapshot; where they describe input,
installer version or the absence of a parity matrix, this continuation supersedes
them. Full parity and release acceptance have not been achieved.

## 1. Mission and working agreement

The user wants a complete, modern C++ Windows GUI transplant of IINA, using the
sibling IINA repository as the behavioral reference. Continue implementation
and end-to-end validation, not merely another inventory or partial milestone.
The application is substantial and builds, but complete IINA parity has NOT
been demonstrated. A passing build or a matching API name is not completion.

- Reply to the user in Traditional Chinese (zh-TW).
- Write all code, documentation, configuration and AGENTS.md content in English.
- Do not spawn subagents without explicit authorization or applicable instructions.
- Preserve existing work. The target started empty; its source tree remains
  untracked. No commits, pushes or public releases were performed.
- Do not reset, clean or overwrite this tree to obtain a fresh baseline.
- Continue authorized, reversible work autonomously. Distinguish missing code
  from tests requiring external hardware, credentials or an isolated OS.
- Do not invent signing credentials, publishing URLs or live service outcomes.

## 2. Workspace and baseline

| Item | Location or value |
| --- | --- |
| Target | C:/Users/WYX/Documents/workspace/velin |
| Reference | C:/Users/WYX/Documents/workspace/iina |
| Verified reference HEAD | 1b05edd0f7524ecc95286af75529007d4cfe1124 |
| Toolchains | MinGW GCC 16.1 + Ninja; Visual Studio 2022/MSVC; CMake 4 |
| Language/platform | C++20, native x64 Windows, Win32, Direct2D/DirectWrite |
| Playback | Dynamically loaded libmpv client API major 2 |
| Extensions | QuickJS-ng, WebView2, Winsock WebSocket server |
| Platform bridges | Windows OCR, MSAA/UI Automation, SMTC/taskbar |
| Dependencies | Hash-pinned runtime/; vendored third_party/; config/ manifests |
| NSIS | .cache/tools/nsis-3.13/makensis.exe |

Read user/project instructions before changes. The user-level file is
C:/Users/WYX/AGENTS.md. No repository AGENTS.md was found during this handoff.
Consult README.md, docs/PORTING.md, docs/IMPLEMENTATION_STATUS.md and
especially docs/PLUGIN_COMPATIBILITY.md. Those describe implemented behavior;
they are not an exhaustive parity acceptance matrix.

## 3. Implemented behavior to preserve

- Playback, tracks/devices, chapters, playlist editing, history/session persistence,
  preferences and IINA preference import, related-file series discovery.
- PiP, music layout, crop/video adjustments, screenshots, OCR, timeline previews.
- CLI, single-instance IPC, native platform integrations, browser extension assets.
- OpenSubtitles/Assrt clients and local service-contract fixtures.
- Plugin package installation, catalog/update UI, permission checks, QuickJS,
  WebView2 UI bridges, WebSockets and authenticated parent/child player messages.
- Portable updates with hash verification/rollback; portable ZIP, source ZIP,
  NSIS installer and optional signing scripts.

Recent fixes already completed; do not reopen these as unimplemented work:

- Startup/window/music preferences, pause-on-open, chapter markers and numbered
  related-file discovery. Selected media stays first; matching siblings follow
  naturally. Explicit multi-file selections and URLs do not auto-expand.
- Actual FFmpeg thumbnail generation/WIC decode, persistent cache, resize,
  latest-request and cancellation tests. New requests clear unconsumed stale frames.
- Native key-up and mouse-down/up/drag plugin callbacks, tested by posted Win32
  messages. Reentrant reload waits until JavaScript returns; old queued events
  are discarded on reload.
- Nested native plugin menus, inherited disabled state, playlist builder closures,
  expired context identifiers, and rejection of disabled/separator invocations.
- Global-only manifests; validation of both entry paths; suppression of global
  entries in child players; blocked WebView2 new-window popups.
- File reads: UTF-8/16/32, endian aliases, mapped Windows/legacy encodings including
  Big5, Windows-1252 and Latin-1. Recursive listings are bounded, permission-checked
  and return leading-slash relative paths with forward slashes.
- Processes: authorized cwd, separate live stdout/stderr callbacks on the polling
  thread, complete UTF-8 chunks, separate final strings and actual nonzero exit
  status. Spawn/invalid-text/limit/cancel failures reject. Combined output is
  limited to 8 MiB, runtime to 90 seconds; inherited handles are explicitly limited
  to stdio. Tests prove a callback unblocks a still-running child, handle split
  Unicode, reject invalid/excessive output and cancel a running child.
- HTTP redirect destinations obey manifest domain restrictions.
- Release outputs moved to artifacts/. Windows treated the old release/ path as
  MSVC's Release/ binary directory; do not use that old location for new packages.

## 4. Evidence and its limits

| Area | Most recent evidence | Limit |
| --- | --- | --- |
| Builds | Both complete Release toolchain builds passed after process/file changes | Existing warnings remain; not a portability proof for all Windows versions |
| CTest | Both: core, library, extensions, platform, thumbnails, actual WebSocket passed | engine_smoke skips without a fixture |
| Real playback | Test-Playback.ps1 passed both builds after the latest code changes | Separate actual GPU tests, not the skipped CTest entry |
| HTTP/services | Test-Extensions.ps1 passed both toolchains during continuation | Local HTTP/XML-RPC/subtitle/catalog fixtures, not live accounts |
| Plugin windows | Test-PluginWindows.ps1 passed both during continuation | WebView2, authenticated player messages, native key-up/mouse callbacks |
| Native GUI | 40 assertions passed per toolchain in the preceding resume | Before the final process/file-only changes; not every preference or menu |
| Updater | Isolated valid update, corrupt manifest, forced copy failure rollback, pending-file preservation and scratch cleanup passed | Does not establish installer lifecycle or a public update channel |
| Preferences | Source consumers found for all Preferences fields | Usage audit only, not exhaustive behavioral coverage |
| Packaging | Latest portable/source/NSIS outputs completed; asset SHA-256 values checked | Installer was compiled, not installed/uninstalled; outputs are unsigned |

Useful evidence locations:

- .cache/continue5-mingw.log and .cache/continue4-msvc.log: latest build logs.
- .cache/continue-playback-mingw.log and .cache/continue-playback-msvc.log.
- .cache/continue-package.log: latest completed release packaging.
- .cache/resume2-gui-mingw.log and .cache/resume2-gui-msvc.log point to GUI reports.
- .cache/gui-smoke/*/summary.json and captures; .cache/plugin-windows/ fixtures.
- build/<preset>/Testing/Temporary/LastTest.log for the latest CTest invocation.

Do not treat an older result as verification of subsequent changes. Run checks
appropriate to the next change; do not repeatedly rebuild unchanged code merely
to show activity.

## 5. Remaining work, in execution order

### A. Establish the full source-to-target parity matrix, then close real gaps

There is no exhaustive, reviewed feature-by-feature IINA parity matrix yet.
The summaries above must not become a false definition of complete transplant.

1. Inventory user-visible behavior in the pinned IINA source: main/player windows,
   menus/actions/shortcuts, preferences, playlist/history, audio/music/PiP, subtitle
   workflows, inspector/filter controls, window lifecycle, plugins, browser/CLI and
   release integration. Include error, cancellation, persistence and restart paths.
2. Create docs/PARITY_MATRIX.md with reference source, target implementation,
   supported Windows behavior, evidence/test, actual gap and acceptance criterion.
   Separate implemented-and-tested, implemented-unverified, missing, deliberately
   adapted, and unavailable-without-environment states.
3. Trace actions to real behavior. Preferences metadata, menu labels, exported JS
   names and documentation do not establish functional support.
4. Implement the remaining feasible gaps with meaningful regressions. Compare
   visible GUI behavior against the reference, including scaling/layout, keyboard
   interaction and accessibility, rather than only matching method signatures.
5. Keep the matrix and status documents current as work lands. Do not stop after
   writing the matrix; use it to drive implementation through acceptance.

### B. Finish compatibility review against real reference behavior

Known differences or audit targets (not all are confirmed bugs):

- Exact IINA process error/rejection payloads and ERROR_BINARY_NOT_FOUND/RUNTIME
  behavior; missing executables, Unicode paths/arguments, concurrent helpers,
  cancellation/reload during live output and throwing/reentrant stream hooks.
- Unmapped Apple/legacy encodings and directory package/symlink enumeration.
  Confirm whether reference quirks should be preserved or explicitly adapted.
- Native mouse callbacks currently receive an empty object, not AppKit NSEvent
  metadata. Inspect the reference input contract before extending it.
- A global entry runs per independently launched parent, not as an app-wide
  singleton. Establish the required Windows multi-window semantics.
- WebView new-window requests are blocked. Decide from actual plugin needs whether
  a permission-preserving popup adaptation is required.
- Audit permission consistency across network APIs and WebView resources. In
  src/player_features.cpp the WebView constructor receives metadata->domains;
  verify how network-request permission should constrain these domains. Existing
  HTTP/redirect tests do not independently prove WebView permission enforcement.
- Use representative real plugin packages from the reference ecosystem to test
  installation, startup, UI, interaction, updates, disable/reload and uninstall.
  macOS executables/framework calls require explicit Windows adaptations.

### C. Validate the installed product in an isolated Windows environment

Prefer Windows Sandbox, a VM or a disposable Windows account/snapshot. Choosing
an alternate installation folder alone does not isolate HKCU keys or shortcuts.

- Fresh NSIS install and first launch, with/without WebView2 and optional components.
- Paths with spaces/Unicode, per-user permissions, offline/error/cancel cases.
- Install-over-existing-version and running-player behavior; preserve settings,
  history, scripts and unrelated files.
- File Open-with entries, velin: protocol/browser workflow, CLI/single-instance
  forwarding, shortcuts and uninstall registration.
- Uninstall and reinstall: remove only owned integration, preserve user data and
  unrelated files, and do not remove another installation's registrations.
- Exercise update application against an installed copy, including corrupt payload,
  locked-file failure/rollback and relaunch. Existing updater fixtures are a baseline.
- Add an isolated lifecycle regression script if feasible. Never replace the user's
  actual installation or modify live associations as a casual smoke test.
- `installer/Velin.nsi` requires an explicit VERSION; `Package-Release.ps1`
  derives and supplies the application version. Installed lifecycle acceptance
  still requires a disposable environment.

### D. Finish media/platform and usability acceptance

- Sustained playback and repeated open/close/restart/seek/playlist loops; monitor
  memory/handles, worker cleanup, decoder failure and recovery with real media.
- HDR/SDR, hardware-decoding fallback, color management, multiple GPUs/monitors,
  mixed DPI, fullscreen/PiP/music transitions, monitor unplug and display changes.
- Sleep/resume, device changes, media keys/SMTC/taskbar and keyboard/accessibility
  interaction across multiple players.
- Live subtitle authentication/search/download and actual online-media extraction
  with configured credentials/connectivity; do not substitute local fixtures for
  those outcomes or log secrets.
- Installed browser extensions in supported browsers, not only shipped MV3 files.

Some of these require hardware, a disposable OS, service access or user input.
Record exact unavailable prerequisites while continuing independent code work.

### E. Release and reproducibility acceptance

- Run the actual CI matrix from a clean checkout/environment; the workflow exists
  but this session did not run hosted CI. Ensure MinGW/tool prerequisites exist.
- Dependency acquisition must precede configure so helper post-build copies are
  registered. Keep the current order in .github/workflows/build.yml.
- CI currently produces CPack portable ZIPs; it does not validate an installed NSIS
  lifecycle or exercise the full signed/published release pipeline.
- Rebuild both toolchains after final changes, run relevant integration tests, then
  regenerate portable ZIP, NSIS installer, source ZIP, signing reports and manifests.
- Verify archived file hashes and executable identity; extract/run an isolated
  packaged copy on a clean system. Review corresponding-source/license obligations
  for bundled libmpv and dependencies; the application source ZIP excludes runtime/.
- A real publishing endpoint and signing identity have not been supplied. Finish
  everything possible locally, then identify those exact external prerequisites.
  Release manifest URLs are intentionally empty; no release was uploaded.

## 6. Code map for the next agent

| Concern | Main files |
| --- | --- |
| Core/preferences/import/series matching | include/velin/core.hpp, src/core.cpp, src/player_options.cpp |
| Playback/libmpv | src/media_engine.cpp, include/velin/media_engine.hpp |
| GUI/actions/lifecycle | src/player_window.cpp, src/player_actions.cpp, src/player_features.cpp |
| Dialogs/history/tools | src/preferences_dialog.cpp, src/tool_windows.cpp, src/library.cpp |
| Services/plugins/processes/permissions | src/extensions.cpp, src/iina_compat.hpp, include/velin/extensions.hpp |
| Plugin browser/socket bridges | src/plugin_webview.cpp, src/plugin_websocket.cpp |
| Platform/OCR/accessibility | src/platform.cpp, src/platform_ocr.cpp, src/accessibility.cpp |
| Timeline worker/cache | src/thumbnails.cpp, tests/thumbnail_tests.cpp |
| Plugin regressions | tests/extensions_tests.cpp, tests/extension_mock_server.py, scripts/Test-PluginWindows.ps1 |
| Installer/update/release | installer/Velin.nsi, scripts/Apply-Update.ps1, scripts/Test-Update.ps1, scripts/Package-Release.ps1 |
| Build/CI | CMakeLists.txt, CMakePresets.json, cmake/Extensions.cmake, .github/workflows/build.yml |

Reference entry points include IINA PlayerCore.swift, MPVController.swift,
PlayerWindowController.swift/MainWindowController.swift, Preference.swift and
JavascriptAPI*.swift. Use local source as the primary behavioral reference.

## 7. Build and regression commands

Run PowerShell from the target repository. Inspect script parameters first.
Do not run both Test-Extensions invocations concurrently: they share a fixture
server cache/port file. GUI tests should likewise run sequentially.

```powershell
# If runtime acquisition is needed, do this before CMake configure:
./scripts/Get-Mpv.ps1
./scripts/Get-Helpers.ps1
./scripts/Get-WebView2.ps1
./scripts/Get-InstallerTools.ps1

cmake --preset mingw-release
cmake --build --preset mingw-release -j 4
ctest --preset mingw-release
cmake --preset msvc-release
cmake --build --preset msvc-release -- /m:1 /nr:false
ctest --preset msvc-release

./scripts/Test-Extensions.ps1 -BuildDirectory build/mingw-release
./scripts/Test-Extensions.ps1 -BuildDirectory build/msvc-release
./scripts/Test-PluginWindows.ps1 -Executable build/mingw-release/velin.exe
./scripts/Test-PluginWindows.ps1 -Executable build/msvc-release/Release/velin.exe
./scripts/Test-Playback.ps1 -BuildDirectory build/mingw-release -FFmpeg "$PWD/runtime/ffmpeg.exe"
./scripts/Test-Playback.ps1 -BuildDirectory build/msvc-release -FFmpeg "$PWD/runtime/ffmpeg.exe"
./scripts/Test-Update.ps1

$fixture = Get-ChildItem .cache/playback-fixtures -Filter '*chaptered.mkv' | Select-Object -First 1
./scripts/Test-Gui.ps1 -Executable "$PWD/build/mingw-release/velin.exe" -MediaPath $fixture.FullName
./scripts/Test-Gui.ps1 -Executable "$PWD/build/msvc-release/Release/velin.exe" -MediaPath $fixture.FullName

./scripts/Package-Release.ps1 -BuildDirectory build/msvc-release
```

## 8. Environment pitfalls already resolved

- Sandbox sockets can fail with WSAEACCES 10013. WebSocket skip 77 does not prove
  socket functionality. Narrowly scoped sandbox escape allowed real tests to pass.
- MSVC plugin fixtures can fail weakly_canonical with access denied inside the
  sandbox. The same tests passed outside it; preserve real verification.
- NSIS is a 32-bit tool and encountered a system DLL relocation error in the
  sandbox. Package-Release.ps1 passed outside it, without installing anything.
  It uses Start-Process -WindowStyle Hidden with separate stdout/stderr logs;
  direct redirected execution had a PowerShell StandardOutputEncoding failure.
- GCC 16 Release devirtualization miscompiled OS-owned OCR/WinRT vtables.
  CMake applies -fno-devirtualize to platform_ocr.cpp and plugin_webview.cpp.
  Preserve this unless actual OCR/WebView tests establish a safe replacement.
- The QuickJS bootstrap raw literal is split to satisfy MSVC's literal-size limit.
- Tests use VELIN_DATA_DIR isolation. Do not kill unrelated user player processes.
- Verify resolved absolute paths stay inside the intended workspace before any
  recursive removal/move. Preserve runtime/, .cache/ and current build evidence.

## 9. Current deliverables and completion gate

Latest executable packages are under build/msvc-release/artifacts/:

- Velin-0.2.0-windows-x64.zip
- Velin-0.2.0-windows-x64-setup.exe
- Velin-0.2.0-source.zip
- releases.json, SHA256SUMS, application-signing.json, installer-signing.json

They are unsigned and unpublished. All three asset hashes were checked after
successful packaging. This regenerated handoff and the associated status wording
correction were written AFTER that packaging run; archives therefore contain the
previous documentation snapshot. Code/executable content was not changed by this
handoff-only task. Regenerate archives after subsequent implementation/documentation
changes before presenting them as a final release.

Completion requires an evidence-backed parity matrix, closure of feasible code
gaps, installed-product lifecycle validation, packaged-product acceptance and
honest disposition of hardware/service/signing/publishing prerequisites. Do not
claim end-to-end transplant completion while these remain unverified. The next
agent should begin with section 5A, then implement and validate the highest-impact
open gaps rather than repeating already-passing smoke tests indefinitely.
