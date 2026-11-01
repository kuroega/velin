# Implementation status

Updated: 2026-11-01 (Asia/Taipei). Development version: 0.2.0.
Velin implements a native Windows adaptation of IINA; full feature and plugin parity is not established.

## Integrated behavior

Playback, track/device menus, chapters, playlist editing and persistence, history,
preferences and IINA preference import, PiP, music layout, crop, video adjustments,
screenshots, OCR, timeline previews, CLI and single-instance IPC are implemented.
Windows integrations include MSAA/UI Automation, taskbar and system media controls.
QuickJS plugins support permissions, packages, catalog/update UI, WebView2 bridges,
WebSockets and authenticated cross-player messages. Subtitle services support
OpenSubtitles and Assrt. Browser extension assets and release/update scripts exist.

The latest changes connect startup preferences, pause-on-open, chapter markers,
numbered related-file discovery and plugin key-up/mouse callbacks. Related files
match case-insensitive filename stems with digit runs replaced by placeholders;
the selected file stays first and matching siblings follow in natural order.
URLs, unnumbered names and explicit multi-file selections do not expand.

## Verified in the latest resume

- Session follow-up on 2026-11-01: the dedicated shortcut editor supports
  add/update/remove with draft-only changes until Preferences Apply/OK. GUI
  regression passed 55 assertions on both release toolchains, including editor
  changes and cancellation. Full CTest passed outside the sandbox on both
  toolchains (six tests passed; fixture-less `engine_smoke` skipped).
  `HANDOFF.md` records the reports and remaining environment-dependent checks.

- After the further continuation, both release builds and their CTest suites
  passed outside the restricted sandbox, with the no-fixture engine test skipped.
  Separate headless `Test-Playback.ps1` runs passed on both toolchains. The GUI
  regression passed 51 assertions on each, including crop application and reset.
- MinGW and MSVC full application builds and core/library/extension/platform CTest checks.
- Earlier real GPU playback, subtitles, playlist navigation and engine restart
  integration runs predate the 2026-11-01 continuation.
- Native GUI regression: 48 assertions on each toolchain, including inspector
  watches, live values, unavailable properties and restoration after restart.
  Reports: `.cache/gui-smoke/run-20261101-032232-658/summary.json` (MinGW) and
  `.cache/gui-smoke/run-20261101-032245-342/summary.json` (MSVC).
- Real WebView2, parent/child plugin messages and native key-up/mouse callbacks on both toolchains.
- Local HTTP/XML-RPC/subtitle/catalog fixture contracts and real WebSocket tests
  (localhost tests require execution outside the socket-restricted sandbox).
- Isolated portable updater: valid install, corrupt manifest rejection, forced
  staging collision rollback, pending-file preservation and scratch cleanup.

## Latest completed regression work

- Added crop aspect presets, a custom aspect prompt, and playback menu entries
  for previous frame, go to time and jump to beginning. Geometry tests cover
  valid and invalid ratios; MinGW GUI regression applied and cleared a 4:3 crop
  through mpv. Size presets, aspect lock and further interaction checks remain.
- IINA preference import respects the initial-volume switch and updates only
  supplied SPDIF flags. Core tests cover both behaviors.
- CLI launches and single-instance forwarding resolve relative media paths in
  the sender's working directory; platform tests cover Unicode and quoting.
- The portable updater handles empty restart arguments in Windows PowerShell
  5.1; its regression passes in both Windows PowerShell and PowerShell 7.
- The media inspector can add and remove custom mpv property watches. It refreshes
  values every 500 ms and saves the list atomically in the application data
  directory. Read or save failure does not overwrite the saved list. A null mpv
  value and an unavailable property share the same display text.
- Actual FFmpeg thumbnail generation and WIC BGRA decode, dimensions/pixel checks,
  persistent cache hits with a non-decoding helper, latest-request/resize and cancellation.
- Cleared unconsumed stale thumbnails when a newer request arrives.
- Deferred reload from inside an active JavaScript callback, menu builder closures,
  expired context identifiers, disabled items and inherited disabled menu groups.
- Preserved menu hierarchy in native Win32 popup menus.
- UTF-16 file decoding, bounded recursive directory listing, authorized process cwd,
  global-only manifests and suppression of global entries in child players.
- HTTP redirect domain enforcement using an actual local server on both toolchains.
- Blocked WebView2 new-window popups; unmapped encodings reject explicitly.
  Live process stream hooks and additional legacy encodings were subsequently
  implemented and tested as described below.
- Source audit found consumers for all Preferences fields. This does not establish
  behavioral coverage for every preference or device-dependent effect.
- Release packaging now uses artifacts/, avoiding Windows release/Release collision.
- Separate live process stdout/stderr callbacks, split UTF-8 handling, actual
  nonzero exit status, output-limit rejection and cancellation regressions.
- IINA-style relative listing paths and additional legacy/UTF-32 text encodings.
- Child processes inherit only their explicit stdio handles, avoiding accidental
  pipe inheritance between simultaneous helpers.

The normal engine CTest entry skips without a fixture. Separate Test-Playback.ps1
runs passed actual GPU playback on both toolchains; those outcomes do not rely on
the skip. WebSocket CTest checks ran outside the restricted socket sandbox.

See [plugin compatibility](PLUGIN_COMPATIBILITY.md) for precise API differences.

## Remaining verification and compatibility work

- The working [parity matrix](PARITY_MATRIX.md) records concrete missing and
  unverified reference workflows. It is not an exhaustive acceptance review.
- WebView WebSocket handshakes bypass the HTTP resource filter. As a fail-closed
  mitigation, plugin HTML views require network permission and an explicit `*`
  domain grant. This disables offline and restricted-domain HTML views until a
  reliable enforcement mechanism is available; see the compatibility document.
- Verify additional unmapped encodings, IINA process error payloads and package
  enumeration details for particular third-party plugins. Native mouse events
  expose adapted coordinates, click count and basic pressure, not full AppKit events.
- Test installer lifecycle, long playback, HDR, multiple monitors/GPUs and recovery
  in appropriate environments. Local fixtures do not verify live subtitle accounts.
- Verify inspector invalid-input and persistence errors, resizing and screen-reader
  interaction; the automated watch checks do not establish those behaviors.
- Supply a public release endpoint and signing identity before a signed release.

Old 0.1 archives are stale. Do not infer macOS plugin compatibility from exported
API names or describe unsigned artifacts as signed.
