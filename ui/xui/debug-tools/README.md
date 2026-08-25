# xemu Debug Tools Extensions

This directory contains the custom RAW Cheat Engine, Current Game manager,
Memory/Map tools, detached tool windows, x86 debugger UI, HDD/FATX tooling, and
the small C bridges used to access QEMU/xemu CPU and memory facilities.

The project is now split into a **main addition plus optional additions**. Normal
xemu UI code includes only `debug-tools/debug-tools.hh`; optional tools register
with Debug Tools-owned APIs instead of adding their own xemu hooks.

- **Main addition (always built):** Current Game + RAW Cheat Engine, shared guest
  memory/debug bridge, F0 assembler/hook support, Capstone decode, Keystone encode,
  XBE/label/disc support, and the generic detached-window host.
- **HDD addition (optional):** HDD Directory, FATX services, Guest Kernel RPC, and
  the Current Game HDD/Kernel-RPC extension tabs.
- **Memory addition (optional):** Memory Viewer, Search, x86 Debugger, Inject,
  Labels and RAM Dump frontends.

Select the source build in `build-profile.txt`: `main`, `main+hdd`,
`main+memory`, or `full`. `full` is the default and preserves the complete
v2.90.6 feature set. `main` is the intended Current Game/Cheat Engine-only fork;
HDD and Memory implementation translation units are not compiled in that profile.

### Docker profile selection

`Build-Xemu-DOCKER.bat` can select the Debug Tools profile at build time without
editing `build-profile.txt`. Double-click/run the BAT and choose:

1. `main` - Current Game + Cheat Engine only
2. `main+hdd` - Main + HDD Directory/Kernel RPC
3. `main+memory` - Main + Memory Viewer/Search/x86 Debugger
4. `full` - all additions

For scripted builds, pass the profile as argument 2 after an optional source
root, for example `Build-Xemu-DOCKER.bat . main`, or set
`XEMU_DEBUG_TOOLS_PROFILE=main`. The Docker selection is an environment override
and does not rewrite the source-tree `build-profile.txt`. The Docker container
can enter through normal root `build.sh`. A single integration line redirects once into
`ui/xui/debug-tools/build-xemu.sh`, so Capstone/Keystone dependency preparation
remains owned by this addition rather than being implemented in xemu's build script.

The project rule remains to keep custom implementation inside
`ui/xui/debug-tools/` whenever possible. The root of that directory is reserved for
the main Current Game/RAW Cheat Engine addition, shared/core services, and build/facade
files. Optional HDD and Memory implementations live physically under their respective
`addons/` directories. Upstream xemu files should contain only the smallest required
integration hooks; unrelated upstream code is not a cleanup target.

## RAW Cheat Engine and Patch lifecycle

- Ordinary `+Cheat` blocks live on **Cheats** and execute only when the live
  Cheats control is enabled. Unchecking an ordinary Cheat is an unconditional
  OFF boundary: the block is disabled immediately and any active F0/F1 hook
  owned by it restores the exact original hook bytes captured at installation.
  Group deselection follows the same rule.
- Startup patches use the preferred `+:PREENTRY:Name{Description}` syntax. The
  standalone `:PREENTRY:` compatibility form remains supported.
- **Patch** is independent of the Cheats-tab Enabled/Disabled button. The global
  **Engine Enabled** option remains the master startup safety gate.
- Checking/unchecking a Patch stages the next startup/reset. A Patch is reported
  as applied only after its block executes successfully. Failed startup blocks
  keep their own error and remain reset-required.
- Direct Game A -> Game B identity changes are treated as real startup boundaries.
  Explicit UI Reset retains its separate `ResetRequested -> ApplyPending`
  lifecycle and does not depend on observing a transient no-XBE frame.
- Patch selection identity is code-file path + group path + block name +
  occurrence ordinal, so duplicate names remain independent. Stale identities
  are pruned only for the file currently being parsed.
- Live Type-F ownership and PREENTRY Type-F ownership remain separate. Disabling
  live Cheats never tears down an already-applied Patch hook.

## Debug Tools tab states

Debug Tools use exactly three local tab states without altering xemu's global
theme:

- **Inactive:** light grey (`#949494`)
- **Hovered:** steel blue (`#5B8FBA`)
- **Selected:** xemu's existing active-tab green

The style is owned by a scoped RAII helper so every pushed tab color is restored
automatically. Partially selected Cheat/Patch groups display an indeterminate
mark inside the checkbox.

## Current Game / debugger / memory tools

Current Game tracks the running XBE identity and mounted-disc metadata. Debugger
and Memory Tools provide disassembly/navigation, conditional breakpoints, live
register views, labels/importers, memory/search/map views, RAM dumps, and F0
CodeCave integration. The existing historical debugger behavior is protected by
the packaged golden tests.

## HDD / FATX safety

Filesystem mutation retains the permanent transaction invariant:

`Create/Open -> Write -> Flush -> Close -> fresh FATX Verify`

Cleanup and optimization must not weaken that sequence. Copy/Move verification
continues to use fresh coherent FATX snapshots and byte-for-byte content checks.
Read-side optimizations may reuse host buffers or measurement helpers only when
those verification semantics remain unchanged.

## x86 Debugger Inject restore behavior

`Inject > NOP` and `Inject > Change` share one remembered-original instruction
record for the session. Change exposes only `APPLY` and `RESTORE`; Restore fills
the Replacement field with the remembered original instruction and writes the
exact captured original bytes. A conditional `Inject > Restore` menu item is
shown on rows owned by an active NOP/Change patch and restores the complete
tracked instruction span after verifying that the live bytes still match the
patch xemu wrote. CodeCave continues to use its separate Type-F0 restore path.

The debugger's right-side section is tabbed as **Breakpoints | Changes**. Changes
lists active restorable debugger patches as `Address | Original | Changed | HEX`.
HEX is per-row and defaults off: unchecked rows show decoded instruction text;
checked rows show the captured/written bytes. Address right-click reuses the
normal debugger/disassembler context menu and double-click follows the address.
For the temporary debugger CodeCave/F0 hook, Changes records only the hook /
jump-from address and never enumerates the generated F0 cave body. A real user
Reset discards these debugger-only Changes records without writing captured
pre-reset bytes into the new guest. The disassembler refresh is deferred until QEMU
has consumed the pending reset, then the existing refresh path rereads post-reset RAM
exactly once; PREENTRY Patch activation remains separate.
CodeCave RUN is refused when the hook overwrite span overlaps the live XBE header
(`m_base` through `m_sizeof_headers - 1`) so a debugger JMP cannot modify the
header bytes used for running-XBE revision detection.

After NOP, Change, Restore, or CodeCave RUN/RESTORE, the disassembly refresh is
anchored to the instruction that was actually modified; an older Follow/Go-To
target cannot pull the view away, and the refresh does not add a Back/Forward
history entry.

## Files

- `cheat-engine.cc` - small per-frame Cheat Engine coordinator (`Tick`).
- `cheat-engine-source.cc` - CMP/RAW source parsing, file discovery/loading, game identity, and PREENTRY/Patch lifecycle.
- `cheat-engine-fhooks.cc` - debugger/Live Cheat Type-F0/F1 compile, install, restore, retirement, and ownership lifecycle.
- `cheat-engine-execute.cc` - guest read/write helpers and RAW code execution (0/1/2/3/4/5/6/7/9/A/D/E/F).
- `cheat-engine-ui.cc` - Cheat/Patch selection controls, menu/help, and Cheat Engine rendering frontend.
- `cheat-engine.hh` - shared Cheat Engine state/API contract; unchanged by the Phase-9 split.
- `cheat-engine-memory.c/.h` - C bridge to QEMU/xemu memory and debugger access.
- `current-game.cc/.hh` - current XBE/title/revision, mounted-disc tracking, label/XDK/MAP/PDB state, and disc export core.
- `current-game-ui.cc` - Current Game rendering frontend: summary, Game Info, Disc Contents, extension slots, and disc-entry context UI; it has no HDD/Kernel-RPC dependency.
- `debug-tools.cc/.hh` + `debug-tools-module.hh` - the single xemu-facing integration facade and Debug Tools-owned module/extension registry.
- `prepare-build-dependencies.sh` / `build-xemu.sh` - Debug Tools-owned Capstone/Keystone preparation and the wrapper reached by root `build.sh`'s single integration line.
- `docker-build-windows.sh` - the Docker container-side GitHub-equivalent Windows build driver used by `Build-Xemu-DOCKER.bat`. Every successful Docker build exports both the exact GitHub-style `.tar.zst` source archive and a convenient ZIP made from that same packaged source tree under the run's `source/` directory.
- `detached-tools.cc/.hh` - independent SDL/OpenGL/ImGui tool-window host with registration rather than a hard-coded feature list.
- `addons/hdd/` - the complete optional HDD addition: HDD Directory UI/state, FATX parsing/snapshots/export, Guest Kernel RPC, filesystem planning/streaming, and its registration unit.
- `addons/memory-tools/` - the complete optional Memory addition: Memory Viewer/Search, x86 Debugger, Inject, Labels, RAM Dump, breakpoint conditions, register-copy helper, and its registration unit.
- `addons/stubs/` - tiny no-op registration units linked when an optional addition is disabled.
- `label-symbol-utils.hh` - shared ASCII and Microsoft symbol-display helpers used by XDK/MAP/PDB importers.
- `x86-cheat-assembler.cc` - Type-F0 directive expansion, temp/preserve handling, labels/data, and public assembler frontend.
- `x86-cheat-assembler-keystone.cc` / `x86-cheat-assembler-internal.hh` - Keystone-backed IA-32 instruction assembly plus the small F0 syntax/label utility bridge; the old hand-written generic opcode encoder has been removed.
- `backend/xemu-dbg.c/.h`, `backend/whpx-debug.c/.h` - debugger backend bridge.
- `tests/` - historical freeze guards, current feature guards, native goldens,
  and Heavy randomized regression models.

## Upstream integration points

The intended upstream-touch surface remains deliberately small. In particular,
the PREENTRY reset lifecycle uses only the existing two-line Reset notification
bridge in `ui/xui/actions.cc`; PREENTRY execution/state remains inside Debug
Tools.

Capstone and Keystone dependency preparation is owned by Debug Tools rather than
implemented in GitHub workflow YAML or in xemu's build logic. Root `build.sh`
differs from upstream by exactly one redirect line: when the Debug Tools addition
is present it enters `ui/xui/debug-tools/build-xemu.sh` once; when the addition is
absent the upstream build path is unchanged. The wrapper calls
`prepare-build-dependencies.sh`, which reuses compatible target libraries or builds
the pinned static dependencies, exports their pkg-config paths, adds xemu's
`--enable-capstone` configure option, then returns to normal root `build.sh` with a
recursion guard. Capstone is built with only the Xbox-required X86 decoder;
Keystone is built with only the X86 assembler backend.
Both helpers are target-aware for Linux x86_64/AArch64, macOS x86_64/ARM64, and
Windows x86_64/ARM64. Upstream multi-platform workflow YAML remains unchanged.
`build-capstone-windows.sh` remains only as a legacy compatibility wrapper; the
current Docker builder enters through `docker-build-windows.sh` -> root `build.sh` -> the one-line Debug Tools redirect -> `build-xemu.sh`.

Type-F0 source syntax does not change with the Keystone backend. Existing labels,
`DD`, `PRESERVE`/`RESTORE`, `T0-T7`/`TFLAGS`, and hex-first numeric input remain
owned by the F0 frontend, while normal Intel-syntax IA-32 instructions are encoded
by Keystone. A compatibility adapter preserves legacy forms that Keystone itself
parses more strictly: register-sized memory operands such as `mov cl, [0046D784]`
are given the implied size internally, and F0-generated/bare A-F-leading hexadecimal
values are normalized to explicit `0x...` form before assembly. Cheat files therefore
do not need to be rewritten to Keystone-specific spelling. This greatly broadens
usable x86 instructions (including normal x87, MMX, SSE, and less-common integer
forms) without introducing a second cheat-code format. Capstone continues to own
hook-span decoding/disassembly and executable boundary validation.

## Validation

The `tests/` directory is optional for normal builds. If it is present, the local Docker
builder automatically runs the regression suite before packaging/building. If the entire
`tests/` directory is absent, project-layout validation records that tests are omitted and
the build continues normally. A present-but-incomplete tests directory remains an error so
a damaged regression package cannot be silently ignored.

Every successful Docker build also keeps the exact source package used for that build. The
run output contains `source/xemu-<version>.tar.zst`, `source/xemu-<version>-source.zip`, and
`source/DEBUG_TOOLS_PROFILE.txt`. In the disposable packaged copy, `build-profile.txt` is set
to the profile selected for that run (the user's working source is never rewritten). The ZIP
is created from the same freshly archived source tree consumed by the Windows build, and the
two Windows build BAT files are explicitly staged into that source package so it can be
rebuilt locally with the same profile by default.

The regression suite is under current **v2.87** ownership.
`tests/v287-run-regression-tests.py` exposes Static, Native, and Heavy phases, targeted
`--test` selection, compiler selection, per-command timing, and optional Static
`--jobs` parallelism. Stable behavior-based tests keep subsystem-oriented names,
while historical release-numbered guards are consolidated into the current
`v287-*` suites.

`v287-final-production-audit-golden.py` owns the current v2.91.6 complete 96-file production
fingerprint and the cumulative dead-code/helper-ownership audit.
`v287-ownership-structure-golden.py` owns the current final split boundaries
without retaining stale per-phase byte fingerprints. Current consolidated suites
cover UI/runtime, HDD/FATX/Kernel RPC, PREENTRY/Patch/Cheat, and Windows/build/test
infrastructure regressions. Every retained top-level golden carries a v2.87
current-ownership marker.

The final production audit explicitly follows the production HDD delete/import
entry points and preserves the permanent Xbox-kernel transaction:
`Create/Open -> Write -> Flush -> Close -> fresh FATX Verify`.

Packaging validation must keep generated Python bytecode and `.git/index`
mutations out of source artifacts. The suite remains a regression layer in
addition to the pinned Windows build and runtime confirmation.

## Release history

See [`CHANGELOG.md`](CHANGELOG.md) for the complete release-by-release history.
