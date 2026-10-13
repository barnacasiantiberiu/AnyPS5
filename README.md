# About

Tool for automatic executables porting to Linux and Windows.

Includes a [relinker](core/relinker) that converts executable to the target system's native format and implementations of [system prx libraries](core/libs/prx) suitable for dynamic linking. No emulation or separate runtime process.

[Usage](docs/user/USAGE.md), [Build instructions](docs/dev/BUILD.md), [Architecture](docs/dev/ARCHITECTURE.md), [Technical debt of the project](docs/dev/TechnicalDebt.md), [code style conventions](docs/dev/CONVENTIONS.md), [contributing](CONTRIBUTING.md)

## Status

<!-- anyps5-status:start -->

<!-- Source snapshot: libraries 3213/3593 (89.42%); shaders 1166/1166 (100.00%). -->

[![libraries](https://barnacasiantiberiu.github.io/AnyPS5/badge-libraries.svg?v=445e2b65cf1fd3a3)](https://barnacasiantiberiu.github.io/AnyPS5/) [![shaders](https://barnacasiantiberiu.github.io/AnyPS5/badge-shaders.svg?v=445e2b65cf1fd3a3)](https://barnacasiantiberiu.github.io/AnyPS5/)

[![progress map](https://barnacasiantiberiu.github.io/AnyPS5/progress.svg?v=445e2b65cf1fd3a3)](https://barnacasiantiberiu.github.io/AnyPS5/)

[![progress history](https://barnacasiantiberiu.github.io/AnyPS5/history.svg)](https://barnacasiantiberiu.github.io/AnyPS5/history.html)

<sub>* System libraries: percentage of the functions known to the project so far (declared in [core/libs/prx](core/libs/prx)), not of every PS5 system function. The total grows as more functions are declared.</sub>

<!-- anyps5-status:end -->

Unsupported or unexpected states strictly throw `std::runtime_error`. `what()` is printed to stderr and the process terminates.

The [shader recompiler](core/shader/recompiler/Recompiler.cpp) successfully produces SPIR-V (validated via [Spirv-Tools](3rdparty/SPIRV-Tools) when built with `ANYPS5_ENABLE_SPIRV_TOOLS`).

## Compatibility

Guest module conversion preserves original module files alongside converted modules so engine plugin discovery can supply the required initialization arguments. This prevents the missing-path PSN version error observed in Unity titles; it does not establish full game compatibility.

Kernel address waits defer guest signal delivery while internal synchronization locks are held. Windows and Linux regression tests cover repeated stop/resume handlers during concurrent waits and wakes. This removes the reproduced Valheim startup deadlock; later GPU compatibility is tracked separately. Export coverage is unchanged.

Shader preparation transactions also defer guest signals until their writer lock and staged objects are released, including rollback and nested preparation.

The AGC driver can emulate fragment barycentrics after triangular tessellation on GPUs without `VK_KHR_fragment_shader_barycentric`. Render regressions check perspective and linear weights against triangle reference pixels. This addresses the subsequent Valheim shader-stage failure; full gameplay and performance remain unverified.

Depth sampling supports single-mip cube and 2D-array views assembled from separately rendered depth faces. GPU regression coverage checks face selection and refresh after a new depth write. This addresses the depth-cubemap failure observed after Valheim's intro: the main menu now opens and menu audio is audible. Intro audio, low frame rate and full gameplay remain under investigation. On the tested GTX 1050 Ti with driver 582.78, the 3D menu still runs around 2-3 FPS after pipeline warmup; profiling identifies GPU draw execution as the main cost. Export coverage is unchanged.

Barycentric fallback geometry shaders are reused by interface and compilation target, avoiding repeated generation and SPIR-V optimization during drawing. Device capabilities and limits are checked before every cache lookup. The focused cache regression passes on Windows and Linux. This does not resolve the GPU draw cost observed in Valheim or its first-use pipeline compilation pauses. Export coverage is unchanged.

Per-game progress (stage reached, open blockers, libraries and GPU coverage) is tracked on the [blocker map](https://alxcbtw.github.io/anyps5-roadmap/), a community project built from public GitHub data ([source](https://github.com/AlxcBTW/anyps5-roadmap)). Test reports go in the [Game bring-up](https://github.com/boykopovar/AnyPS5/discussions/categories/game-bring-up) discussions, one thread per title.

The [Desktop launcher](docs/user/DESKTOP.md) provides saved resolution presets under Settings > Graphics. Supported Unity startup configurations are updated reversibly at launch; other titles retain their original resolution. A Windows runtime trace confirms 1920x1080 video output for Valheim with the 1080p preset. This is not universal internal render scaling or a confirmed FPS fix. Export coverage is unchanged.

The Desktop loading overlay reports real shader/pipeline work during presentation pauses, with completed step counts and explicit no-progress/stale-status messages. It hides when frames resume or the game loses focus. No total or completion time is assumed. See [Desktop loading status](docs/user/DESKTOP.md#loading-overlay) for supported window systems and verification. This does not resolve the remaining Valheim stall or GPU performance issue. Export coverage is unchanged.

## Input mapping

SDL-mapped game controllers are supported, including analog sticks and triggers. Keyboard and mouse controls can be configured with an `anyps5-input.ini` file. See [input mapping](docs/user/INPUT_MAPPING.md) for the supported devices and configuration format.

## Disclaimer

This project is intended for interoperability, research, preservation, and compatibility purposes. It does not include, distribute, or require copyrighted software, firmware, cryptographic keys, or proprietary libraries. Users are responsible for ensuring that any binaries used with this project are obtained and used in accordance with applicable laws and their respective license terms.

## License

This project is licensed under the GNU General Public License version 2 only.
