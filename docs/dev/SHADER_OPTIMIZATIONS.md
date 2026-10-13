# Conservative shader optimizations

Base reviewed: `f84323eca17795590362a7b98404f9a619f94a78`.

This change implements three bounded parts of the shader performance audit. It
does not establish a game framerate improvement, lower physical register use or
a new minimum hardware requirement. Integration and GPU validation are required
before merge. The extra SPIR-V profile is opt-in.

## Implemented

### Denormal-free loop invariants and masks

`Optimization/src/DenormalFlushEliminator.cpp` collects live flush candidates and
constructs their dependency graph before modifying the IR. The independent
`Optimization/DenormalValueProof.hpp` helper uses iterative O(V + E) traversals.

Proven leaves are constants without a subnormal f32 encoding, existing flushes,
supported conversions and masks that force the property. Preserving operations
depend on every relevant operand. Unsupported producers and empty phis remain
unknown. First, reachability from proven leaves anchors cyclic invariants; then
failure propagates from all ungrounded nodes to all their users. One safe arm
cannot conceal an unknown operand or an unanchored subcycle.

Additional rules propagate existing proofs through AND masks retaining every
exponent bit, and OR/XOR masks changing only the sign bit or nothing. Previous
unconditional exponent/mantissa mask rules remain. Arithmetic is not relaxed.
Sources are re-read before replacement so nested eliminations do not restore a
stale flush. Existing dead-code elimination removes the unused checks.

Explicit graph storage replaces recursive stack growth. Compilation time and
peak compiler memory have not been benchmarked; do not assume both improve.

### Shorter geometry-shader passthrough lifetimes

`SpirvBackend/src/BarycentricGeometryShader.cpp` preloads only locations needed
as all-three-vertices data. Passthrough-only inputs are loaded just before the
corresponding output store. Shared locations reuse the preload. The geometry
stage, interfaces, stores, vertex order, weights, limits and cache stay intact.

This shortens live ranges in emitted SPIR-V, not necessarily in driver machine
code. The driver may reschedule loads; reduced physical register use and faster
draw execution need measurement.

### Opt-in balanced SPIR-V profile

`APS5_SPIRV_OPT=balanced` adds SSA rewrite, vector DCE, dead-insert elimination
and load-size reduction to the non-exhaustive pipeline. The extra passes run
only when the existing `HasLimitedUseTypes` guard permits the folding path.
Narrow storage-only types retain the previous pipeline.

The default SPIR-V pass selection remains unchanged. Balanced adds no exhaustive
inlining, loop unrolling, if-conversion, relaxed precision or fast math. Existing
pre/post validation, binding preservation and specialization handling remain.
`full` and `none` retain their meanings; `none` is not a complete bypass of all
specialization transformations. Run each mode in a fresh process. The existing
cache switch key includes `APS5_SPIRV_OPT`; source-version generation tracks
recompiler source/header changes. Separate A/B cache directories are still useful.

## Build and enable

Use the prerequisites in [BUILD.md](BUILD.md). From an already configured checkout:

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DANYPS5_ENABLE_SPIRV_TOOLS=ON -DAPS5_ENABLE_TIMING_LOG=ON
cmake --build build --target libs --parallel 2
```

`--parallel 2` limits build concurrency, not game execution. Make sure the title
loads the newly built patched PRX libraries instead of an older copied build.

Default-profile run:

```powershell
Remove-Item Env:APS5_SPIRV_OPT -ErrorAction SilentlyContinue
$env:ANYPS5_SHADER_CACHE_DIR = Join-Path $PWD 'shader_cache_default'
```

Separate experimental-profile run:

```powershell
$env:APS5_SPIRV_OPT = 'balanced'
$env:ANYPS5_SHADER_CACHE_DIR = Join-Path $PWD 'shader_cache_balanced'
```

Launch the title from the same shell. No resolution or game configuration file
is changed. Unsetting the variable restores the original SPIR-V pass selection;
revert the code changes to undo the denormal and geometry changes as well.

## Validation

Independent tests need only CMake and a C++20 compiler:

```sh
cmake -S core/shader/recompiler/tests/denormal_proofs -B build-proofs -DCMAKE_BUILD_TYPE=Release
cmake --build build-proofs --parallel 2
ctest --test-dir build-proofs --no-tests=error --output-on-failure
```

Executed locally: GCC 14.2, Clang 17, and GCC AddressSanitizer plus
UndefinedBehaviorSanitizer. All passed. Coverage includes cyclic graphs, a
100,000-node graph, 10,000 deterministic random graphs against an independent
traversal, 4,000,182 bit-mask input pairs and invalid inputs. These tests cover
the helper and masks, NOT the complete IR adapter, renderer or console hardware.

Eight cases were added to the production IR regression: sign-preserving loops,
unsafe arithmetic/select back edges, empty/unanchored phis, more masks, special
f32 encodings, nested replacements and deep chains. With `BUILD_TESTING=ON`:

```sh
cmake --build build --target agc_denormal_flush_elimination_tests agc_driver_pixel_barycentric_emulation_tests agc_driver_tessellation_tests
ctest --test-dir build -R '^agc_(denormal_flush_elimination|driver_pixel_barycentric_emulation|driver_tessellation)$' --no-tests=error --output-on-failure
```

Set `ANYPS5_REQUIRE_VULKAN=1` to fail rather than skip unavailable rendering.
Repeat with `APS5_SPIRV_OPT=balanced` in a new process and separate cache directory.
The new `Shader optimization regressions` workflow builds these targets using
the repository's pinned submodules and runs both profiles on Linux Lavapipe.
A workflow definition is not a passing CI result; check the actual job outcomes.

The editing environment could not clone the full repository because direct
network/DNS was unavailable and lacked SPIR-V development dependencies. Source
was read through the GitHub connection. Full native-IR and rendering tests,
Windows builds, and physical-GPU/game benchmarks were NOT run locally.

## Acceptance and scope

Compare the base commit, modified default, and balanced profile separately.
Keep scene, resolution, game/driver versions and power settings constant. Warm
each cache independently and separate cold compilation from steady GPU time.
Use at least three repetitions and the existing trace in
[BUILD.md](BUILD.md#performance-trace). Do not sum inclusive timing scopes.
Smaller SPIR-V or passing software-Vulkan tests do not prove hardware speedups.

Not implemented here: BDA lookup hoisting/removal, GPU descriptor specialization,
partial native interpolation, global native FTZ, cache-memory budgets, wave-size
changes or internal render scaling. Memory checks, atomic/coherent accesses,
barriers, precision controls and disabled resolution presets are not weakened.
