# libSceNpCommerce technical debt

### Unknown function info

- [Commerce parameter/result layouts](../../../core/libs/prx/libSceNpCommerce/Export.cpp) follow [SharpEmu 37694dfb](https://github.com/sharpemu/sharpemu/blob/37694dfbb7b09914af3c63cf6ca8e92cd7c0a30f/src/SharpEmu.Libs/Np/NpCommerceExports.cs): one parameter pointer, a 0x30-byte common base, minimum parameter sizes 0x80/0x88 for Open/Open2, mode at 0x38, user data at 0x58 and a 0x30-byte result with user data at offset 8. Open2's extra eight bytes, target lists, user/service selection and feature flags are not interpreted because this backend never reaches a store. Base/parameter sizes larger than these minima are accepted. These layouts and their compatibility with all PS5 SDK versions have not been measured on hardware.
- [Commerce error and state handling](../../../core/libs/prx/libSceNpCommerce/Export.cpp) use the common-dialog errors and six modes in [SharpEmu's state model](https://github.com/sharpemu/sharpemu/blob/37694dfbb7b09914af3c63cf6ca8e92cd7c0a30f/src/SharpEmu.Libs/Np/NpCommerceDialogState.cs). NOT_FINISHED and ARG_NULL are corrected to 0x80b80005 and 0x80b8000d, matching other host dialogs. Invalid parameters leave the current request unchanged, closing an initialized but unopened dialog returns NOT_RUNNING, and termination clears even a pending request. These are host lifecycle choices, not measured console error precedence.

### Functional

- [Offline commerce lifecycle](../../../core/libs/prx/libSceNpCommerce/Export.cpp) serializes initialization, open, result, close and termination with one mutex. Open snapshots user data and enters RUNNING; UpdateStatus or Close completes it with USER_CANCELED and authorization false. GetResult writes all 48 bytes, including zeroed padding/reserved bytes, and does not modify the output on failure. Repeated results are stable, concurrent opens admit one pending request, and termination removes prior request state. The host does not display a store, contact PSN, authenticate users, authorize purchases or grant content. All 11 counted exports have host paths, including the existing UI stubs; this is export coverage, not full commerce functionality.
- [Commerce tests](../../../core/libs/prx/libSceNpCommerce/CMakeLists.txt) exercise guest ABI calls using independently encoded input bytes and guarded output buffers on Windows and Linux. They cover both layouts, parameter failures, six modes, copied user data, lifecycle, reinitialization, concurrency and icon-call state isolation. They do not validate PSN integration, SDK-version differences or visual presentation.

### Silent stubs

- [libSceNpCommerce](../../../core/libs/prx/libSceNpCommerce/Export.cpp) - the PS Store icon show/hide calls and `sceNpCommerceSetPsStoreIconLayout` do nothing. Both dialog open variants intentionally finish as canceled through the offline lifecycle; no purchase is reported as successful.
