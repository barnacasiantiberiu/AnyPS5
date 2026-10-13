# libSceGameUpdate technical debt

### Unknown function info

- [sceGameUpdateGetAddcontLatestVersion](../../../core/libs/prx/libSceGameUpdate/Export.cpp) rejects a null entitlement label with `0x80412803`, following the argument rejection in [prosper's GameUpdate tests](https://github.com/mattias800/prosper/blob/d5998dba711ebd230cd30056a3aff9909467fd7b/prosper/tests/hle/dispatch/test_gameupdate.cpp). KytyPS5 accepts that argument. This is a host compatibility choice, not a PS5 measurement made for this change. The existing initialization-first validation order, minimum 0x30-byte info size and clearing of the complete known info structure are retained. The caller's size is preserved and bytes beyond the known structure are untouched, even for an oversized size field. Prosper instead validates pointers before initialization and writes only the found byte without checking this size. Those ABI and output-policy differences remain unresolved.

### Silent stubs

- [Update requests](../../../core/libs/prx/libSceGameUpdate/Export.cpp) have no network service: create and check return the existing offline error `0x80D50001` without creating a request or modifying the check result. Abort and delete retain their no-op success behavior for any id. Initialize and terminate are idempotent. The add-on version query reports no version and does not consult installed content or a remote catalog. Real request handles, downloading, authentication, cancellation of live requests and online update discovery are not implemented. The 7/7 implemented-export count does not establish those capabilities or full PS5 behavior.

### Functional

- [libSceGameUpdate](../../../core/libs/prx/libSceGameUpdate/Export.cpp) is covered by GuestGameUpdate, which exercises all seven exported entry points in four PC suites: argument/size boundaries and guarded output bytes, initialization/termination cycles, the existing offline request behavior, and concurrent version queries. The validation suite fails on the original null-label implementation. Tests require no external service, device, local path or downloaded fixture; the Module portability workflow includes them on clean Windows and Linux runners. Offline tests verify the documented limitations rather than claiming a working update service. New setup or packaging dependencies are not required.
