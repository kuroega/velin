# Vendored dependencies

- QuickJS-ng v0.11.0, https://github.com/quickjs-ng/quickjs/tree/v0.11.0
  - Source ZIP SHA-256: `424e50a6513b0ea9fe72ecbba3cb31e524f78d3a4ce7f8ed894aec4a446ca3b4`
  - MIT license retained in `quickjs/LICENSE`.
- nlohmann/json v3.12.0, https://github.com/nlohmann/json/tree/v3.12.0
  - MIT license retained in `nlohmann/LICENSE.MIT` and the single header.
- miniz 3.0.2, https://github.com/richgel999/miniz/tree/3.0.2
  - MIT license retained in `miniz/LICENSE`.
  - `miniz_export.h` is the static-library export configuration.

QuickJS is embedded without its `std` or `os` modules. Plugins receive only the
explicit, permission-checked host APIs. Each plugin has a separate runtime,
64 MiB memory limit, 512 KiB stack limit, and bounded execution time.
