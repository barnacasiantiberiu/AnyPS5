### What
<!-- In English, briefly describe what was added, changed or fixed. For a system library, use one completed module per commit and PR; include its exact implemented/total count. -->

### Tested
<!-- OS, title or homebrew, what was checked. Implementation completion requires passing PC functional tests; "Not tested" does not establish completion. -->
<!-- System-library completion requires passing PC functional tests. Record the command/result and distinguish implemented-export counts from test coverage. -->
<!-- When setup, dependencies or packaging change, report their validation commands/results, skips and host limitations. State whether setup was checked from a clean checkout or reused an existing installation. -->

### Checklist
<!-- Rules behind each item: https://github.com/barnacasiantiberiu/AnyPS5/blob/main/CONTRIBUTING.md -->
- [ ] Based on current `main`; no other open PR implements the same functions
- [ ] One topic per PR; follow-ups go in a new PR
- [ ] No comments in code except [technical debt](https://github.com/boykopovar/AnyPS5/blob/main/docs/dev/CONVENTIONS.md)
- [ ] Unimplemented paths throw (`NotImplemented_nid_no_patch`); silent stubs are listed in [TechnicalDebt](https://github.com/boykopovar/AnyPS5/blob/main/docs/dev/TechnicalDebt.md#silent-stubs)
- [ ] No notes, investigation `.md` files or images added to the repository (attach them to this PR)
- [ ] New third-party code is a submodule built from source
- [ ] Required source, setup, tests, packaging and documentation are included with the implementation; dependencies are pinned and reproducible
- [ ] Changed setup and packages were tested on PC; required runtime files are packaged and results/limitations are reported
- [ ] Generic behaviour, not specific to one title
- [ ] Depends on: <!-- #PR, or none -->
- [ ] Unblocks: <!-- title ID and Game bring-up discussion, e.g. PPSA01325 #3232, or none -->
- [ ] AI-assisted: <!-- yes / no -->
