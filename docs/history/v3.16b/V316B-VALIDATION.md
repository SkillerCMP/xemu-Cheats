# XEMU Cheats v3.16b validation

## Basis and scope
Exact delivered v3.16a Debug Tools and full builder archives. The user's original
`Cheat file BUG.txt` was read without modification. Its 22 code entries and 32
F0/F1/F2 headers are used as an inactive-load/source-deletion stress fixture.
The earlier stall diagnosis was a source-level finding, not a reproduced full
XEMU crash. No claim is made that this package fixes every possible boot crash.

## Layer 1 — native behavior tests

| Test | Result | What really runs / limitation |
|---|---|---|
| Engine / source deletion | 272 assertions PASS with GCC and Clang | Actual parser, executor, F-hook ownership, deletion and SHA-256 helper; guest memory/pause/allocator and generic assembler are test seams. Real installed GLib performs disk IO. |
| Type-F frontend | 27 assertions PASS with GCC and Clang | Actual F0/F2 frontend and extracted production parsing helpers. Generic x86 encoding is a NOP stand-in; F2 raw relocation bytes are real. Not a Keystone byte-equivalence test. |
| Recorder compositor | 13,158 assertions PASS with GCC and Clang | Actual RGB alpha renderer and extracted ComposeFrame, synthetic glyph coverage. Left/right panels, wrap, clipping, alpha blending, cache, 24/480/720/1080/1440/2160 heights; unchanged game pixels. Not an encoder/ImGui rasterizer test. |
| Confirmation flow | 18 assertions PASS | Actual menu/modal method bodies with simulated ImGui inputs; opening/cancel/stale selection/failure/confirm. Actual disk transactions are tested separately. |

The native engine test loads the entire unmodified 8,130-line bug fixture with
ZERO assembly probe calls. It deletes all 22 explicit definitions independently
in changing middle positions, checks exact source byte removal, and proves
neighbouring parsed definitions remain unchanged. It also checks one-time
compile caching, cached errors after manual reselect, all-Type-F preflight
before RAW writes, same/overlapping address refusal, surviving hook ownership
after index changes, duplicate names, applied PREENTRY preservation, CRLF,
post-DEADCODE data, missing terminator isolation, stale revisions, external edits,
read-only source and backup/target-write failures. Fault injection intercepts
only the selected GLib write; successful writes use the actual GLib runtime.

AddressSanitizer + UndefinedBehaviorSanitizer with Clang: engine/deletion,
frontend, recorder and simulated confirmation tests PASS. No sanitizer report.
The newly exercised real-GLib library itself is the installed system binary,
not rebuilt with sanitizer instrumentation.

A CPU panel preview was inspected using masks from the already supplied Roboto
Condensed font through PIL. That validates the RGB path and layout visually,
not the production ImGui provider. No borrowed font or mask file is packaged.

## Layer 2 — syntax / inherited regression checks

Complete Cheat Engine UI, persistence and recorder font-provider translation
units pass GCC and Clang C++17 declaration-facade syntax checks. These fakes are
explicitly local to tests and DO NOT substitute for real SDL/ImGui/nlohmann
headers in a production build. The provider's APIs were checked against the
baseline pinned ImGui header; no integrated dependency build was completed.

The unmodified File Replace v3.16 suite passes with GCC and Clang:
- 6,572 native core assertions (without the optional retail OLK input).
- 166 isolated C read-bridge assertions and 41 physical-span reads.
- Legacy fixed-range overlay and XDVDFS tests pass.
- Address/undefined sanitizers pass on core, bridge, legacy and XDVDFS tests.
- The v3.16a memalign include requires one extra test-only declaration facade;
  it lives under tests/v316b/bridge-extra, not File Replace production code.

Full existing static golden suite: **89 of 92 PASS** with patches 300 and 10–60.
Three old Vulkan fingerprint checks fail identically using the original v3.16a
addon/tests and the same unchanged patched renderer inputs:
- v314ab-vertex-coverage-golden.py
- v314y-vulkan-observer-order-golden.py
- v314z-pfifo-draw-aggregate-golden.py
Those checks were not weakened or suppressed. They are historical-fingerprint
mismatches already documented on the v3.16/v3.16a baseline.

Two historical test files were deliberately adapted: v309 expects saved-state
version 3 rather than 2; v313's isolated RAW-only class stub now exposes the new
preflight/error members. RAW semantics assertions are unchanged. Production JSON
uses find/end rather than contains to retain the existing compatibility guard.

## Layer 3 — source and package audit

The change manifest records SHA-256 before/after for every addon entry. Scope is
Cheat Engine source/UI/persistence/Type-F validation, recorder text/provider,
Meson registration, new documentation/tests, and the two test adaptations above.
Patch 300, patches 10–60, the official source ZIP, build drivers, File Replace
implementation/schema and existing bundled SCII files remain byte-identical.
The nested addon ZIP in full and FIX packages must match the standalone addon
ZIP exactly. Archives are reopened, CRC-tested, source-compared and checksummed
by an independent package-verification pass.

## Still required outside this environment

A complete Windows MXE/MinGW XEMU build, real application/guest execution,
active-hook retirement under a live guest, actual recorded MP4/AVI inspection,
and full macOS/Linux builds. In particular, declaration-only syntax success is
NOT evidence of a complete Windows link. The deliverables are source/build
candidates, not precompiled or runtime-confirmed executables.
