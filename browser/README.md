# Open in Velin

Use Velin's installer, or its View > Set up file associations command, to register
the `velin:` protocol. This does not change default applications automatically.

For Chrome or Edge, enable Developer mode on the browser's Extensions page and
load this directory as an unpacked extension. For Firefox development, copy the
directory, replace `manifest.json` with `manifest.firefox.json`, and use
`about:debugging` to load it temporarily. Public browser-store distribution
requires the publisher's browser-store account and signing/review process.

The toolbar opens the current HTTP(S) page. The context menu opens media or link
targets and can append them to the existing playlist. The browser may ask before
launching the registered desktop application. Website extraction depends on the
bundled yt-dlp and site access; ordinary direct media URLs need no extraction.

The extension sends the selected URL through the local operating-system protocol
handler. It does not collect history, upload page contents, or contact a server.
