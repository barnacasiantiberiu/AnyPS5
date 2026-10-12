# About

Tool for automatic executables porting to Linux and Windows.

Includes a [relinker](core/relinker) that converts executable to the target system's native format and implementations of [system prx libraries](core/libs/prx) suitable for dynamic linking. No emulation or separate runtime process.

[Usage](docs/user/USAGE.md), [Build instructions](docs/dev/BUILD.md), [Architecture](docs/dev/ARCHITECTURE.md), [Technical debt of the project](docs/dev/TechnicalDebt.md), [code style conventions](docs/dev/CONVENTIONS.md), [contributing](CONTRIBUTING.md)

## Status

<!-- anyps5-status:start -->

Status of this fork, generated from the code in this commit.

| Component | Counted | Known total | Progress |
| --- | ---: | ---: | ---: |
| System library functions* | 3081 | 3537 | 87.11% |
| Decoded shader instructions* | 1166 | 1166 | 100.00% |

**456 known library functions remain marked unimplemented.**

*These are static source counts, not test coverage or a guarantee of game compatibility. The library total includes only declarations known to this project. Shader progress counts recognized instructions, not verified GPU execution. See the [counting rules](docs/dev/PROGRESS.md).

<details>
<summary>System libraries: status by module</summary>

| Library | Counted | Known total | Progress |
| --- | ---: | ---: | ---: |
| libc | 707 | 712 | 99.30% |
| libkernel | 527 | 577 | 91.33% |
| libkernel_sys | 7 | 10 | 70.00% |
| libkernel_web | No declared exports | — | — |
| libReactNative.Modules.Vsh | 0 | 4 | 0.00% |
| libReactNative.Modules.Vsh.WebAppLauncher | 0 | 7 | 0.00% |
| libSceAcm | 8 | 8 | 100.00% |
| libSceAgc | 207 | 229 | 90.39% |
| libSceAgcDriver | 40 | 51 | 78.43% |
| libSceAgcVsh | No declared exports | — | — |
| libSceAjm | No declared exports | — | — |
| libSceAjm.native | 30 | 35 | 85.71% |
| libSceAjmi | No declared exports | — | — |
| libSceAmpr | 135 | 148 | 91.22% |
| libSceAppContent | 11 | 13 | 84.62% |
| libSceAudio | No declared exports | — | — |
| libSceAudio3d | 13 | 13 | 100.00% |
| libSceAudiodec | 6 | 6 | 100.00% |
| libSceAudiodec.native | Shared with libSceAudiodec | — | — |
| libSceAudioIn | 6 | 8 | 75.00% |
| libSceAudioOut | 32 | 47 | 68.09% |
| libSceAudioPropagation | 22 | 23 | 95.65% |
| libSceAvPlayer | 28 | 28 | 100.00% |
| libSceAvPlayer.native | Shared with libSceAvPlayer | — | — |
| libSceBluetoothHid | 5 | 8 | 62.50% |
| libSceCdlgG2P | 7 | 7 | 100.00% |
| libSceCdlgPlayerReview | 7 | 7 | 100.00% |
| libSceCesCs | 5 | 5 | 100.00% |
| libSceCommonDialog | 2 | 2 | 100.00% |
| libSceContentDelete | 2 | 4 | 50.00% |
| libSceContentExport | 7 | 10 | 70.00% |
| libSceContentSearch | 2 | 11 | 18.18% |
| libSceConvertKeycode | 2 | 4 | 50.00% |
| libSceErrorDialog | 6 | 6 | 100.00% |
| libSceFiber | 13 | 13 | 100.00% |
| libSceFont | 120 | 195 | 61.54% |
| libSceFont-module | Shared with libSceFont | — | — |
| libSceFont.native | Shared with libSceFont | — | — |
| libSceFontFt | 23 | 42 | 54.76% |
| libSceFontFt-module | Shared with libSceFontFt | — | — |
| libSceGameLiveStreaming | 3 | 4 | 75.00% |
| libSceGameUpdate | 6 | 7 | 85.71% |
| libSceHmd | 9 | 9 | 100.00% |
| libSceHmd2 | 1 | 29 | 3.45% |
| libSceHttp | 62 | 63 | 98.41% |
| libSceHttp2 | 36 | 43 | 83.72% |
| libSceIme | 13 | 14 | 92.86% |
| libSceImeBackend | Shared with libSceIme | — | — |
| libSceImeDialog | 8 | 8 | 100.00% |
| libSceJobManager | 11 | 12 | 91.67% |
| libSceJpegDec | 5 | 5 | 100.00% |
| libSceJpegEnc | 4 | 4 | 100.00% |
| libSceJson2 | 108 | 116 | 93.10% |
| libSceKeyboard | 6 | 6 | 100.00% |
| libSceLibcInternal | 10 | 10 | 100.00% |
| libSceLoginDialog | 7 | 7 | 100.00% |
| libSceMbus | 4 | 4 | 100.00% |
| libSceMouse | 4 | 5 | 80.00% |
| libSceMsgDialog | 10 | 10 | 100.00% |
| libSceMsgDialog.native | 10 | 10 | 100.00% |
| libSceNet | 48 | 50 | 96.00% |
| libSceNetCtl | 10 | 10 | 100.00% |
| libSceNetCtlApDialog | 0 | 6 | 0.00% |
| libSceNgs2 | Shared with libSceNgs2.native | — | — |
| libSceNgs2.native | 35 | 38 | 92.11% |
| libSceNpAuth | 5 | 9 | 55.56% |
| libSceNpAuthAuthorizedAppDialog | 7 | 7 | 100.00% |
| libSceNpCommerce | 10 | 11 | 90.91% |
| libSceNpEntitlementAccess | 19 | 21 | 90.48% |
| libSceNpGameIntent | 4 | 4 | 100.00% |
| libSceNpManager | 30 | 34 | 88.24% |
| libSceNpPartner001 | 0 | 2 | 0.00% |
| libSceNpSessionSignaling | 12 | 17 | 70.59% |
| libSceNpTrophy2 | 17 | 18 | 94.44% |
| libSceNpUniversalDataSystem | 41 | 41 | 100.00% |
| libSceNpUtility | 5 | 5 | 100.00% |
| libSceNpWebApi2 | 26 | 28 | 92.86% |
| libSceOpusCeltDec | 6 | 6 | 100.00% |
| libSceOpusDec | 6 | 6 | 100.00% |
| libScePad | 24 | 41 | 58.54% |
| libScePlayerInvitationDialog | 7 | 7 | 100.00% |
| libScePlayerSelectionDialog | 1 | 1 | 100.00% |
| libScePlayGo | 18 | 18 | 100.00% |
| libScePlayGoDialog | 7 | 7 | 100.00% |
| libScePngDec | 5 | 5 | 100.00% |
| libScePngEnc | 4 | 4 | 100.00% |
| libScePosixForWebKit | 12 | 12 | 100.00% |
| libSceProprietaryVoiceChatHelperPrx | 0 | 4 | 0.00% |
| libScePsml | Shared with libScePsml_debug | — | — |
| libScePsml_debug | 3 | 28 | 10.71% |
| libSceRandom | 1 | 1 | 100.00% |
| libSceRazorCpu | 10 | 10 | 100.00% |
| libSceRemoteplay | 3 | 3 | 100.00% |
| libSceRtc | 34 | 34 | 100.00% |
| libSceRudp | 20 | 24 | 83.33% |
| libSceSaveData | 9 | 9 | 100.00% |
| libSceSaveData.native | 26 | 30 | 86.67% |
| libSceSaveDataDialog | 8 | 8 | 100.00% |
| libSceSaveDataDialog.native | 10 | 10 | 100.00% |
| libSceShare | 16 | 17 | 94.12% |
| libSceShare.native | 9 | 13 | 69.23% |
| libSceSharePlay | 3 | 3 | 100.00% |
| libSceSigninDialog | 7 | 7 | 100.00% |
| libSceSsl | 9 | 23 | 39.13% |
| libSceSysmodule | 8 | 8 | 100.00% |
| libSceSystemGesture | 19 | 22 | 86.36% |
| libSceSystemService | 19 | 26 | 73.08% |
| libSceTextToSpeech2 | 8 | 8 | 100.00% |
| libSceUlt | 37 | 39 | 94.87% |
| libSceUsbd | 30 | 30 | 100.00% |
| libSceUserService | 21 | 21 | 100.00% |
| libSceVdecsw | 13 | 13 | 100.00% |
| libSceVideodec2 | 11 | 11 | 100.00% |
| libSceVideoOut | 42 | 42 | 100.00% |
| libSceVideoRecordingP | 2 | 9 | 22.22% |
| libSceVoice | 17 | 22 | 77.27% |
| libSceVoiceChat | 8 | 8 | 100.00% |
| libSceVoiceQoS | 2 | 14 | 14.29% |
| libSceVrSetupDialog | 6 | 6 | 100.00% |
| libSceVrTracker2 | 0 | 10 | 0.00% |
| libSceWebBrowserDialog | 7 | 10 | 70.00% |
| libSceXml | No declared exports | — | — |
| libSceZlib | 5 | 5 | 100.00% |
| ulobjmgr | 2 | 2 | 100.00% |

</details>

<!-- anyps5-status:end -->

[List of verified games](docs/user/COMPATIBILITY.md)

Dreaming Sarah (2D platformer) runs at a stable 60 fps on a GTX 1050 Ti / i5-7500 3.4GHz.

Unsupported or unexpected states strictly throw `std::runtime_error`. `what()` is printed to stderr and the process terminates.

The [shader recompiler](core/shader/recompiler/Recompiler.cpp) successfully produces SPIR-V (validated via [Spirv-Tools](3rdparty/SPIRV-Tools) when built with `ANYPS5_ENABLE_SPIRV_TOOLS`).

## Compatibility

See the [game compatibility list](docs/user/COMPATIBILITY.md) for tested games and known issues.

## Input mapping

SDL-mapped game controllers are supported, including analog sticks and triggers. Keyboard and mouse controls can be configured with an `anyps5-input.ini` file. See [input mapping](docs/user/INPUT_MAPPING.md) for the supported devices and configuration format.

## Disclaimer

This project is intended for interoperability, research, preservation, and compatibility purposes. It does not include, distribute, or require copyrighted software, firmware, cryptographic keys, or proprietary libraries. Users are responsible for ensuring that any binaries used with this project are obtained and used in accordance with applicable laws and their respective license terms.

## License

This project is licensed under the GNU General Public License version 2 only.
