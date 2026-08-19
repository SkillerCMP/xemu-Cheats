# Debug Tools regression tests

These tests protect behavior that has been deliberately kept stable during the
v1.67 cleanup/optimization baseline. They are host-native tests and do not require
a complete xemu build.

`run-regression-tests.py` performs the project-layout/ownership checks, syntax
checks the Python/Bash helpers, and then uses both GCC and Clang when available.

The current golden tests are:

- `assembler-golden.cpp` - compiles the real `x86-cheat-assembler.cc` and checks
  representative F0 assembly, labels, DD data, PRESERVE/RESTORE, T0-T7,
  private TFLAGS consumers, and expected errors by output size/hash.
- `allocator-golden.py` - extracts the real free-list helper bodies from
  `external-code-memory.c`, compiles those exact bodies in a small host harness,
  and compares first-fit allocation, consumption, insertion, coalescing, overlap
  rejection, and alignment against an independent model for 1,000,000 randomized
  operations per compiler.
- `search-compare-golden.py` - extracts the real `ValueSize`, `MatchTarget`, and
  `MatchPrevious` implementations from the split MemoryTools sources, compiles those exact
  bodies, and checks all value kinds/comparison modes against an independent
  signed/unsigned/Float32 model over 5,600,000 randomized pairs plus NaN/Inf/zero
  edge cases per compiler.

These are regression guards, not replacements for a full Windows xemu build and
runtime test.

- `f0-steady-state-golden.py` - freezes the steady-state F0 early-return ordering,
  exact definition signature, reusable active-hook bookkeeping, and final dead-code
  audit invariants.
- `memory-format-golden.py` - extracts the real hot-path Memory Viewer hex
  formatters and compares them against the previous `%02X` / `%08X` output for all
  byte values, edge dwords, and 1,000,000 randomized dwords per compiler.

- `dump-ram-golden.py` verifies the v1.69 physical-only, mapped-virtual-only,
  and combined RAM dump routing/UI plus the existing dump filename contracts.

- `conditions-golden.cpp` compiles the real conditional-breakpoint parser/
  evaluator and checks case-insensitive register names, optional `0x` values,
  AND semantics, every comparison path used by the UI, and malformed input.
- `debugger-conditions-registers-golden.py` freezes the clickable Condition
  YES/NO column, condition editor/operator help, safe execute/watchpoint filter
  routing, and the General/x87-FPU/MMX/SSE tabs inside Current Registers.

### Current Game / XDVDFS disc browser

`xdvdfs-golden.cpp` builds a synthetic XDVDFS image and validates volume/root
parsing, binary-tree directory traversal, nested directories, disc offsets, and
case-insensitive `default.xbe` lookup with GCC and Clang.
`current-game-disc-golden.py` verifies the UI reads the mounted `ide0-cd1`
backend, exposes Game Info / Disc Contents tabs, and calculates the complete
mounted-disc `default.xbe` SHA-256 without reopening a host image path.

- `xbe-labels-golden.cpp` builds a synthetic retail XBE and verifies entry, section, kernel ordinal, string, xref, inferred-function, and primary-label behavior.

- v1.73 extends `assembler-golden.cpp` with absolute-address JMP/Jcc/CALL cases
  and Change-specific short-branch checks, while `inject-golden.py` freezes the
  remembered-original/reopen-safe revert UI and history state.

### v1.74 portable label packs

- `label-packs-golden.cpp` compiles the real `.xlabel` parser/writer together
  with the XBE label core. It verifies Cheat-style header matching, section-
  relative save/load, relocation when a section Virtual base changes, explicit
  rejection of Physical-address persistence, GameID mismatch rejection, and
  MANUAL-over-auto primary-label priority.
- `labels-ui-golden.py` now also freezes the `Labels/XDK`, `PDB`, `Packs`, and
  `Cache` workspace, `.xlabel` load/save/reload controls, label Source column,
  and Meson ownership of the new label-pack translation unit.

### v1.75 local XDK labels

- `xdk-labels-golden.cpp` creates a synthetic i386 COFF static library and a
  synthetic XBE, verifies relocation-normalized exact matching, verifies the
  local cache can be reloaded after the source library is removed, and checks
  that the cache does not retain the original executable-byte sequence.
- `labels-ui-golden.py` also freezes the XDK index build button, Current Game
  XDK status, and label Source filter integration.

### v1.76 MAP labels

`map-labels-golden.cpp` verifies exact Microsoft linker MAP timestamp/layout
matching, XBE section-relative address resolution, common MSVC symbol cleanup,
MAP provenance, data-symbol typing, and rejection of mismatched builds.


### v1.77 PDB labels

`pdb-labels-golden.cpp` builds a synthetic MSF 7.00 PDB and verifies RSDS
GUID/Age matching, original section-header validation, S_PUB32 function/data
resolution, PDB provenance, and strict rejection of GUID, Age, or section-layout
mismatches. `labels-ui-golden.py` also freezes the PDB load control/status and
Meson ownership of the importer.

### v1.78 same-address F0 handoff

`f0-handoff-golden.py` freezes the checkbox deactivation path and the retired-
cave ownership model used for `F0 A ON -> A OFF -> F0 B ON` at the same guest
hook. It verifies that the true original hook bytes/identity survive, the old
cave/PRESERVE/T-register allocations are detached instead of blocking B, a fresh
B cave can install immediately, retired executable memory stays protected from
Inject edits, and retirement cleanup continues even while live cheats are
otherwise disabled.


### v1.79 virtual memory-map snapshot

`memory-map-golden.py` freezes the generic x86 page-table snapshot path, the
legacy per-page fallback, and randomized range-to-page expansion equivalence so
the faster mapping source preserves the old Virtual->Physical page semantics.


### v1.80 disassembler bulk-read / Capstone optimization

`disassembler-golden.py` freezes the two-page decode cache, removal of the old
byte-at-a-time guest reads, single reusable `cs_insn`, iterator decoding, page-
base Physical translation, and exact focus-address resynchronization guard.


### v1.81 F0 precompile + lightweight retired-cave reaper

The F0 steady-state guard now requires parse-time F0/F1 probe/signature caches
and forbids per-tick F0 probe assembly. The handoff guard also freezes the
once-per-cave CALL-return cache and retirement backoff so old caves are not
redisassembled/scanned at full 10 Hz while still referenced.


### v1.82 label filter/cache + batch merge

`label-performance-golden.py` freezes the label-generation/filter cache and
single-pass source merge. `label-batch-golden.cpp` compares 1,000 randomized
multi-source databases and proves Append+one SortAndUnique is byte-for-byte
label-order equivalent to the previous repeated Merge behavior.


### v1.83 XBE/PDB/MAP I/O optimization

`file-io-golden.py` freezes the one-read mounted `default.xbe` path, SHA-256
from that same buffer, move-ownership into Current Game, and direct destination
buffers for local PDB/MAP reads. The Current Game disc guard now explicitly
rejects a second DVD read used only for hashing.

### v1.84 debugger/Cheat Engine streamlining cleanup

`debugger-streamlining-golden.py` freezes the behavior-preserving hot-path cleanup:
processed debug stops return before the redundant per-frame register fetch, paired
disassembly panes consume cached display/label strings, containing-opcode lookup is
binary-search equivalent to the old linear scan, and Tick selects the same normal
cheat-owned disabled F hooks with one hook-table scan while excluding debugger-owned
CodeCaves and preserving the previous block deactivation order.

### v1.85 OPT Pass 1

`pass1-streamlining-golden.py` freezes the frame-local Memory Viewer 4 KiB page
snapshot cache (including the partial-mapping short-read fallback), the lifecycle-
invalidated/borrowed F0 T-register-bank metadata cache, and one-shot RAM mapping
result allocation. `source_test_utils.py` also centralizes the brace-balanced
source-function extractor shared by the structural/native golden tests.

### v1.86 OPT Pass 2

`pass2-streamlining-golden.py` freezes the reusable thread-local Capstone
context/scratch buffers, Count-mode decoded-page reuse, conservative final-14-
byte cross-page fallback, and reuse of the selected row's resolved Physical
address.

### v1.87 debugger navigation focus fix

`debugger-navigation-focus-golden.py` freezes keyboard-focus handoff for
Right/Shift+Right Follow, Left Back, and Alt+Right Forward while preserving the
active Virtual/Physical pane and leaving generic debugger jumps unchanged.

### v1.88 OPT Pass 3

`pass3-structural-refactor-golden.py` protects the move-only MemoryTools split.
It fingerprints all 99 v1.87 member-function bodies and shared helper bodies,
keeps the public MemoryTools state/header and Cheat/F0/Capstone/CodeCave bridge
files unchanged, verifies every split translation unit is owned by Meson, and
prevents `memory-tools.cc` from growing back into the former monolith.
Source-inspection tests consume the complete split implementation through
`source_test_utils.py` so future behavior-neutral ownership moves do not require
per-test source-path rewrites.


### v1.89 OPT Pass 4

`pass4-debugger-preferences-golden.py` freezes the debugger-owned preference
keys/defaults, lazy load and range sanitization, in-memory-only per-frame store,
RESET UI display-only boundary, register-tab restoration across main/detached
ImGui contexts, and continued disabling of global ImGui `.ini` persistence.
The Pass-3 structural guard now permits only the explicitly added Pass-4
preference state/declarations and the three display methods that own the new QoL
controls; all other v1.88 MemoryTools member bodies remain fingerprinted.

### v1.90 OPT Pass 5

`pass5-runtime-polling-golden.py` freezes the 500 ms Current Game detection
cadence, complete loaded-header reread, exact byte-for-byte proof required before
skipping derived work, force-refresh/retry behavior, and continued mounted-disc
poll on the unchanged-header fast path. It also guards the reusable
`xemu_get_xbe_info()` header scratch allocation while requiring the historical
full read/header-size/certificate validation path, fingerprints every other
Current Game member body so the UI/disc/label behavior stays v1.89, and keeps the
CurrentGameManager public API unchanged.

### v1.91 OPT Pass 6

`pass6-render-steady-state-golden.py` protects the debugger steady-state cleanup:
hidden x87/MMX/SSE backend reads are skipped while General is active with an
immediate same-frame refresh on extra-tab activation; breakpoint Physical
translations may share only a stopped-guest, frame-local 4 KiB page cache with
direct fallback; debugger backend identity is sampled once per render method;
and unchanged debugger preferences are not rewritten to the in-memory config.
It also fingerprints every MemoryTools method outside the four Pass-6 render
methods and requires the v1.90 `memory-tools.hh` state layout to remain exact.

### v1.95 OPT Pass 10

`pass10-lifecycle-hardening-golden.py` protects the shared scoped C++ guest-pause
ownership, all five post-allocation Type-F install rollback branches, and the
historical label/page dump resume boundaries. It explicitly requires full-range
RAM dumps to keep leaving the guest paused, fingerprints every untouched v1.94
CheatEngine/MemoryTools member, and freezes Inject, debugger/navigation,
Capstone/external-code, Current Game/XBE, and debugger-backend files so lifecycle
hardening cannot drift into a feature or semantic change.

### v1.96 OPT Pass 11

`pass11-post-optimization-audit-golden.py` freezes every non-test Debug Tools
file byte-for-byte to the v1.95 runtime baseline, so this pass cannot drift into
a Cheat/Debugger behavior change. `run-regression-tests.py` now discovers Python
helpers/static golden tests automatically, syntax-checks Python in memory without
creating `__pycache__`, supports independently selectable `static`, `native`, and
`heavy` phases (while keeping the default full suite), accepts explicit compiler
selection and repeatable `--test` glob filtering for bounded targeted runs, and compiles the shared `xbe-labels.cc` native-test object once per
compiler instead of five times. The phases make complete GCC/Clang and randomized
coverage easier to run in bounded command windows without weakening the default
all-phases validation.

### v1.97 detached-window playback continuity

`v197-detached-playback-golden.py` protects the Windows detached-debug-window
move/resize workaround. Detached window move/resize/expose events may present the
guest framebuffer through the main console GL context, but the modal SDL event
watch must not re-enter ImGui frame construction, Debug Tools, Cheat Engine Tick,
Inject, breakpoint processing, or detached-window rendering. The historical main
console expose/resize full-redraw path remains unchanged.

### v1.98 Current Registers COPY ALL

`v198-copy-all-registers-golden.py` protects the Current Registers-only blue
`COPY ALL` action beside the General/x87/MMX/SSE tabs. The click path performs
on-demand General and extra-register reads, copies all four current register
views as `Register<TAB>Value` lines, and does not add steady-state x87/MMX/SSE
backend work while General is selected. `register-copy-golden.cpp` freezes the
clipboard ordering and hexadecimal widths for General, x87/FPU, MMX and SSE
values under both GCC and Clang. All other MemoryTools member bodies remain
fingerprinted to v1.97.

### v1.99 detached Current Game + Xbox HDD Directory

`v199-detached-hdd-current-game-golden.py` protects the move of Current Game out
of the main playback HUD into the shared detached Debug Tools framework and the
new independent read-only Xbox HDD Directory window. The HDD viewer snapshots
FATX metadata from the active `ide0-hd0` BlockBackend under the shared scoped
pause guard, then browses only the captured C/E/X/Y/Z tree (plus F when a valid
extended FATX partition is detected). `fatx-hdd-golden.cpp` validates the retail
partition map, superblock/FAT geometry, nested directory parsing, file metadata,
and FATX timestamp formatting under both GCC and Clang. The viewer has no disk
write API; Current Game keeps its existing polling/content behavior and v1.97's
generic detached-window drag playback path automatically covers both new
external windows.


## v2.00 guest-pause header build fix

- `guest-pause-guard.hh` now establishes the normal QEMU header environment with `qemu/osdep.h` before including `system/runstate.h`.
- `system/runstate.h` is included inside `extern "C"` because the pause guard is C++ while the VM runstate API is implemented in C.
- The fix is intentionally local to Debug Tools; upstream `include/system/runstate.h` is unchanged.
- `v200-guest-pause-build-fix-golden.py` protects include order/linkage and the native pause-guard test uses a minimal `qemu/osdep.h` stub.

## v2.01 detached Current Game font-atlas fix

`v201-current-game-detached-font-golden.py` protects the Current Game detached
window from pushing `g_font_mgr.m_fixed_width_font`, which belongs to the main
ImGui font atlas/OpenGL context. Detached Current Game keeps using its own
context-local default font, while the historical non-detached path may still use
the main fixed-width font. This prevents the Game Info body from rendering with
foreign glyph UVs/texture ownership while leaving Current Game data/polling and
all other detached-window behavior unchanged.

## v2.02 regression-guard ownership fix

- `pass5-runtime-polling-golden.py` now excludes `DrawGameInfoTab()` from the historical v1.90 protected-method digest because v2.01 intentionally changed only that UI method for detached font-atlas safety.
- The remaining 24 Pass-5-protected Current Game methods retain the same digest as v2.00/v1.90. This is a test-only ownership correction; runtime source is unchanged.

## v2.03 HDD save metadata / Current Game HDD / export

`v203-hdd-saves-export-golden.py` protects the read-only v2.03 HDD feature:
TitleMeta.xbx and SaveMeta.xbx friendly display names, the Current Game HDD tab
filtered by the running Title ID, and file/folder/save export that rebuilds a
fresh paused FATX snapshot before streaming bytes to the host. The guard also
requires that v2.03 adds no FATX/HDD write or import path. `fatx-hdd-golden.cpp`
uses a synthetic retail-layout image to verify TitleMeta/SaveMeta decoding and
byte-exact streamed file export under both native compilers.
