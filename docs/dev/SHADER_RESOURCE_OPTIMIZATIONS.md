# Shader resource efficiency: experimental second stage

Base: `ef96035cc620645246ab5ef8580b76d0e9449a6b` (PR #26). This change is layered on that branch, not a replacement for it. Both new controls are off by default. No game framerate, physical GPU register count, peak process memory or minimum-hardware improvement has been measured.

## BDA last-range lookup

`APS5_BDA_RANGE_CACHE=1` enables one private last-range index per shader invocation. The read, probe, write and atomic lookup helpers share this index. A hit must select the same upper-bound interval as the existing binary search; an invalid index or a miss uses the original search. The cache never stores loaded guest data or a device pointer.

All existing table-header checks, selected-range checks, permissions, guest/device address overflow checks and fault handling remain. No memory check, barrier, atomic or coherent access is removed. The optimization relies on the existing host-published BDA table remaining immutable while recorded work uses it. The cached index is accepted only inside the sorted table's current search interval, including duplicate starts and overlapping ranges under the original upper-bound rule.

The C++ template in `BdaRangeCache.hpp` supplies the exact decision used by the SPIR-V emitter and the independent CPU differential tests. It is not a substitute for testing the generated SPIR-V. A hit bypasses binary-search iterations but adds selection code and private state. It can be slower for poor locality or register-limited shaders, and remains opt-in. `APS5_BDA_BYTE_READS` disables this fast path for existing byte-read diagnostics. Values other than `0` and `1` are rejected when lookup helpers are emitted.

## Estimated specialization-cache budget

`APS5_SPECIALIZATION_CACHE_MIB` accepts an unsigned decimal MiB value; unset or `0` preserves unlimited retention. A positive value divides an estimated retained-memory budget across four caches:

| Cache | Share |
|---|---:|
| Specialized modules | 1/2 |
| Prepared modules | 1/4 |
| Registration modules | 1/8 |
| Descriptor binding plans | 1/8 |

Completed entries use LRU eviction. In-progress entries retain single-flight `call_once` publication and are not evicted. Removing an entry from a cache does not invalidate a `shared_ptr` held by a caller. Destruction of retired values occurs after releasing the cache mutex. Entries larger than their part's budget are returned to the caller but are not retained. Error results remain memoized while retained, as before.

Accounting uses capacities of SPIR-V, binding and descriptor vectors plus estimated object/key/node overhead. Shared payloads can be counted in multiple caches. This is deliberately an estimate, NOT a cap on RSS, VRAM or all shader memory. Pending compilation, externally retained entries, the source/variant cache, result memos, driver pipeline caches, textures and other allocations are outside the limit. Evicted plans receive fresh identities on reconstruction. A very small budget can cause repeated specialization and additional pipeline variants; the patch does not bound those downstream caches. No automatic low-memory preset is selected.

`APS5_PROFILE_SHADER_CACHE=1` prints `[shader-cache]` counters on first use and at most once per ten seconds during accesses: budget, retained estimate, entries, pending entries, hits, misses, evictions and oversized results. With unlimited retention the byte estimate is not collected and is reported as zero. This telemetry does not create a background thread.

## Build and enable

Use an existing full checkout with the documented toolchain and pinned submodules. Configure again so shader-source/cache-version generation sees the new headers, then rebuild and use the resulting PRX libraries:

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DANYPS5_ENABLE_SPIRV_TOOLS=ON
cmake --build build --target libs --parallel 2
```

The following is an experimental comparison configuration, not a measured recommendation for a particular PC:

```powershell
$env:APS5_BDA_RANGE_CACHE = "1"
$env:APS5_SPECIALIZATION_CACHE_MIB = "128"
$env:APS5_PROFILE_SHADER_CACHE = "1"
$env:APS5_SPIRV_OPT = "balanced"
$env:ANYPS5_SHADER_CACHE_DIR = Join-Path $PWD "shader-cache-resource-experiment"
```

Launch the title from the same session using the newly built libraries. Do not judge FPS during first-use compilation. Use separate fresh cache directories for base, modified-default, BDA-only, bounded-only and combined runs; warm each independently. Keep scene, resolution, driver and settings fixed. Record GPU time, frame-time percentiles, compilation counts/time, cache eviction rate, process memory and downstream pipeline growth. Repeat at least three times and also measure without profiling enabled.

Reset for the next process:

```powershell
Remove-Item Env:APS5_BDA_RANGE_CACHE -ErrorAction SilentlyContinue
Remove-Item Env:APS5_SPECIALIZATION_CACHE_MIB -ErrorAction SilentlyContinue
Remove-Item Env:APS5_PROFILE_SHADER_CACHE -ErrorAction SilentlyContinue
Remove-Item Env:APS5_SPIRV_OPT -ErrorAction SilentlyContinue
$env:ANYPS5_SHADER_CACHE_DIR = Join-Path $PWD "shader-cache-resource-default"
```

Environment choices are read during process initialization/first use, not hot-reloaded. Returning to the original code also requires checking out the baseline and rebuilding `libs`.

## Tests and evidence

Standalone tests require no Vulkan, game assets or submodules:

```sh
cmake -S tools/shader-resource-tests -B build-resource-unit -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-resource-unit --parallel 2
ctest --test-dir build-resource-unit --output-on-failure
```

Add `-DSHADER_RESOURCE_SANITIZERS=ON` for GCC/Clang AddressSanitizer and UndefinedBehaviorSanitizer. Local C++20 builds passed with GCC 14.2 and Clang 17 with warnings treated as errors, plus GCC ASan/UBSan. Tests exercise LRU, estimated bytes, pending and oversized entries, live callers, stale completion, reentrant destruction, 96,000 concurrent accesses and invalid budgets. The BDA suite compares 1,000,000 generated decisions with an independent upper-bound reference, including boundaries, gaps, duplicates, overlaps, invalid cached indices and modeled faults. These are CPU tests, not one million GPU shader executions. Logs and a JUnit report accompany the delivered patch.

Production integration needs a full build configured with `BUILD_TESTING=ON` and `ANYPS5_ENABLE_SPIRV_TOOLS=ON`:

```sh
python tools/shader-resource-tests/run_integration.py --build-dir build --jobs 2
```

The runner builds the six actual regression targets and runs default, BDA-only, one-MiB-bounded and combined/balanced configurations in separate processes and cache directories. It rejects missing tests, skips, failures and missing JUnit reports. `ANYPS5_REQUIRE_VULKAN=1` prevents a missing Vulkan device being accepted as success. The existing BDA device test intentionally omits read and multi-dword-write execution on CPU Vulkan devices; the runner records this limitation in `summary.json` instead of claiming complete BDA coverage.

`.github/workflows/shader-resources.yml` defines GCC/Clang sanitizer jobs and Linux Lavapipe production integration. Merely adding this workflow is NOT evidence of passing integration. The full production build, this new workflow and physical GPU execution were not run in the local editing environment, which lacked direct repository-network access and SPIR-V development dependencies. A physical Vulkan GPU is required to cover the BDA execution cases omitted on Lavapipe. Windows integration remains unverified.

## Not implemented here

General GPU-descriptor specialization, removing/hoisting BDA structural validation, partial native interpolation, whole-process cache/resource budgeting, wave-size or barrier changes, global native FTZ and internal render scaling remain outside this patch. Rendering quality and requested resolution are unchanged. Export coverage is unchanged.
