# Windows-only v0.1.42 backport: Step Into + Type F

This source tree keeps the original v0.1.42 Windows-only build/platform layout and
backports only the later debugger stepping fix and finalized Type-F code-cave engine.
No Linux/macOS builder restoration, local MINGW builder, DLL packaging, cv2pdb, or
other v0.1.43-v0.1.59 build-system changes are included.

Backported runtime features:

- WHPX explicit one-shot **Step Into** behavior from v0.1.52.
- F0 automatic 32-bit x86 assembly caves with labels and `DEADCODE`.
- F1 aligned raw-hex caves with `DEADCODE 000000NN` (`NN=01..08`).
- Type-F stable free-list lifecycle: disabling restores the hook, safely clears/frees only that cave, coalesces adjacent free blocks, and never shifts another active cave.
- Automatic external 1 MiB xemu-owned mapped arena, split into 960 KiB for
  F0/F1 code + DD data and 64 KiB for preservation/T0-T7 private state; includes cave allocation,
  hook sizing, JMP in, generated JMP back, and original hook-byte restoration.
- Optional leading `$` on all Type-F lines.





## v0.1.65 private TFLAGS + exact debugger navigation/layout

- Every T-using F0 now owns a private `TFLAGS` state alongside `T0-T7`.
  Flag-producing instructions that involve a T register save the guest EFLAGS,
  execute, capture the resulting flags into `TFLAGS`, then immediately restore
  the guest EFLAGS.
- Following `Jcc`, `SETcc`, `CMOVcc`, and `LOOPcc` instructions consume private
  `TFLAGS` until a normal x86 flag-writing instruction takes ownership of the
  architectural EFLAGS again. This keeps T-register comparisons/loop counters
  from leaking flag changes back into the game.
- The logical private bank is now 40 bytes: T0-T7 (32 bytes), TFLAGS (4 bytes),
  and one hidden saved-game-EFLAGS scratch dword (4 bytes). The allocator still
  rounds the allocation to its existing 16-byte boundary.
- The debugger F0 Temp Registers pane now shows read-only `TFLAGS` and decodes the
  common CF/ZF/SF/OF/PF/AF bits.
- Typing or following a debugger address now resolves to the containing opcode
  start. If the requested byte is in the middle of an instruction, the input,
  selection, and navigation history are normalized to that instruction start.
- The lower debugger layout is compacted to `Current Registers | Last BP` with
  F-Type temporary state below that pair, while Breakpoints moves to the right.
  This leaves the disassembly workspace free to expand horizontally with the
  window instead of forcing the register panes to stretch across the full width.
- Added a true full-width horizontal resize handle immediately below the paired
  Virtual/Physical disassembly panes. Dragging it resizes both panes vertically
  from 160 to 1200 px; double-click resets to 320 px. The handle lives above
  the Physical-pane explanatory text so its drag target cannot be obscured.
- Fixed Follow/keyboard branch navigation so the scroll-to-destination request is
  applied equally from either Virtual or Physical pane instead of being cleared
  when the Physical pane owned keyboard focus. Right now follows the selected
  branch/call/return and positions the destination in the disassembly view.
- Single-clicked branch sources are synchronized into navigation history before
  their target is pushed, so Left returns to the exact instruction the branch was
  followed from. Context-menu Follow/Back/Forward is deferred until both panes
  finish drawing, avoiding disassembly-row invalidation while a menu is active.
- Disassembly hover is now neutral grey. Hovering a row never changes selection;
  a normal single left-click remains the action that selects an instruction.

## v0.1.64 F0 persistent T0-T7 temp registers

- Added eight F0-only virtual 32-bit scratch registers: `T0` through `T7`.
- Each active F0 hook that references a T register receives one persistent
  private state bank in the existing `680F0000-680FFFFF` private area.
  Different F0 cheats never share a T bank.
- T values are not cleared when the cave returns; the last values remain
  visible for debugging and are reused on the next execution. The bank is
  zeroed/freed only when that F0 is disabled, the XBE changes, or Type-F
  allocations are reset.
- Common syntax includes `mov T0,CarList`, `mov T1,[T0]`, `mov eax,T0`,
  `mov T0,eax`, `cmp eax,T1`, `test T1,T1`, `add T0,4`, and ordinary unary/
  shift/push/pop forms that accept a dword memory operand. `[Tn]` means a
  32-bit memory access through the pointer stored in Tn.
- The x86 Debugger Current Registers pane now adds an **F0 Temp Registers**
  section whenever an active F0 uses T0-T7. It shows the owning cheat, hook,
  cave, T-bank address, live T0-T7/TFLAGS values, and allows paused editing
  of T0-T7; TFLAGS is shown read-only. With multiple active T-using F0 cheats, a selector is
  shown and the bank owning the current cave EIP is auto-selected.
- `T0` through `T7` are reserved names and cannot be used as F0 labels.

## v0.1.62 F0 static data + register preservation

- Added F0 `DD` static-data declarations. Multiple 32-bit values may be placed on
  one line, for example `dd 01D28710, 8B80C5FC, E3BDE8CB`.
- Labels attached to DD data resolve to the final guest address automatically, so
  normal source can use forms such as `mov edx, CarList`.
- DD declarations may be written after `DEADCODE`; the parser attaches them to
  that F0 block and the assembler physically places the data after the generated
  return JMP. Code + return JMP + DD data are one allocator-owned cave block.
- Added `PRESERVEALL`, selective `PRESERVE EAX, ECX, ...`, `RESTORE`, and
  selective `RESTORE EAX, ECX, ...` directives. Cheat-style `PRESERVE, EAX` /
  `RESTORE, EAX` spellings are accepted too. Supported saved state is
  EAX/EBX/ECX/EDX/ESI/EDI/EBP/EFLAGS; ESP and EIP are intentionally excluded.
- Adjacent `PRESERVE` directives accumulate into one frame. Each later preserve
  region opens another LIFO frame, and RESTORE when nothing is saved is a no-op.
- The top 64 KiB of the existing xemu-owned Type-F mapping is now permanently
  reserved for PRESERVE/T0-T7/TFLAGS private state: `680F0000-680FFFFF`. Normal F0/F1 code and DD
  allocations are limited to `68000000-680EFFFF`.
- Each F0 hook that uses preservation receives a private block with 16 nested
  48-byte frames. RESTORE clears restored slots; a fully restored frame is zeroed
  and reused. Disabling the cave zeroes/frees its private preservation block once
  the cave is safe to reclaim.
- Preservation values live in xemu-owned external memory, not Xbox machine RAM.
  PUSHFD/PUSHAD are used only as a short internal transfer mechanism while the
  live register values are copied to/from the private preservation frames.

## v0.1.61 expanded F0 x86 assembler

- Added 8-bit registers `AL/CL/DL/BL/AH/CH/DH/BH` and 16-bit registers
  `AX/CX/DX/BX/SP/BP/SI/DI` in addition to the existing 32-bit register set.
- `MOV`, `ADD/ADC/SUB/SBB/AND/OR/XOR/CMP`, `TEST`, `INC/DEC/NEG/NOT`, and
  shifts/rotates now support useful 8/16/32-bit register and memory forms.
- `MOVZX/MOVSX` now accept register sources as well as byte/word memory sources.
- Added `RET imm16`, indirect `CALL/JMP reg32` and `CALL/JMP dword ptr [memory]`,
  plus word/dword memory forms for `PUSH/POP`.
- Added common stack/flags/helper instructions: `PUSHAD/POPAD`, `PUSHFD/POPFD`,
  `LAHF/SAHF`, `CLC/STC/CMC`, `CLD/STD`, `CBW/CWDE`, `CWD/CDQ`, `LEAVE`,
  `INT3`, and `IRET`.
- Expanded multiply/divide support: one/two/three-operand `IMUL`, plus `MUL`,
  `DIV`, and `IDIV`.
- Added `XCHG`, `XADD`, `CMPXCHG`, `BSWAP`, `BSF/BSR`, `BT/BTS/BTR/BTC`,
  `SHLD/SHRD`, `SETcc`, and `CMOVcc`.
- Added `LOOP/LOOPE/LOOPNE/JECXZ` label branches and common x86 string
  instructions with `REP/REPE/REPNE` prefixes.
- FPU/MMX/SSE/SSE2 remain intentionally outside the built-in F0 assembler for
  now; F1 remains available for exact raw machine code.

---

# xemu Debug Tools Extensions

This directory contains the custom RAW Cheat Engine, Current Game manager,
Memory/Map tools, detached tool windows, x86 debugger UI, and the small C
bridge used to access QEMU/xemu CPU and memory facilities.

The goal of this layout is to keep custom tooling separate from upstream xemu
sources. The custom implementation lives here; upstream files should contain
only the minimum integration hooks needed to expose and render the tools.







## v0.1.60 compile fix

- Fixed the Windows-only backport build after the Type-F cave reclamation helper
  began taking `XemuCheatX86Registers` by reference.
- `XemuCheatX86Registers` already lives in `cheat-engine-memory.h`;
  `cheat-engine.hh` now forward-declares that existing type instead of creating
  a duplicate definition or incorrectly placing it in the F0 assembler header.
- No debugger, Type-F, allocator, or code-format behavior changed.

## v0.1.59 debugger navigation + live Current Registers

- x86 disassembly navigation now behaves like a small code browser: Right follows a selected
  JMP/Jcc/CALL/RET target, Shift+Right follows fall-through, Left goes Back, and Alt+Right goes Forward.
  Navigation never executes the guest and is separate from Step Into.
- Direct targets are followed from the disassembly itself. While paused, register-indirect targets,
  common `[base+index*scale+disp]` memory-indirect targets, and `RET` via `[ESP]` are resolved from the
  live Current Registers/RAM when possible; unresolved targets are reported rather than guessed.
- x86 right-click adds `Follow > Branch / Call Target / Fall Through / Return Target` and
  `Navigation > Back / Forward`. Double-click remains Execute-breakpoint creation.
- The disassembly branch gutter uses baby blue for backward/negative JMP/Jcc links and yellow for
  forward/positive JMP/Jcc links, including edge arrows when a direct target is outside the visible rows.
- Current Registers is now editable while the guest is paused/debug-stopped. Double-click a live
  value or use right-click `Edit Register Value`; writes go through QEMU's GDB register bridge and WHPX
  uploads the dirty CPU state before execution resumes.
- Current-register right-click also provides `Copy > Register / Value` and `View In > Memory / x86 Debugger`.
  `PC` is derived and remains read-only; edit `EIP` instead. Registers at Last Breakpoint stays read-only.

## v0.1.58 Type-F allocator + unified context menus

- Type-F external memory now uses a 16-byte-aligned first-fit free-list allocator.
  Each active cave keeps a fixed address for its lifetime; caves are never compacted
  or shifted when another cave grows.
- Actual Type-F install/update transactions pause a running guest until the new cave
  bytes, generated return JMP, and guest hook JMP are all written, preventing WHPX
  from observing partially-written executable code. An already-paused/debug-stopped
  guest remains stopped.
- External cave bytes are written directly to xemu's cheat-owned RAM backing after
  allocation establishes the guest mapping, then translation state is synchronized.
  This removes the redundant second mapping/write path that could fail on reactivation.
- Disabling F0/F1 pauses/synchronizes the guest, restores the original hook, and
  clears/frees the retired cave when EIP is outside that allocation and the
  live stack has no return address back into it. If execution/reference is still
  active, the block stays retired and the disabled-cheat Tick path
  retries later instead of erasing live instructions.
- Free neighboring blocks are coalesced. A larger edited cave reuses the old address
  only when the resulting free block is large enough; otherwise it receives another
  free address while every other active cave remains unchanged.
- Memory, Search, and x86 Debugger rows now share the same right-click layout:
  `Dump Ram > Current Page / DUMP PHYSICAL / DUMP MAPPED VIRTUAL RAM / DUMP PHYSICAL + DUMP MAPPED VIRTUAL RAM`,
  `Copy > Address / Value / x86 Instructions`, and `Break Point > Exe / Read / Write / Read/Write`.
- `View In` is context-specific: Memory -> x86 Debugger; Search -> x86 Debugger or
  Memory; x86 Debugger -> Memory.
- Double-clicking an x86 disassembly row toggles the Execute breakpoint at that
  row's guest virtual instruction address: add/re-enable when absent/disabled, remove when active.


## v1.69 conditional breakpoints + Current Registers tabs

- The Breakpoints table adds a clickable `Condition` column. `NO` means the
  breakpoint has no conditions; `YES` means one or more conditions are active.
  Both values open the same editor so conditions can always be added, edited,
  or removed.
- The condition editor accepts one register comparison per non-empty line.
  Multiple lines are ANDed. Supported operators are `==`, `!=`, `<`, `<=`,
  `>`, and `>=`; each operator has a plain-English hover tooltip. Values are
  hexadecimal with an optional `0x` prefix.
- Conditions are debugger-side filters. The backend breakpoint/watchpoint still
  triggers normally, xemu snapshots the architectural registers, and a false
  condition resumes automatically. Execute-breakpoint filtering reuses the
  existing TCG/KVM/WHPX step-over paths so a false condition cannot immediately
  retrigger at the same EIP.
- The existing Current Registers grid is preserved as the `General` tab inside
  the same pane. Additional tabs expose `x87 / FPU` (ST0-ST7 plus FCTRL/FSTAT/
  TOP/FOP), `MMX` (MM0-MM7), and `SSE` (XMM0-XMM7 plus MXCSR). The existing
  `Last BP` pane remains unchanged.

## v1.69 split full-RAM dump modes

- The Dump RAM tab now exposes three independent full-range actions:
  `DUMP PHYSICAL`, `DUMP MAPPED VIRTUAL RAM`, and
  `DUMP PHYSICAL + DUMP MAPPED VIRTUAL RAM`.
- `DUMP PHYSICAL` writes only the complete installed Xbox RAM `.bin`; it does not
  require building/scanning the virtual page map.
- `DUMP MAPPED VIRTUAL RAM` scans the full 32-bit guest virtual address space and
  writes only RAM-backed contiguous virtual regions plus `VIRTUAL-MAP.txt`; MMIO
  remains excluded and aliases remain represented.
- The combined action preserves the prior behavior and produces both sets from the
  same paused guest state. All three full-range actions leave the VM paused after
  completion for debugger inspection.
- The same three full-range actions are also available under the shared
  `Dump Ram` right-click submenu, below the existing `Current Page` action.

## x86 Debugger Inject menu

- Right-click a Virtual or Physical disassembly row and choose `Inject > NOP` to
  replace exactly that complete x86 instruction with `90` bytes. A running guest
  is paused for the patch and resumed only if it was running beforehand.
- `Inject > Change` opens a compact one-instruction editor showing the selected
  address, current bytes/instruction, an editable replacement assembly line, and
  the exact replacement-byte preview. Replacements must fit inside the original
  instruction span; shorter instructions are NOP-padded and longer ones are rejected
  with a prompt to use CodeCave. APPLY patches transactionally and RESTORE is guarded
  so it will not overwrite newer guest bytes that changed after the manual patch.
- `Inject > CodeCave` opens a compact Type-F0 builder using the selected virtual
  instruction as `$F0000000 AAAAAAAA`. The starter source automatically copies
  every complete instruction that the 5-byte hook JMP will overwrite, then adds
  `$DEADCODE`.
- The preview shows the current overwritten instruction span and the future JMP /
  return address before RUN. After RUN it shows the actual allocated cave address
  and hook bytes, and the main disassembly refreshes to the live JMP.
- RUN calls the same Type-F0 assembler, external allocator, hook sizing, original-byte
  save/rollback, T-register/PRESERVE handling, and retired-cave safety path as normal
  Cheat Engine F0 codes. RESTORE explicitly puts the saved original bytes back.
- NOP/Change/CodeCave injection is blocked when another active Type-F hook or cave owns the
  selected address, preventing the debugger from silently corrupting an active cheat.

## v0.1.42 true single-step fix

- Fixes **Step Into** under WHPX so it resumes through QEMU's step-aware VM
  startup path instead of ordinary `vm_start()`.
- WHPX now receives `step_pending=true` before execution resumes, ensuring the
  next x86 Trap Flag (`#DB`) is intercepted even when no execute breakpoint or
  data watchpoint is active.
- Prevents Step Into from running many instructions and appearing to behave
  like Step Over/Step Out.
- The fix remains entirely inside `ui/xui/debug-tools/`; no upstream WHPX/QEMU
  source modification is required.

## v0.1.41 live-register and breakpoint workflow

The debugger now keeps Current Registers live beside the frozen Registers at
Last Breakpoint snapshot. Execute and data breakpoints share one Address/Type/Len
control row and one combined list. Memory Viewer row addresses/byte values and
Search result addresses expose right-click breakpoint creation; Search also
provides View in Memory. Physical addresses are translated through the current
memory-map alias before debugger breakpoints are created.

## v0.1.40 actual watchpoint-access highlight

Read/Write watchpoint #DB exits occur after the memory instruction executes.
The debugger now resolves the instruction immediately ending at that stop EIP
and makes that actual memory-access instruction the persistent light-blue row.
The architectural Current EIP remains visible separately. Execute-breakpoint
highlighting is unchanged. If the previous instruction cannot be resolved
without guessing, the stop EIP is used as the fallback highlight.

## v0.1.39 full-page breakpoint disassembly

Breakpoint/watchpoint follow now uses a scrollable 4 KiB disassembly page by default.
The architectural debugger stop EIP is highlighted light blue in both Virtual and
Physical panes, and the initial scroll position leaves preceding instructions visible.
The previous Count-based forward view remains selectable from the debugger toolbar.
All of this implementation remains inside `ui/xui/debug-tools/`.

## v0.1.38 breakpoint backend layout

`backend/xemu-dbg.c` initializes QEMU's internal gdbstub state with the special
`none` transport before creating `BP_GDB` debugger objects. `backend/whpx-debug.c`
contains the WHPX guest-debug implementation and now mirrors data watchpoints
into x86 DR0-DR3 hardware debug registers.

WHPX supports four hardware data slots. Write and Read/Write normally consume
one slot per aligned 1/2/4-byte chunk. Read-only is implemented with a paired
Read/Write + Write slot so DR6 can distinguish reads from writes; this consumes
two slots per chunk. The upstream WHPX sources only contain the minimum hooks
needed to keep #DB interception active and forward a debug trap to this backend.

## Files

- `cheat-engine.cc/.hh` - CMP/RAW cheat parser, UI, and runtime engine.
- `cheat-engine-memory.c/.h` - C bridge to QEMU/xemu memory, mapping,
  disassembly and breakpoint/debug facilities.
- `current-game.cc/.hh` - current XBE/title/revision tracking.
- `detached-tools.cc/.hh` - independent SDL/OpenGL/ImGui tool windows.
- `memory-tools.cc/.hh` - linked Physical/Map/Virtual memory workspace,
  search/compare, RAM dump and x86 debugger UI.
- `backend/xemu-dbg.c/.h` - target-specific QEMU execute/data breakpoint bridge, adapted from Josh's debugger design; TCG uses QEMU watchpoints directly and WHPX delegates them to the hardware backend.
- `backend/whpx-debug.c/.h` - WHPX guest-debug registration plus DR0-DR3 hardware Read/Write watchpoint implementation.
- `meson.build` - all custom source registration, target-specific debugger backend registration, and Capstone dependency.

## Upstream integration points

The intended upstream-touch surface is small:

- `ui/xui/main.cc` - initialize, tick, draw and route detached tool windows.
- `ui/xui/menubar.cc` - Debug menu entries.
- `ui/xui/meson.build` - `subdir('debug-tools')`.
- `ui/xemu.c` - filters console-window expose/resize events so detached tool
  windows cannot trigger console redraw handling.
- `target/i386/whpx/whpx-accel-ops.c` - one debug-tools header include and one
  `xemu_debug_register_whpx_ops(ops)` registration call.
- `target/i386/whpx/whpx-all.c` - small hooks to keep debug-exception exits
  enabled for data watchpoints and forward #DB events to the debug-tools backend.

Other project/workflow changes used by this fork (for example the Windows-only
release build and Capstone provisioning) remain outside this directory because
they are build-system concerns rather than debugger implementation code.

### Type-F stable allocation/reactivation lifecycle

- Deactivation restores the original hook before a cave can be reclaimed.
- A stopped guest is checked before retired cave bytes are cleared; a cave caught at the current EIP is left intact and retried later.
- Freed 16-byte-aligned blocks return to a first-fit free list and adjacent holes are coalesced.
- Active caves never move. Growing one cave cannot shift or rewrite neighboring cave addresses/hooks.
- Editing/reloading a Type-F cheat can therefore safely allocate the same freed block when it fits or another free block when it does not.
- Mutually-exclusive Type-F cheats targeting the same hook can hand off the inactive hook state cleanly.
- Hook-install failures report the exact failed stage instead of only `Type-F Hook Installation Failed`.

## v0.1.63 Type-F PDE accessed-bit allocation fix

- Fixes repeated F0/F1 activation/allocation failures after the guest has executed the 0x68000000 Type-F mapping.
- The IA-32 CPU may set the page-directory Accessed bit; Type-F now validates the PDE page-table base and required mapping flags instead of requiring a byte-for-byte pristine PDE value.
- Allocation errors now report whether the guest mapping is unavailable/conflicting versus the 960 KiB code/DD arena actually being exhausted or fragmented.

## v0.1.64 debugger T-register view

The x86 Debugger shows a live `F0 Temp Registers` block under Current Registers
for active F0 cheats that use T0-T7. Values are persistent, can be edited while
paused, and can be viewed either as their value target or as their private
storage address. With multiple active T-using F0 cheats the pane has a selector;
when EIP is inside one of those caves that bank is selected automatically.

## v1.69 Current Game mounted-disc browser + full XBE SHA-256

- `Current Game` now separates the existing loaded-header hash from a new
  `XBE SHA-256`, which hashes the complete `default.xbe` file directly from the
  currently mounted `ide0-cd1` Xbox DVD backend. The browser never reopens a
  host ISO filename, so the displayed hash follows the media actually mounted
  in xemu.
- The window now has `Game Info` and `Disc Contents` tabs. `Disc Contents` is a
  read-only XDVDFS tree showing file/folder name, type, size, starting sector,
  and absolute disc-image offset. `Refresh Now` rescans the mounted medium and
  recalculates the full `default.xbe` SHA-256.
- The XDVDFS parser validates both volume-descriptor signatures, bounds-checks
  directory/file offsets, caps malformed nesting/table sizes/entry counts, and
  supports the common game-partition base offsets recognized by Xbox XISO
  tooling.
- The register-family tabs remain drawn only above `Current Registers`, but the
  selected General/x87-FPU/MMX/SSE family controls both Current Registers and
  Last BP. Last BP reserves matching tab-row height and snapshots the selected
  extended register state at accepted breakpoint/watchpoint hits.

## v1.71 mounted-disc build compatibility fix

- Moves QEMU `BlockBackend` headers and calls out of `current-game.cc` into the
  C-only `disc-block-io.c` bridge. This prevents QEMU C11 `_Generic` lock macros
  from being parsed by the C++ Current Game translation unit.
- The bridge passes an explicit `(BdrvRequestFlags)0` to `blk_pread()`, fixing
  the strict C++ `int` -> `BdrvRequestFlags` conversion failure while keeping
  mounted-disc reads read-only and behavior-identical.
- Current Game disc browsing and full `default.xbe` SHA-256 continue to read the
  currently mounted `ide0-cd1` backend; no host ISO/XISO path is reopened.


## v1.72 XBE labels

The x86 debugger can build a read-only label database from the complete
`default.xbe` on the currently mounted Xbox DVD. The XBE virtual address is
the stable master address; the Physical column is translated from the current
guest page tables at display/export time.

Label sources include the XBE entry point, section starts, Xbox kernel thunk
ordinals (resolved through the CC0 XboxDev/nxdk export list), useful ASCII
strings, MSVC RTTI strings, exact immediate xrefs from the title `.text`, and
clearly marked inferred function starts. Inferred names begin with `~` and are
heuristics, not original PDB symbols.

`Labels` toggles disassembly annotations. `LABELS` opens the searchable Current
Labels browser with Virtual/Physical jump actions. `DUMP LABELS` writes a text
index containing each stable Virtual address and its current Physical mapping
(or `UNMAPPED`).


## v1.73 Inject Change original-history + direct branch addresses

- `Inject > Change` remembers the first original instruction bytes and ASM for
  each changed virtual address for the current xemu session. Closing/reopening
  the editor no longer loses the original state.
- The editor shows both `Original instruction (remembered)` and the live
  `Current instruction`. An active tracked patch can be restored later with
  `REVERT TO ORIGINAL`; the restore is still guarded by an exact live-byte
  comparison so it cannot silently overwrite a newer/external patch.
- Reopening a changed instruction keeps the original instruction span even when
  the replacement decoded to a shorter instruction plus NOP padding.
- Direct hexadecimal control-flow targets are accepted by Change, for example
  `jmp 0008C5A2`, `jne 0x0008C5A2`, and `call 0008C5A2`. Nearby direct JMP/Jcc
  targets use their 2-byte short encoding when that is the only/smallest safe
  fit; farther targets use the normal rel32 encoding.
- The general Type-F0 assembler also accepts absolute hexadecimal JMP/Jcc/CALL
  targets while preserving existing label behavior and cave sizing.

## v1.74 portable `.xlabel` label packs

- Adds a root `Labels` workspace beside the xemu executable with four isolated
  subfolders created on demand:
  - `Labels/XDK/` for future local XDK reference/import files.
  - `Labels/PDB/` for future local PDB reference/import files.
  - `Labels/Packs/` for portable/distributable `.xlabel` files.
  - `Labels/Cache/` for future locally generated indexes/caches.
- Adds a portable `.xlabel` format whose header deliberately mirrors the Cheat
  file identity fields: `^1 = Hash`, `^2 = GameID`, and `^3 = NAME`. It also
  records the complete `default.xbe` SHA-256 when available and a format version.
- Label records persist XBE section + section-relative offset as their stable
  location. The absolute Virtual address is retained only as a revision-bound
  hint/fallback for labels that are outside normal XBE sections.
- Physical addresses are never stored in `.xlabel`. The debugger continues to
  translate Virtual -> Physical from the running Xbox page tables, so a launch
  whose Physical RAM backing shifts does not invalidate a label pack.
- Matching `.xlabel` files in `Labels/Packs/` are loaded automatically for the
  current GameID/Header hash. The full `default.xbe` hash is an additional
  revision guard when both the pack and mounted disc provide it.
- The Current Labels window adds `LABELS FOLDER`, `LOAD .xlabel`, `SAVE .xlabel`,
  and `RELOAD PACKS`. Saved packs can be copied to another XEMU - CHEATS install
  without distributing the original XDK/PDB input files.
- Labels now retain provenance (`XBE`, `XDK`, `PDB`, `MANUAL`) and confidence
  metadata. Manual/PDB/XDK names can take primary-display priority over an
  automatically inferred XBE name at the same Virtual address.
- This version establishes the portable label-pack/import foundation only. XDK
  and PDB source parsers are intentionally separate follow-up work; no Microsoft
  XDK/PDB code or binary data is included in the repository by this feature.

Generated `.xlabel` files use a simple text layout similar to the Cheat files:

```text
^1 = Hash: <loaded XBE header SHA-256>
^2 = GameID: EA009E
^3 = NAME: Example Game
^4 = XBEHASH: <complete default.xbe SHA-256>
^5 = FORMAT: 1

[LABELS]
; SECTION|OFFSET|VIRTUAL_HINT|TYPE|SOURCE|CONFIDENCE|LABEL
.text|00099230|001A9230|INFERRED|MANUAL|MANUAL|UnlockSystem_IsDLCUnlock
```

## v1.75 local XDK label importer

- Adds a local-only XDK importer on top of the v1.74 `.xlabel` foundation.
- The current XBE library-version table is parsed (for example `XAPILIB 5849`,
  `D3D8 5849`, `D3DX8 5849`, `XGRAPHC 5849`, `LIBCMT 5849`, and `DSOUND 5849`).
- `Labels/XDK/<build>/` is searched recursively, so a normal extracted XDK tree
  such as `Labels/XDK/5849/XDK/xbox/lib/` can be used without reorganizing its
  internal folders.
- `BUILD / REFRESH XDK INDEX` reads only the local matching `.lib` files and
  builds `Labels/Cache/XDK-<build>.xdkidx`.
- The cache contains symbol names, function sizes, relocation masks, and
  fingerprints only. It deliberately does **not** store the original XDK
  machine-code bytes, `.obj` members, `.lib` contents, PDB streams, or source.
- COFF relocations are normalized before fingerprinting so linker-fixed calls,
  addresses, and other relocation fields can still match the linked XBE.
- Only unambiguous fingerprints with a unique current-XBE match become labels;
  short functions below 16 bytes and unsupported relocation forms are skipped
  rather than being guessed.
- Exact matches are added as `FUNCTION` labels with `Source = XDK` and
  `Confidence = EXACT`, and continue to use XBE section-relative locations.
  Physical addresses are still resolved live from the running page tables.
- Current Game now shows XDK build/library/signature/exact-label/cache status.
  The Current Labels window adds the index build/refresh control and a Source
  filter (`XBE`, `XDK`, `PDB`, `Manual`).
- XDK-derived labels can be exported with the existing `.xlabel` saver. A user
  receiving that `.xlabel` does not need the original XDK or the local cache.

## v1.76 Microsoft linker MAP label importer

- Adds `LOAD .map` to the Current Labels window. The file picker starts in
  `Labels/PDB/`, so game `.map` and later `.pdb` inputs can be kept together.
- Microsoft linker MAP files are parsed locally; no MAP/PDB content is embedded
  in xemu or copied into the source tree.
- MAP `segment:offset` symbols are resolved against the current XBE section
  table, so imported labels remain XBE/Virtual-relative and Physical addresses
  continue to be resolved live from the running page tables.
- Function records become `FUNCTION` labels; non-function public/static records
  become `SYMBOL` labels. A lightweight MSVC-name cleanup makes common names
  such as `?Pause@cBackgroundLoader@@...` display as
  `cBackgroundLoader::Pause` without adding a Microsoft ABI library.
- MAP labels use `Source = MAP` and `Confidence = EXACT`. `MAP` is available in
  the Current Labels Source filter and survives portable `.xlabel` export.
- Safety is intentionally strict: the MAP linker timestamp must match the
  current XBE's original PE timestamp, and mapped segment spans must fit the XBE
  sections. A different build is parsed for diagnostics but **no addresses are
  applied**. This prevents a near-match MAP from silently naming the wrong code.
- Current Game and Current Labels show MAP timestamp, symbol, section, and exact
  label counts after an import attempt.
- The supplied Crusty Demons research set is a useful negative/lineage test:
  `Crusty.map` reports timestamp `4476D0D9`, while the supplied `Default.xbe`
  reports PE timestamp `44771B02`. Even if the timestamp is artificially made
  equal, the `.text` segment is larger than the current XBE `.text` section, so
  v1.76 correctly rejects it as a different build rather than importing stale
  addresses.


## v1.77 Microsoft PDB label importer

- Adds `LOAD .pdb` beside the MAP importer in Current Labels. The picker starts
  in `Labels/PDB/` so a game's XBE, MAP, and PDB research files can stay grouped.
- Reads Microsoft MSF 7.00 PDBs locally and imports CodeView `S_PUB32` public
  function/data symbols. The original PDB is never copied into xemu source, an
  `.xlabel`, or a distributable cache.
- Reads the current XBE's embedded `RSDS` record and displays its original PDB
  path, GUID, and Age in Current Game.
- Safety is intentionally strict: PDB symbols are applied only when the PDB GUID
  **and Age** exactly match the current XBE and the PDB's original section layout
  fits the XBE section table. A same-GUID/different-Age file is reported as the
  same symbol lineage but a different link and contributes zero labels.
- Public code/function symbols become `FUNCTION`; public data symbols become
  `SYMBOL`. Common MSVC decorated names receive the same lightweight cleanup used
  by the MAP importer. Labels use `Source = PDB`, `Confidence = EXACT`, and PDB
  has higher primary-display priority than MAP/XDK/XBE labels at the same address.
- PDB segment offsets are converted to XBE section-relative locations. Physical
  addresses remain runtime-only and continue to be translated through the current
  Xbox page tables.
- Imported PDB labels can be saved through `SAVE .xlabel`; recipients of the
  resulting label pack do not need the original PDB.
- The supplied Crusty Demons PDB is a useful safety case: its GUID matches the
  XBE (`BB75F5CA-2E73-4229-9F5E-904AC286D4C8`) but its PDB Age is 2 while the
  XBE requests Age 5, so v1.77 identifies the lineage and deliberately applies
  zero PDB addresses. Its older `.text` layout also exceeds the supplied XBE's
  `.text` span.

## v1.78 same-address Type-F handoff fix

- Fixes the normal checkbox workflow where one F0 is enabled, unchecked, and a
  different F0 using the **same hook address** is then checked.
- Restoring the first F0 still happens immediately. If its old executable cave
  is still running (or a `CALL` can still return into it), those old cave,
  PRESERVE, and T0-T7/TFLAGS allocations are detached into a private retirement
  queue instead of blocking the replacement hook.
- The second F0 receives a fresh cave and can install at the restored hook
  immediately; it never overwrites or reuses the first cave while that cave may
  still be referenced.
- Retired caves are checked every Cheat Engine tick and freed only after EIP and
  pending CALL-return checks prove that the old code is no longer live.
- Retired executable ranges remain protected by the existing Inject ownership
  guard until reclamation, so NOP/Change/CodeCave cannot edit code that may
  still be finishing execution.
- A title/XBE change clears both active and retired F0 state before resetting the
  external allocator, preserving the existing cross-title safety rule.
- The global Enabled/Disabled button is **not required** for this handoff; the
  intended workflow is simply `A checked -> A unchecked -> B checked`.


## v1.79 virtual memory-map optimization

- Memory Map and Mapped Virtual RAM dump now ask QEMU's x86 page-table walker
  for a point-in-time mapping snapshot instead of probing all 1,048,576 4 KiB
  virtual pages one by one.
- The snapshot is restricted to installed Xbox RAM, expanded into the existing
  Virtual/Physical page model, and preserves alias/linear-region behavior.
- The original exhaustive page probe remains as a compatibility fallback when
  an accelerator cannot provide the generic mapping snapshot.


## v1.80 disassembler bulk-read / Capstone optimization

- Sequential paired disassembly now caches 4 KiB guest pages instead of issuing
  up to fifteen guest-memory reads for every x86 instruction.
- Capstone uses one reusable instruction object with `cs_disasm_iter()` instead
  of allocating/freeing a decode result for every row.
- Full-page disassembly translates the page's Physical base once and derives row
  Physical addresses by offset.
- Instruction bytes, unmapped-page stopping behavior, and exact focus-address
  resynchronization remain unchanged.


## v1.81 Type-F precompile / retired-cave optimization

- F0 probe assembly, F1 raw parsing, and Type-F definition signatures are now
  prepared once when the cheat source is parsed instead of rebuilt every tick.
- F0 final assembly still happens once at the allocator-selected cave address,
  preserving label/DD/PRESERVE/T-register relocation behavior.
- Each installed cave caches its real CALL return sites once; retired-cave
  checks no longer redisassemble immutable code every cleanup pass.
- Retired caves still in use use a bounded tick backoff up to about one second,
  reducing repeated stack scans while preserving the v1.78 same-address handoff.


## v1.82 label generation/filter/batch-merge optimization

- Current Labels caches its filtered index and rebuilds it only when the label
  database generation, search text, Type filter, or Source filter changes.
- XBE/XDK/MAP/PDB/portable-pack labels are appended first and canonicalized
  once, instead of sorting the entire growing database after every source.
- Existing Merge() semantics remain available for standalone/manual imports.


## v1.83 XBE/PDB/MAP file-I/O optimization

- Mounted `default.xbe` is read once: the same buffer is SHA-256 hashed, parsed
  for labels/PDB identity, and then moved into Current Game storage.
- PDB files are read directly into their final byte vector instead of a GLib
  allocation followed by a second full-size vector copy.
- MAP files are read directly into their final `std::string` instead of a GLib
  buffer followed by another full text copy.
- Existing size limits and parser validation remain in place.

## v1.84 debugger/Cheat Engine streamlining cleanup

- The paired x86 disassembly panes now cache their complete per-row display
  strings between disassembly/label changes. Instruction-byte formatting and
  primary-label lookup happen once per refresh instead of once per visible row
  in both panes on every rendered frame.
- The exact-address/interior-opcode lookup used by refresh alignment and
  scroll-to-focus is shared and uses binary search over the already sorted
  disassembly rows instead of two separate linear scans.
- After a RUN_STATE_DEBUG stop has already been processed, the hit-state path
  no longer performs a second architectural-register backend read every frame;
  the existing 100 ms live-register refresh remains authoritative while paused.
- Cheat Engine Tick now identifies all disabled normal cheat-owned F hooks with
  one hook-table scan and retires them inside one guest pause window. The old
  block-by-block ordering is retained, while debugger-owned Inject > CodeCave
  hooks remain independent of the Cheat Engine global/live enable checkboxes.
- No code type, F0/F1 allocation, breakpoint/watchpoint, UI layout, label text,
  navigation, or memory read/write semantics are intentionally changed.

## v1.85 OPT Pass 1

- Memory Viewer Physical/Virtual panes now snapshot each visible 4 KiB guest
  page at most once per pane/frame instead of reading every visible 16-byte row
  separately. The cache is frame-local, so live-memory refresh behavior is
  unchanged, and the previous short-span read remains as a fallback for unusual
  partial mappings/MMIO where a full-page debug read fails.
- The Memory Viewer page cache uses a fixed four-entry ring, avoiding a new heap
  allocation in the render hot path.
- Active F0 T0-T7/TFLAGS display metadata is cached by the Cheat Engine and
  borrowed directly by the debugger. The sorted cache is rebuilt only when
  source/game/hook lifecycle state changes instead of every rendered frame.
- RAM virtual-mapping snapshots allocate their result array once from QEMU's
  known mapping-count upper bound instead of reallocating for every accepted
  mapping.
- Shared source-test helpers remove duplicated brace-balanced function extraction
  code, and a dedicated Pass-1 regression guard freezes these optimizations.

## v1.86 OPT Pass 2

- Capstone x86 decoder state is now retained in a per-thread disassembly
  context. Each thread opens/configures its Capstone handle and allocates the
  `cs_disasm_iter()` instruction cache once, then reuses them for later page,
  paired, Inject, Type-F, and watchpoint helper decodes.
- The same context retains the two paired-disassembly 4 KiB page buffers and
  the full-page scratch buffer. They are resized only if QEMU's runtime target
  page size changes instead of allocated/freed for every disassembly request.
- Debugger Count mode reuses the already decoded full-page rows used to align
  an interior address whenever the requested instruction slice is safely
  inside that page. The final 14 bytes deliberately keep the paired decoder so
  a valid x86 instruction crossing the 4 KiB boundary preserves the previous
  behavior.
- RefreshDisassembly reuses the selected decoded row's already resolved
  Physical address instead of preparing/translating the same Virtual address a
  second time after decoding.
- Virtual and Physical debugger panes continue to consume the same paired row
  set; no separate decode/cache state was introduced for either pane.
- Existing Capstone failure results, exact focus-address resynchronization,
  page-boundary behavior, instruction bytes/text, Inject > NOP/Change/CodeCave,
  Type-F hook analysis, and breakpoint/watchpoint helper paths are otherwise
  unchanged.
- The disassembler golden guard now protects the reusable-context lifecycle,
  and `pass2-streamlining-golden.py` freezes the Count-mode page-slice reuse and
  conservative cross-page fallback.

## v1.87 debugger navigation focus fix

- Browser-style x86 navigation now hands ImGui keyboard focus to the actual
  destination instruction after Right/Shift+Right Follow, Left Back, and
  Alt+Right Forward. Up/Down therefore continues from the newly selected
  destination instead of the pre-navigation source row.
- The active Virtual/Physical pane is preserved across that focus handoff. A
  Follow initiated from Physical stays keyboard-focused in Physical, and the
  same applies to Virtual.
- Generic debugger jumps such as Go to EIP, breakpoint Go, and label navigation
  retain their existing focus behavior; only browser-style navigation performs
  this keyboard-focus transfer.
- Navigation history, exact-source Back behavior, opcode-start alignment, branch
  target resolution, scrolling, and guest execution state are unchanged.

## v1.88 OPT Pass 3 structural / technical-debt cleanup

- `memory-tools.cc` is split by ownership into small core, Memory Viewer/map,
  Search, Debugger, Inject, Labels, and Dump translation units. The public
  `MemoryToolsWindow` state layout and method declarations remain unchanged.
- Shared implementation-only constants/formatting/address helpers moved to
  `memory-tools-internal.hh`; this header is private to the split translation
  units and is not exposed through `memory-tools.hh`.
- The split is deliberately move-only for behavior-bearing code. All 99
  `MemoryToolsWindow` member bodies and the shared helper bodies are byte-for-
  byte identical to the v1.87 baseline.
- Source-level regression tests now read the complete split MemoryTools
  implementation rather than assuming every method lives in `memory-tools.cc`.
  This removes a source-location dependency that previously made harmless file
  ownership refactors look like behavioral regressions.
- `pass3-structural-refactor-golden.py` fingerprints every v1.87 MemoryTools
  member/helper body plus the protected Cheat Engine, F0/F1, Capstone bridge,
  external CodeCave-memory, and public MemoryTools header files. It also guards
  Meson ownership of every split unit and prevents the core file from growing
  back into the previous monolith.
- No Cheat Engine, F0/F1 lifecycle, Inject > NOP/Change/CodeCave, breakpoint/
  watchpoint, disassembler navigation, Memory Viewer behavior, UI layout, v1.87
  navigation-focus fix, or v1.86 Capstone optimization behavior is changed by
  this pass.


## v1.89 OPT Pass 4 debugger QoL / persistent preferences

- Debugger-owned display preferences now persist through xemu's existing
  `xemu.toml` settings tree without enabling ImGui's global `.ini` storage.
- Persisted values are the horizontal disassembly pane height, Full Page/Count
  mode, Count instruction total, Follow EIP, Labels visibility, and the selected
  Current Registers tab (General/x87-FPU/MMX/SSE).
- Preference loading is lazy on the first Memory Tools draw, after xemu's
  normal settings load has completed. Invalid/manual values are clamped to the
  existing UI ranges (160-1200 px, 1-128 instructions, register tab 0-3).
- Register-tab restoration notices a change of ImGui context, so the selected
  tab also follows the Memory Tools window into a detached debugger context.
- `RESET UI` restores only those six debugger display preferences to their
  established defaults. It does not touch breakpoints/watchpoints, navigation
  history, Inject state, addresses, register snapshots, guest execution, Cheat
  Engine state, or F0/F1 lifecycle.
- Preference changes update only the in-memory xemu configuration during draw;
  the existing exit-time settings save remains responsible for disk I/O.
- The v1.88 split, v1.87 navigation-focus behavior, and v1.86 Capstone path are
  otherwise unchanged.

## v1.90 OPT Pass 5 Current Game / recurring-runtime polling cleanup

- Current Game keeps an exact copy of the most recently derived in-memory XBE
  header block. The existing 500 ms poll still rereads the complete loaded
  header before making any cache decision; title/version/base fields alone are
  deliberately not treated as proof because the full loaded-header SHA-256 is
  part of game/revision identity.
- When the complete header length and every byte are identical, the poll skips
  repeated SHA-256 calculation, UTF-16 title conversion, revision-key
  construction, and equivalent `GameInfo` string assignment. Mounted-disc
  backend identity/length polling still runs, so disc changes remain visible
  even while the running XBE is unchanged.
- `Refresh Now` bypasses the derived-state cache and retains the previous full
  refresh behavior. A failed title conversion or SHA calculation also leaves
  the cache non-authoritative so the next poll retries instead of freezing a
  transient failure.
- The stable no-XBE/frontend state no longer rebuilds and assigns an equivalent
  empty `GameInfo` every 500 ms; disc polling and the valid-XBE -> no-XBE
  generation transition remain unchanged.
- `xemu_get_xbe_info()` now retains its small header scratch allocation while
  capacity is sufficient instead of free+malloc on each successful poll. The
  complete guest XBE header is still reread and reparsed every call, preserving
  exact revision/change detection and all existing XBE consumers.
- No Cheat Engine/F0/F1, Inject, breakpoint/watchpoint, disassembler navigation,
  Memory Viewer, debugger layout/preference, label/disc parsing, or UI behavior
  is intentionally changed by this pass.

## v1.91 OPT Pass 6 debugger steady-state / render-path cleanup

- The Current Registers pane keeps the established 100 ms architectural
  register refresh used by EIP highlighting/navigation, but no longer asks the
  debugger backend for x87/MMX/SSE state while the General tab is selected.
  Entering x87/FPU, MMX, or SSE from General performs an immediate same-frame
  extra-register refresh before drawing the selected table, so visible register
  behavior and breakpoint snapshots remain unchanged.
- Breakpoint/watchpoint Physical columns now use a fixed 16-entry, frame-local
  4 KiB page-translation cache while the guest is stopped and the current
  virtual map has been synchronized. Breakpoints sharing a page therefore avoid
  repeated page walks during the same table draw. The cache is never retained
  across frames, never used while the guest is running, and falls back to the
  original direct translator when preparation is unavailable or the fixed cache
  is full.
- `DrawDebugger()` and `DrawBreakpoints()` each take one frame-local debugger
  backend snapshot instead of repeatedly querying the unchanged accelerator
  backend while rendering the same frame.
- The frame-end debugger preference mirror still owns the same six v1.89
  values, but now writes an in-memory config field only when its value actually
  changed. No settings-file I/O was added.
- No new `MemoryToolsWindow` state was introduced. The v1.88 split, v1.89
  preference ownership, v1.90 Current Game polling, v1.87 navigation-focus fix,
  v1.86 Capstone path, Memory Viewer behavior, Inject behavior, and Cheat/F0/F1
  lifecycle are unchanged.

## v1.92 OPT Pass 7 disassembly / label display-cache cleanup

- The paired Virtual/Physical disassembly panes now validate their shared
  immutable display-text/label cache once per debugger frame before either pane
  is drawn, instead of repeating the same row-count, label-generation, and
  Labels-toggle checks independently in both panes.
- Rebuilding cached disassembly text keeps the same exact primary-label result
  but replaces one binary `PrimaryLabelAt()` lookup per decoded row with one
  initial `lower_bound` followed by a forward walk through the already-sorted
  label database. Full-page refreshes therefore avoid repeated logarithmic
  label searches while label names and formatting remain unchanged.
- The Label Browser keeps Physical addresses live. While the guest is stopped
  and the virtual map has been prepared, a fixed 32-entry frame-local 4 KiB
  page cache lets visible/selected labels on the same page share a translation.
  While the guest is running, every label continues through the historical
  direct-per-address translator so mapping changes cannot be hidden.
- The label translation cache is stack/frame-local, has no persistent
  `MemoryToolsWindow` state, and falls back to direct page translation after the
  fixed cache fills. The selected-label controls reuse the same frame-local
  translator as the table instead of repeating an already-resolved page walk.
- No Cheat Engine/F0/F1, Inject, breakpoint/watchpoint semantics, disassembler
  navigation, Memory Viewer behavior, UI layout, v1.86 Capstone path, v1.87
  navigation-focus fix, v1.88 source split, v1.89 preferences, v1.90 Current
  Game polling, or v1.91 steady-state behavior is intentionally changed.


## v1.93 OPT Pass 8 table/list presentation + allocation cleanup

- Memory Search keeps a small fixed 256-entry direct-mapped presentation cache
  for clipped result rows. Address, Previous, and Current display strings are
  formatted once and reused while the exact result index, address, previous raw
  value, current raw value, and value kind remain identical. Any scan mutation
  or type change therefore self-invalidates without a separate generation or
  persistent search-semantic state.
- The Search display cache is bounded (~tens of KiB), does not scale with the
  potentially multi-million-result scan vector, and does not alter scan/filter
  algorithms or result ordering. Context-menu value text uses the same exact
  cached string that is rendered in the Current column.
- Active F0 bank metadata now prebuilds the existing `name  [hook XXXXXXXX]`
  combo label when the already-established F0 metadata cache rebuilds. The
  debugger consumes that cached label directly instead of formatting preview
  and item strings every frame/open-combo pass. Hook ordering, selection,
  T0-T7/TFLAGS reads, editing, install/deactivate behavior, and cache invalidation
  points are unchanged.
- The breakpoint/watchpoint table and Label Browser were audited but deliberately
  left without new persistent caches: their expensive Physical translation work
  is already frame-local cached by Passes 6-7, and xemu still has no authoritative
  guest page-table generation suitable for persistent translation caching.
- No Cheat Engine/F0/F1 lifecycle, Inject, breakpoint/watchpoint semantics,
  disassembler navigation, Memory Viewer behavior, UI layout, v1.86 Capstone
  path, v1.87 navigation-focus fix, v1.88 source split, v1.89 preferences,
  v1.90 Current Game polling, v1.91 steady-state behavior, or v1.92
  disassembly/label caching is intentionally changed.

## v1.94 OPT Pass 9 audit / pruning / technical-debt cleanup

- Performed a whole `ui/xui/debug-tools/` audit before taking new changes. No
  additional persistent breakpoint/label translation cache was added: the
  existing frame-local stopped-state caches already cover the expensive page
  walks, and xemu still has no authoritative guest page-table generation that
  would make a persistent mapping cache provably safe.
- The Microsoft MAP and PDB label importers now share one private
  `label-symbol-utils.hh` implementation for their previously duplicated C/
  stdcall/import-name cleanup and simple MSVC C++ name formatting. MAP and PDB
  keep their intentionally different compiler-internal filtering rules, so
  accepted/rejected symbol behavior is unchanged.
- Removed the unused exported
  `xemu_breakpoint_condition_register_name()` declaration/definition after a
  tracked-source audit confirmed it had no caller. The condition parser,
  evaluator, register set, operators, editor, and breakpoint semantics are
  unchanged.
- Source-level regression tests now share one generic brace-balanced class
  member extractor in `tests/source_test_utils.py`. Passes 3, 5, 6, 7, and 8 no
  longer carry local copies of the same parser, reducing implementation-specific
  test maintenance without changing their protected digests/invariants.
- Added direct shared-symbol-helper golden cases plus
  `pass9-audit-cleanup-golden.py`. The Pass-9 guard fingerprints the behavior-
  sensitive v1.93 Cheat Engine, F0/F1, Memory Tools, Capstone bridge, debugger
  backend, Current Game, Inject/external-code, label-pack/XDK/XBE, and disc
  paths so this cleanup cannot silently expand into a runtime feature change.
- Matching helper names such as generic endian/range utilities in the XBE,
  XDVDFS, XDK, MAP, and PDB parsers were deliberately left local where their
  data types/ownership differ; centralizing them would add cross-parser coupling
  without a meaningful runtime or maintenance win.
- No Cheat Engine/F0/F1 lifecycle, Inject > NOP/Change/CodeCave, breakpoint/
  watchpoint semantics, disassembler navigation, Memory Viewer behavior, UI
  layout, v1.86 Capstone path, v1.87 navigation-focus fix, v1.88 source split,
  v1.89 preferences, v1.90 Current Game polling, v1.91 steady-state behavior,
  v1.92 display/label caching, or v1.93 presentation caching is intentionally
  changed.

## v1.95 OPT Pass 10 lifecycle / resource / failure-path hardening

- Consolidated the C++ Debug Tools transactional guest pause/resume pattern into
  one private `guest-pause-guard.hh`. Type-F install/deactivate/reclamation,
  current-page RAM dumping, and label dumping now get automatic resume on every
  early return while preserving an already-paused/debugger-stopped guest.
- The label dump explicitly releases the scoped pause at the historical point
  immediately after `fclose()`, before status/UI bookkeeping. Full-range RAM
  dumping is intentionally **not** converted: it continues to leave the VM
  paused after a dump exactly as before.
- Type-F `InstallFHook()` now has one local failed-install cleanup contract for
  the five post-allocation failure branches (preserve allocation, T-register
  allocation, final F0 assembly, layout verification, and external payload
  write). The helper performs the same pre-hook/unreachable resource release as
  the former duplicated branches; hook installation/rollback and retired-cave
  semantics are unchanged.
- `ReleaseFHookCaveIfSafe()` and `DeactivateFHook()` use the same scoped pause
  owner instead of manual `vm_start()` calls on individual success/error exits.
  Nested callers remain safe because a guard only resumes when it observed and
  paused a running VM itself.
- No persistent state, allocator policy, retired-cave timing, mapping cache,
  Inject behavior, breakpoint/watchpoint semantics, navigation, Memory Viewer
  behavior, UI layout, or Current Game detection behavior is changed.
- `pass10-lifecycle-hardening-golden.py` fingerprints all untouched v1.94
  CheatEngine/MemoryTools methods plus the Inject, debugger, Capstone, external
  cave, Current Game/XBE, and debugger-backend files. It separately freezes the
  five-path F0 install cleanup contract and the intentionally leave-paused full
  RAM dump behavior.
