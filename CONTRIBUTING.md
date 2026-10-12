# Contributing

## Code

- Follow the [coding conventions](docs/dev/CONVENTIONS.md): naming, no comments except [technical debt](docs/dev/TechnicalDebt.md), Conventional Commits.
- Every function either does exactly what it is supposed to or throws. Unimplemented exports call `NotImplemented_nid_no_patch(__func__)` (see [libSceAudioIn](core/libs/prx/libSceAudioIn/Export.cpp)).
- Pull requests that add or change shader instruction semantics say where they come from: measured on hardware (which GPU and what was checked, e.g. with the [hardware oracle](docs/dev/HW_ORACLE.md)) or the exact source (ISA section, LLVM, ACO or Mesa file). Reviewers check the semantics on hardware. Cases the source doesn't settle throw, and behaviour not verified on hardware is recorded in [technical debt](docs/dev/TechnicalDebt.md).
- A silent stub is allowed only when it unblocks a title and only affects the UI; add it to [silent stubs](docs/dev/TechnicalDebt.md#silent-stubs).
- Missing imports fail at startup with a clear error; don't replace them with fallbacks that keep running.
- Implement the general behaviour of a function, not what one title happens to need.
- Don't add replacements for modules that titles ship themselves in `sce_module/`, `sce_modules/` or `prx/`: engine or middleware modules (Cohtml, FMOD, GOG Galaxy) and SDK libraries that only wrap other system libraries (NpCppWebApi over NpWebApi2). The relinker converts and loads the title's own module, and a host library with the same name is left out of `DT_NEEDED`. Only system libraries, which reach the kernel or the hardware, are reimplemented in [core/libs/prx](core/libs/prx).
- Avoid non-standard extensions (`__attribute__`, etc.) where standard C++ is enough. Helper symbols that must not become NIDs use the `_nid_no_patch` or `_nid_no_patch_cut` suffix.
- The relinker uses only the C++20 standard library.
- Third-party code is added as a submodule under `3rdparty/` and built from source, not found on the system. With `ANYPS5_USE_SYSTEM_DEPS=ON` the packages packaged by the distributions (SDL2, freetype, Vulkan-Headers, SPIRV-Headers, glslang and SPIRV-Tools) come from the system instead; that resolution lives in [cmake/SystemDeps.cmake](cmake/SystemDeps.cmake) and the default build never reads the system.
- Don't add tests that only check that a symbol is exported: a missing export already fails at startup.
- Each test finishes in 30 s on the CI runners, Vulkan tests included (Linux runs them on lavapipe), counting the first run with no shader cache. ctest runs in parallel there, so a slow test competes for the cores and times out on some runs and not others. Split or shrink a test that gets close instead of raising its timeout. CI reports every test over 30 s as a warning on the pull request.

## Build and test

### Fork development: reproducible implementations

- Every implementation must include the source, configuration, setup helpers, functional tests, packaging rules and build instructions needed to reproduce it on a PC. Include required support files in the implementation commit. A clean checkout must work through documented prerequisites and tracked setup commands, without undocumented downloads, ignored local scripts or user-specific paths.
- Pin third-party source with submodule gitlinks. For new or changed binary dependencies, record the version, source URL and SHA-256, and verify the checksum before installation or extraction. Keep portable download/setup procedures and dependency provenance in the repository; document supported host versions and external prerequisites.
- Keep compiler archives, extracted toolchains, research PDBs, caches, logs and generated build outputs outside Git. If an external artifact is required to build, test or run the implementation, provide a tracked way to obtain or recreate it. Packaging must include the required redistributable runtime libraries, discovered from the configured dependencies.
- Test changed setup and packaging procedures on the PC as well as the implementation. Check that generated packages contain the required libraries and runtime files. Record commands, results, skips and host limitations in the English PR; distinguish a reused local build from a verified clean-checkout run.

### Fork development: functional tests for every module

- Finish the requested system library before committing: all its exports must be implemented and its PC functional tests must pass. Check the exact implemented/total counts, not a rounded percentage. Keep partial module work uncommitted, and report unresolved dependencies or unknown behavior without counting stubs as complete.
- Keep the completed library's changes, required support files, functional tests, technical-debt updates and generated README status in one commit and one pull request. Do not create per-function or partial-module commits. Write commit messages and concise pull request descriptions in English, stating what was added, changed or fixed and how it was tested. Independent shared infrastructure may have a separate commit and PR; declare it as a prerequisite when needed. Do not rewrite already published commits just to combine later tests with an earlier implementation.
- Run `python tools/update_readme_status.py --install-hook` once per checkout. The pre-commit hook updates the README's hidden source snapshot and image cache keys using only staged source files, so partial source changes are counted correctly. It refuses to overwrite unstaged README edits. Other checkouts must install the hook too; commits made with hooks bypassed or through GitHub must run `python tools/update_readme_status.py` and include the resulting README. Use `--check` to verify synchronization. The hook updates counts only; functional tests remain required separately.
- The README uses the original project's badges and progress map, linked to [this fork's progress page](https://barnacasiantiberiu.github.io/AnyPS5/). The Progress workflow regenerates that page, including its original System libraries table (`Library`, `Implemented`, `Total`, `%`), on every push to `main`. Images display the latest deployed `main`, including when viewing an older README; the hidden snapshot records the source counts of that individual commit. This fork enables the Progress workflow; the inherited build workflows described below remain disabled, and functional tests run locally.
- Every newly implemented or changed module must have PC-runnable functional tests registered with CTest labels `module-functional` and `module:<library-name>`, and a nonzero timeout of at most 30 seconds. Add affected dependent modules to validation when shared code changes.
- Exercise public guest-ABI entry points and check concrete outputs against independent expected values or documented invariants. Cover valid inputs, boundary and invalid inputs, lifecycle/state isolation, and untouched output outside the permitted region where relevant. An export check, success return alone, or a test that only expects `NotImplemented` does not validate an implementation.
- Run `python tools/test_modules.py --build-dir build` before committing or pushing module work. This builds, runs every registered functional module suite and saves per-module JUnit/logs, a JSON result and a separate progress snapshot. Skipped, disabled, missing or failed tests are failures. Use `--module <library-name>` repeatedly for targeted runs; use `--base <git-ref>` to require suites for every library changed from that ref.
- Tests must fail when the behavior they protect is broken. For a new regression, demonstrate failure on the original implementation or a controlled local mutation, then restore the implementation and rerun successfully. Record the evidence and remaining limitations in the commit message or validation report.
- Validation for this fork runs on the PC; PS5 hardware is not a prerequisite. Host tests and the implemented-export percentage are separate evidence, and neither establishes unmeasured console behavior. Keep unverified ABI details and differences in technical debt.

Toolchains are listed in the [build instructions](docs/dev/BUILD.md).

For relinker changes, use the [relinker-only build](docs/dev/BUILD.md#relinker-only) to build and test without third-party submodules, system libraries or a GPU. It also works on macOS. Changes to system libraries or shaders still need the full build and relevant runtime tests.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Python is optional for building without validation; without it some relinker tests are not registered. This fork's implementation validation and Windows setup helpers require Python 3.10 or newer.

Every push to `main`, including a merged pull request, automatically runs full Linux and Windows builds and tests and the relinker matrix on Linux, Windows and macOS, using the pushed commit without contributor checks.

For pull requests, full Linux and Windows builds and tests run only when a collaborator with write access selects Actions > Build > Run workflow on `main` and enters an open pull request number targeting `main`. A collaborator can start the same manual run from the command line, replacing `1234` with the pull request number:

```
gh workflow run build.yml --repo boykopovar/AnyPS5 --ref main --field pr=1234
```

This manual run also builds and tests the relinker on Linux, Windows and macOS, regardless of the author's previous contributions or the changed files. The relinker matrix also runs automatically when a pull request targeting `main` is opened, updated or reopened, if its author already has a commit in `main` and it changes a file the relinker-only build uses; full builds never run automatically for pull requests. Every job that runs automatically for pull requests runs only when the pull request changes its files: a `paths` filter on the `pull_request` trigger, or `needs: eligibility` with an `if` on the files output it sets in `build.yml`. The Conventions check fails a workflow change that adds a job without one. All build jobs build the selected pull request merged into the current `main` (GitHub's merge commit), so a pull request that no longer merges cleanly must be rebased first. New commits require another manual run for full builds.

The Conventions check runs automatically for every contributor, including first-time contributors. It uses the base branch checker and reads pull request Git objects without executing pull request code. The Conventions check runs on every pull request and fails when a rule on this page is broken. It accepts code comments only when the pull request also changes [TechnicalDebt](docs/dev/TechnicalDebt.md), and only UTF-8 text files. Run it locally before pushing:

```
python3 tools/check_conventions.py --base origin/main
```

## Branches and pull requests

The [pull request template](.github/pull_request_template.md) is the checklist for these rules.

- One branch per topic, based on current `main`. Follow-up work goes in a new pull request, not into an open one.
- Open pull requests per author are limited by how many of theirs are merged into `main`: 1 with none merged, 20 with at least 1, no limit from 20. A pull request opened or reopened over the limit is closed automatically; reopen it once another one is merged or closed.
- Before starting, check that no open pull request already implements the same functions.
- Keep the branch up to date with `main` and resolve conflicts yourself; rebasing and force-pushing is fine.
- If a pull request needs another one first, say so on the checklist line (`Depends on: #N`; `none` otherwise).
- If a pull request fixes a stop or a wrong result seen on a title, name it on the checklist line with the title ID and its [Game bring-up](https://github.com/boykopovar/AnyPS5/discussions/categories/game-bring-up) discussion (`Unblocks: PPSA01325 #3232`; `none` otherwise). Reviewers review these first.
- Fixes a title needs go to `main` as pull requests. A Game bring-up discussion lists the open pull requests and local changes the title still needs; update the list when one is merged, closed or replaced.
- The PR overlap comment lists open pull requests that change the same lines or exports; agree with their authors which one goes first and declare it with `Depends on`.
- Run the tests before opening the pull request and describe what was tested (OS, title or homebrew).
- Investigation notes, reports, screenshots and logs go in the pull request, not in the repository. Maintained setup, test and packaging scripts and required build instructions belong in the repository with the implementation. Images for documentation go in the [gist](https://gist.github.com/boykopovar/0e53f2e1426f29ecd41e3b51540b8a90) comments.
- Say whether the change was written with AI assistance. The author of the pull request is responsible for every line of it.

## Documentation

Text in the repository is dry, strict and concise. Record known gaps, unknown NIDs and signatures in [technical debt](docs/dev/TechnicalDebt.md) instead of new files.
