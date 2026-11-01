# Third-party notices

## IINA

Velin is a Windows C++ port of the playback model and user experience of
[IINA](https://github.com/iina/iina), an open-source macOS media player.
The reference checkout used for this port is IINA commit
`1b05edd0f7524ecc95286af75529007d4cfe1124`.

Copyright (C) 2016-present IINA developers and contributors.
IINA is distributed under the GNU General Public License, version 3.
Velin is also distributed under GPL-3.0-or-later; see [LICENSE](LICENSE).
The AppKit interface, Apple frameworks, IINA application icon, and macOS
binary dependencies are not included in this Windows implementation.

## mpv client API header

`third_party/mpv/client.h` is copied from the reference IINA dependency headers.
Copyright (C) 2017 the mpv developers. It is licensed under the ISC license;
the full copyright, permission, and warranty notice is preserved in that file.
The header license does not replace the license of the mpv runtime.

## Optional libmpv playback runtime

Velin loads libmpv dynamically. `scripts/Get-Mpv.ps1` obtains the x86-64 runtime
from the third-party Windows build project
[shinchiro/mpv-winbuild-cmake](https://github.com/shinchiro/mpv-winbuild-cmake),
one of the providers listed on the
[official mpv installation page](https://mpv.io/installation/).
The archive is not stored in Velin's source repository.

The exact release, download URL, archive SHA256, and extracted DLL SHA256 are
recorded in [config/mpv-runtime.json](config/mpv-runtime.json).
This pin selects release `20261021`, mpv revision `e471496c56`, and the baseline
x86-64 build rather than the build that requires x86-64-v3 CPU features.

Upstream projects and corresponding source locations:

- [mpv source at the pinned revision](https://github.com/mpv-player/mpv/tree/e471496c56)
- [mpv copyright and licensing](https://github.com/mpv-player/mpv/blob/e471496c56/Copyright)
- [Windows build scripts at the release tag](https://github.com/shinchiro/mpv-winbuild-cmake/tree/20261021)
- [Windows release and build provenance](https://github.com/shinchiro/mpv-winbuild-cmake/releases/tag/20261021)
- [FFmpeg source](https://git.ffmpeg.org/ffmpeg.git)
- [libass source](https://github.com/libass/libass)
- [libplacebo source](https://code.videolan.org/videolan/libplacebo)

mpv and the components compiled into a particular binary retain their upstream
licenses. The mpv project documents its GPL/LGPL build choices in its copyright
file. The build scripts record dependency selection and build options. A binary
distributor must provide the applicable notices and corresponding source for
the actual runtime they distribute, including its dependencies and build scripts.
A Velin source archive alone is not the corresponding source for libmpv.

## Windows platform components

Velin uses the Windows SDK APIs, including Win32, Direct2D, DirectWrite, and
Windows shell dialogs. Segoe UI fonts are provided by Windows. No Microsoft
SDK, system DLLs, or font files are bundled in this repository.
