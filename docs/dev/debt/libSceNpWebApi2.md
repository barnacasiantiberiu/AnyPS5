# libSceNpWebApi2 technical debt

### Unknown function info

- [sceNpWebApi2GetMemoryPoolStats](../../../core/libs/prx/libSceNpWebApi2/Export.cpp) (libSceNpWebApi2) - imported by PPSA21703's Unity PSN plugin. The signature `(int libCtxId, stats*)` and the layout of the Http2 statistics above (measured on a PS5 for Http2, Ssl and Net) are assumed for this library, not measured. The pool size is the one given to `sceNpWebApi2Initialize`, and the peak and current use are 0. The error codes are unknown, so a null output or an unknown context throws

### Functional

- [sceNpWebApi2PushEventAbortHandle](../../../core/libs/prx/libSceNpWebApi2/Export.cpp) (libSceNpWebApi2) tracks push handles per library context, marks one aborted, and makes subsequent filter creation on it return ABORTED (0x80553409). Unknown context and handle errors are 0x80553404 and 0x8055340D; deletion and termination remove the corresponding handles. Signatures, validation order and errors follow [shadPS4](https://github.com/shadps4-emu/shadPS4/blob/e0a466753a1aacbc8db8253be685bf7ab4d9daae/src/core/libraries/np/np_web_api2/np_web_api2_internal.cpp), adapted from [PR #4668](https://github.com/boykopovar/AnyPS5/pull/4668), and remain unverified on PS5. The backend has no PSN service; existing callbacks are not cancelled and filter argument validation and timeouts remain incomplete. PC tests cover state transitions, repeated abort/delete, context isolation, stale handles after termination and four concurrent workers completing 128 handle lifecycles.
