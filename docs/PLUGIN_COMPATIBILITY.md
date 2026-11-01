# Windows plugin compatibility

Updated: 2026-11-01. Target: Velin 0.2.0.

Velin runs a Windows adaptation of the IINA JavaScript API in QuickJS-ng.
Exporting an API name does not establish compatibility with every macOS plugin.
The reference for this audit is the sibling IINA checkout identified in PORTING.md.

## Audited behavior

| Area | Windows behavior and support boundary |
| --- | --- |
| Entry points | Player `entry`, `globalEntry`, or global-only manifests are accepted. Both paths must resolve inside the package. Child players suppress global entries. A global entry runs per independently launched parent player; this is not a separate singleton process. |
| Text files | Writes use UTF-8. Reads accept UTF-8, UTF-16/32 variants and IINA endian aliases, ASCII, ISO Latin-1/2, Windows-1250 through 1258, Big5, Shift-JIS, GBK/GB18030, Korean and Mac Roman names mapped in the implementation. Windows code-page availability applies. Invalid input and unsupported names raise an error. Binary handles support bounded reads/writes, seeking and offsets. Apple-only and other unmapped encodings still need conversion. |
| Directory listing | `includeSubDir` enables recursive listing, bounded to 10,000 entries. Paths use a leading slash and are relative to the requested directory, matching the IINA listing shape; recursive paths use forward slashes. Every result is checked against package/data permissions. Directory symlinks are not followed recursively. |
| Processes | Requires file-system permission. Executable arguments are passed directly to CreateProcess. Optional cwd is resolved through plugin permissions. Output must be UTF-8, is bounded to 8 MiB and has a 90-second deadline. stdout and stderr have separate live callbacks on the polling/UI thread and separate final strings. Completion resolves with the actual exit status, including nonzero exits. Callbacks receive complete UTF-8 sequences even when the OS splits a character across pipe reads. Spawn, decoding, output-limit and cancellation failures reject. Handle inheritance is restricted to the three child stdio handles; cancellation terminates the child process. Cross-stream callback order is not guaranteed. |
| Menus | Nested menu groups, enabled state, selected state and separators are preserved in native menus. Disabled ancestors disable descendants. Menus are bounded to 1,000 entries and 16 levels. Playlist builders receive selected filenames; rebuilding expires old context command identifiers. |
| Input | Native key-down/key-up and mouse-down/up/drag events dispatch to plugin handlers. Key names follow Windows bindings. Returning true consumes the event. Mouse callbacks receive `x` and `y` in DPI-independent client coordinates with a bottom-left origin, `clickCount` (one or two), and `pressure` (1 while pressed, 0 on release). This approximates IINA's event fields for an ordinary Windows mouse; pressure-sensitive devices and click counts above two are not represented. |
| Reload | Reload requested during JavaScript execution is deferred until poll. Queued callbacks belonging to the old runtime are discarded when reload occurs. File handles and runtime resources are released with the old runtime. |
| Networking | HTTP requests and redirect targets are checked against allowedDomains. Exact names and wildcard subdomains are supported. Plugin requests require network permission and a nonempty domain list. Local fixtures exercise allowed requests and a redirect to a denied hostname. |
| Web views | WebView2 virtual-host mapping serves package HTML. WebSocket handshakes bypass WebView2's resource filter, including in frames and workers. To prevent bypassing restricted network grants, HTML views are disabled unless the running plugin has the `network-request` permission and explicitly declares `*` in `allowedDomains`. This also disables offline and domain-restricted plugin HTML views; an on-screen notice and plugin error explain the restriction. A wildcard grant permits unrestricted WebSocket connections. HTTP(S) navigation and resource filtering still use the newer WebView2 all-source API and fail closed on older runtimes. WebView device permissions are denied and new-window popups are suppressed. Bridge messages are accepted from package pages and the host's simple-mode about:blank document. `scripts/Test-WebViewPermissions.ps1 -IncludeWebSockets` verifies the unrestricted case and that restricted views are refused. Restoring restricted or offline views requires an enforceable WebSocket policy. Multiple arbitrary browser windows are not supported. |

## Regression coverage

The C++ extension tests cover script execution, input dispatch, UTF-16 decoding,
relative recursive listing, legacy text decoding, process cwd, live split streams,
nonzero exit, partial UTF-8, invalid bytes, output limits and cancellation,
menu hierarchy, disabled actions, stale context
identifiers, global-only manifests and deferred reentrant reload. The local HTTP
fixture also exercises denied redirect targets. Test-PluginWindows.ps1 posts real
Win32 input messages and checks mouse coordinates, click counts and pressure, in addition to WebView2 HTML
messages and authenticated parent/child player messages.

The preference audit found consumers for all Preferences fields, including
history window dimensions in preferences_dialog.cpp. This is a source-level
usage audit, not an exhaustive behavioral test of every setting. Engine mappings
are covered by core tests; device-dependent effects need the relevant hardware.

## Remaining compatibility work

Unmapped Apple/legacy encodings, exact IINA process error payloads and directory
package-enumeration details still need individual compatibility checks. Apple framework integrations,
AppKit event objects and platform-specific executables need Windows adaptations.
An arbitrary third-party plugin still needs its own compatibility test.
