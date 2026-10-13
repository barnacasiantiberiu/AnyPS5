# AnyPS5 Desktop

The launcher source is tracked under tools/desktop so releases can be rebuilt. Version 0.5.0 adds Settings > Graphics with original resolution, 720p, 900p, 1080p (default), 1440p and 4K.

## Build on Windows

Install Node.js 22 or newer, then run npm ci and npm run package in tools/desktop. package-lock.json pins the Electron, packager and test dependencies with registry integrity hashes. The output is dist/AnyPS5 Desktop-win32-x64/AnyPS5 Desktop.exe; keep the whole output directory. Engine binaries are configured separately and are not bundled.

For portable data, create portable-data beside the packaged executable before opening it, or set ANYPS5_DESKTOP_DATA to a writable profile directory. Existing profiles can be copied there while the launcher is closed. Without either choice the standard application-data directory is used.

## Resolution

The selected preset is saved with the launcher settings and applied at launch to supported Unity PS5 app0/Media/boot.config or app0/boot.config width settings. The original converted-game configuration is retained in an adjacent .anyps5-resolution.json recovery file. Selecting original resolution restores it at the next launch. Other games are launched unchanged with an explicit message; no general render-target scaling or detail presets are implemented. A title may override its internal rendering size, so a requested resolution is not a measured rendering size.

Do not change graphics settings for a game still running outside this launcher. Changes also affect direct launches of that converted installation until original resolution is restored. The unconverted source files are not changed. Unexpected edits to a managed configuration are preserved and reported rather than overwritten.

## Focused verification

npm run test:graphics runs generated-file functional tests without a game or GPU, on Windows and Linux. npm run test:graphics-ui checks the packaged or development UI (ANYPS5_TEST_APP can select a package). Linux packaging and gameplay are not established by these tests. See tools/desktop/README.txt for the other launcher features.
