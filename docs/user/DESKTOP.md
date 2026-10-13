# AnyPS5 Desktop

The launcher source is tracked under tools/desktop so releases can be rebuilt. Version 0.5.2 restores original game resolution and disables the unsafe output-width presets in Settings > Graphics.

## Build on Windows

Install Node.js 22 or newer, then run npm ci and npm run package in tools/desktop. package-lock.json pins the Electron, packager and test dependencies with registry integrity hashes. The output is dist/AnyPS5 Desktop-win32-x64/AnyPS5 Desktop.exe; keep the whole output directory. Engine binaries are configured separately and are not bundled.

For portable data, create portable-data beside the packaged executable before opening it, or set ANYPS5_DESKTOP_DATA to a writable profile directory. Existing profiles can be copied there while the launcher is closed. Without either choice the standard application-data directory is used.

## Resolution

Original game resolution is the only enabled option. Previously saved 720p, 900p, 1080p, 1440p and 4K selections migrate to original resolution. Output-width overrides caused corrupted Valheim rendering because they did not resize the internal render targets, depth buffers and dependent passes together. No universal internal render scaling or detail presets are implemented. Use a game's own graphics settings where available.

On the next launch, an existing .anyps5-resolution.json recovery file restores the exact original bytes of the managed Unity boot.config. Configurations without a recovery file are left unchanged. Unexpected external edits are preserved and reported rather than overwritten. Do not launch the same installation with another launcher while restoring its configuration.

## Focused verification

npm run test:graphics runs generated-file functional tests without a game or GPU, on Windows and Linux. npm run test:graphics-ui checks the packaged or development UI (ANYPS5_TEST_APP can select a package). Linux packaging and gameplay are not established by these tests. See tools/desktop/README.txt for the other launcher features.

## Loading overlay

Version 0.5.1 automatically displays a click-through status card above the focused game when presentation pauses. Launch from Desktop with updated libSceAgcDriver and libSceVideoOut libraries. The card shows real SPIR-V compilation and Vulkan graphics/compute pipeline activity, completed steps since launch, and seconds without a new presented frame. The total is unknown because games discover shaders during play; the animated band is an activity indicator, never a percentage or time estimate. A pipeline call may reuse the driver's cache and still counts as a completed step.

After 15 seconds without a completed step, the band stops animating and warns that a slow driver call or stall is possible. A pause with no active compilation is explicitly identified as waiting for a frame. Stale telemetry is reported separately. The card hides when frames resume, the game loses focus, or the game exits. This is diagnostic feedback; it does not repair loading stalls or improve GPU frame times.

The launcher passes a unique ANYPS5_LOADING_STATUS path to each game. Engine reporters publish small snapshots four times per second on independent threads, so they do not need the rendering thread to advance. These snapshots are optional and have no game-state effect; ordinary direct launches do not start reporters. Keep Desktop open to see the overlay. Normal exits remove the session files; closing Desktop while a detached game runs can leave two small session files in the profile's loading-status directory.

Native window tracking supports Windows and Linux X11. Wayland, exclusive fullscreen, and Linux Desktop game launching/packaging are not validated. Unsupported native window systems leave the overlay hidden. Older engine libraries do not provide telemetry and will not display a card. Windows mixed-DPI positioning uses Electron's screen-to-DIP conversion.

Focused checks: CTest loading_progress and loading_window exercise concurrent work accounting, errors and real native-window telemetry; Linux loading_window requires an X11 display (xvfb-run is sufficient). npm run test:loading checks activity/stall/recovery decisions on Windows and Linux. npm run test:loading-ui checks the Electron card using generated engine-format snapshots; it is not a gameplay benchmark.
