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

Export coverage is separate from functional portability. Kernel file-I/O regressions are tested on Windows and Linux; the Windows fork backend currently requires an exact Windows 11 runtime profile and fails the mandatory fork tests on Windows Server 2022. See [kernel validation and remaining limitations](docs/dev/debt/libkernel.md) and [portability CI](https://github.com/barnacasiantiberiu/AnyPS5/actions/workflows/portability.yml).

Unsupported or unexpected states strictly throw `std::runtime_error`. `what()` is printed to stderr and the process terminates.

The [shader recompiler](core/shader/recompiler/Recompiler.cpp) successfully produces SPIR-V (validated via [Spirv-Tools](3rdparty/SPIRV-Tools) when built with `ANYPS5_ENABLE_SPIRV_TOOLS`).

## Compatibility

Per-game progress (stage reached, open blockers, libraries and GPU coverage) is tracked on the [blocker map](https://alxcbtw.github.io/anyps5-roadmap/), a community project built from public GitHub data ([source](https://github.com/AlxcBTW/anyps5-roadmap)). Test reports go in the [Game bring-up](https://github.com/boykopovar/AnyPS5/discussions/categories/game-bring-up) discussions, one thread per title.

## Input mapping

SDL-mapped game controllers are supported, including analog sticks and triggers. Keyboard and mouse controls can be configured with an `anyps5-input.ini` file. See [input mapping](docs/user/INPUT_MAPPING.md) for the supported devices and configuration format.

## Disclaimer

This project is intended for interoperability, research, preservation, and compatibility purposes. It does not include, distribute, or require copyrighted software, firmware, cryptographic keys, or proprietary libraries. Users are responsible for ensuring that any binaries used with this project are obtained and used in accordance with applicable laws and their respective license terms.

## License

This project is licensed under the GNU General Public License version 2 only.
