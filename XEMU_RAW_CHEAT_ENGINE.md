# xemu Built-in RAW Cheat Engine - v0.1.17

This branch adds a built-in ImGui RAW cheat engine under **Debug > Cheat Engine**.
It is an early implementation focused on the address model and
conditional behavior discussed for Xbox/xemu.

## v0.1.17 Cheat-window UI / cursor changes

- Detached Cheat Engine and Memory Viewer windows now own their SDL events; those
  events are no longer also sent through the main Xbox-window ImGui backend.
- Entering/focusing a detached debug window forces a normal visible desktop cursor,
  fixing cases where the game window left SDL's process-global cursor hidden.
- Cheat Engine has a menu bar:
  - **File > Open** opens `<xemu.exe folder>/Cheats`.
  - **File > Reload** reloads the currently matched cheat file.
  - **File > Edit/Add Cheats** opens the current text file in the OS-associated
    editor, creating a current-game header/file first when none exists.
  - **Options** contains Code-Type-Aware D/E, Auto-load Current Game File, and
    Engine Enabled. Auto-load and Engine Enabled default ON.
  - **Help** opens built-in code-type/example information.
- The main cheat pane is reduced to the game/file status, Reload button, a master
  Disabled/Enabled button, and the code tree.
- Live cheat selection defaults **Disabled**. The tree is gray/non-selectable while
  disabled. Switching back to Disabled clears and stops every active cheat. When
  Enabled, clicking a cheat or group checkbox immediately activates/deactivates it.
- Group labels are rendered blue for easier visual separation.


## Memory access

The engine runs inside xemu while the main-loop/BQL lock is held by the HUD
render path.

- **Virtual** access uses `cpu_memory_rw_debug(qemu_get_cpu(0), ...)`, allowing QEMU's
  x86 MMU implementation to translate the full 32-bit guest virtual address.
- **Physical** access uses `address_space_read/write(&address_space_memory, ...)`.

There is no host-process RAM scan and no copied Xbox page-table walker.

## Implemented RAW types

### 0 / 1 / 2 - basic writes

```text
0aaaaaaa 000000vv    8-bit write
1aaaaaaa 0000vvvv    16-bit write
2aaaaaaa vvvvvvvv    32-bit write
```

With no active type-9 context these are **guest virtual addresses**. The address
encoded in the RAW line is used literally; no implicit `0x80000000` base is
added. For example, `10000001 000034EB` writes the 16-bit value `0x34EB` to
guest virtual address `0x00000001`. Inside a type-9 context, `aaaaaaa` becomes
an offset from the current context base and the context selects Virtual/Physical.

### Default address-space model

The top-level/default address space is **Virtual RAM**. Types 0/1/2/3/4/5/6/7
and A therefore operate on guest virtual addresses when they are not wrapped by
a type-9 context. Type 9 keeps its existing meanings and is still available for
explicit virtual/physical bases and pointer-base chains. In practice, mode 1/3
is how a code explicitly switches to physical RAM.

### 9 - scoped address context

```text
9MNNNNNN BBBBBBBB
```

`M`:

```text
0 = Virtual direct base
1 = Physical direct base
2 = Virtual pointer base (read a 32-bit pointer)
3 = Physical pointer base (read a 32-bit pointer)
```

`NNNNNN` is the number of following **physical RAW lines** controlled by this
context. `000000` means until the end of the current cheat block.

At the top level, `BBBBBBBB` is an absolute address/base. When a type 9 is
nested inside another type-9 scope, `BBBBBBBB` is added to the current base.
Pointer modes dereference the resulting address and keep the full 32-bit pointer
as the new base.

Example - full 32-bit virtual base:

```text
90000003 81200000
20000120 000003E7    ; VA 81200120
10000130 00000064    ; VA 81200130
00000140 000000FF    ; VA 81200140
```

Example - three-level virtual pointer walk:

```text
92000001 81200000    ; base1 = Read32(VA 81200000)
92000001 00000120    ; base2 = Read32(VA base1 + 120)
92000001 00000040    ; base3 = Read32(VA base2 + 40)
20000080 000003E7    ; Write32(VA base3 + 80)
```

Because each scope starts after its own type-9 header, the count-1 pattern above
naturally supports stacked pointer levels.

### D - level conditional

```text
Daaaaaaa NNTsvvvv
```

- `NN` = number of following lines/commands guarded
- `T` = compare operator:
  - `0` equal
  - `1` not equal
  - `2` less than
  - `3` greater than
  - `4` `(mem & value) == 0`
  - `5` `(mem & value) != 0`
  - `6` `(mem | value) == 0`
  - `7` `(mem | value) != 0`
- `s`:
  - `0` 16-bit physical
  - `1` 16-bit virtual
  - `2` 8-bit physical
  - `3` 8-bit virtual

When D is inside a type-9 context, the current context supplies the base and
address space; the D address becomes an offset from that base. The size still
comes from `s`.

When the condition is false, D skips its configured amount.

### E - edge-toggle conditional

E uses the same field layout as D. A false-to-true edge toggles an internal
on/off state; guarded lines run while the switch is on. This is useful for
button-press toggles.

## D/E skip modes

The Cheat Options panel contains:

```text
[ ] Code-Type-Aware D/E Skip (Experimental)
```

It defaults **OFF**.

### OFF - normal exact-line behavior

`NN` means exactly `NN` physical RAW lines. A skipped type-9 header does not
activate its scope.

Example: D skips 1 before a type-9 header => only the 9 line is skipped.

### ON - experimental code-aware behavior

`NN` counts logical commands. A type-9 command's logical
span includes the type-9 header plus the raw lines it controls. D/E similarly
own their guarded raw lines when they themselves are skipped as a logical
command.

Example:

```text
D....... 01......
90000003 81200000
20000020 11111111
20000024 22222222
20000028 33333333
```

A failed D with `NN=1` skips all four physical RAW lines in code-aware mode.

## Source format

The editor accepts normal pairs and CMP-style `$` prefixes:

```text
20123456 DEADBEEF
$20123456 DEADBEEF
```

`+Name` starts a selectable cheat block. CMP-style `!Group:` / `!!` nesting,
`%Credits:`, `{description}`, blank lines, and semicolon-prefixed comments are
supported by the file parser.

Cheat files are edited externally from **File > Edit/Add Cheats**. If no matching
file exists for the current game, xemu creates one with the current Hash/GameID/
NAME header in the `Cheats` folder beside `xemu.exe`. **File > Reload** reparses
the current text file.

## GitHub Actions / Windows source extraction note

Some Windows ZIP extraction/recommit workflows can strip Unix executable bits from shell scripts.
The bundled workflows invoke `scripts/archive-source.sh` and `build.sh` explicitly through `bash`,
so GitHub Actions does not depend on those files retaining mode `+x`.

## Added RAW types 3-7

### Type 3 - arithmetic

```text
300000VV 0AAAAAAA    8-bit increment
301000VV 0AAAAAAA    8-bit decrement
3020VVVV 0AAAAAAA    16-bit increment
3030VVVV 0AAAAAAA    16-bit decrement
30400000 0AAAAAAA    32-bit increment
VVVVVVVV 00000000
30500000 0AAAAAAA    32-bit decrement
VVVVVVVV 00000000
```

### Type 4 - 32-bit serial write

```text
4AAAAAAA NNNNSSSS
VVVVVVVV IIIIIIII
```

Writes `NNNN` 32-bit values. Address advances by `SSSS * 4`; value advances by `IIIIIIII`.

### Type 5 - byte copy

```text
5AAAAAAA NNNNNNNN
DDDDDDDD 00000000
```

Copies `NNNNNNNN` bytes from A to D. A 16 MiB per-command safety limit is used to prevent accidental enormous allocations.

### Type 6 - pointer write

```text
6AAAAAAA VVVVVVVV
000TNNNN OOOOOOOO
OOOOOOOO OOOOOOOO ...
```

`T=0/1/2` writes 8/16/32 bits. `NNNN` is the offset count. The first offset is the second word of the descriptor line and further offsets are packed two per line. When wrapped by a type-9 context, the base and every pointer dereference use that context's physical/virtual address space. Full 32-bit Xbox pointers are preserved; the PS2 `0x3FFFFFFC` pointer mask is intentionally not used.

### Type 7 - bitwise

```text
7AAAAAAA 00T0VVVV
```

`T=0/1` byte/halfword OR, `T=2/3` byte/halfword AND, `T=4/5` byte/halfword XOR.

### D/E skip interaction

With **Code-Type-Aware D/E Skip OFF**, D/E counts literal RAW lines exactly as before. With it ON, type 3 word arithmetic, types 4/5, variable-length type 6, and xemu type A report their complete logical span so a logical skip does not land in continuation data.

### Type A - xemu raw-byte fill/write

```text
AXXXXXXX ZZZZZZZZ
DDDDDDDD DDDDDDDD
```

This is an **xemu-specific** replacement for the original CodeBreaker type-A setup meaning.

- `XXXXXXX` = destination virtual address by default, or offset from the current type-9 context.
- `ZZZZZZZZ` = total number of bytes to write.
- Each continuation RAW line supplies exactly eight literal bytes, so the engine automatically consumes `ceil(ZZZZZZZZ / 8)` continuation lines.
- Continuation lines are treated as one literal byte stream in the exact left-to-right hexadecimal order shown. They are **not** endian-swapped as 32-bit values.
- On the final continuation line, bytes beyond the requested total are ignored.

Example:

```text
A006C320 00000006
0F85A900 00000000
```

writes these six consecutive bytes at `0x006C320`:

```text
0F 85 A9 00 00 00
```

A longer example writes 12 bytes and therefore consumes two continuation lines:

```text
A006C320 0000000C
0F85A900 12345678
90ABCDEF 11223344
```

The resulting byte stream is `0F 85 A9 00 12 34 56 78 90 AB CD EF`; the unused trailing `11 22 33 44` is ignored.

Inside any type-9 context the same `XXXXXXX` field is an offset from that context base; mode 0/2 uses virtual memory and mode 1/3 uses physical memory.

In normal D/E exact-line mode, type A occupies one header line plus however many physical continuation lines are required by the byte count. In experimental code-aware mode, the complete A header plus all of its continuation lines are treated as one logical command.

Types `8`, `B`, and `C` remain intentionally unsupported because their CodeBreaker meanings are setup/master-engine controls rather than ordinary Xbox memory operations. Type `F` is repurposed by this Windows-only v0.1.42 backport for automatic external x86 code caves; see the Type-F section at the end of this document.

## v0.1.6 C/C++ QEMU bridge fix

The ImGui cheat-engine frontend is C++, but several QEMU CPU/memory headers use
GNU C-only constructs. The frontend no longer includes those headers directly.
Guest CPU/memory access is isolated in `ui/xui/cheat-engine-memory.c`, compiled
as C, with a small plain-C API declared in `cheat-engine-memory.h`.

This avoids MinGW/GCC C++ failures involving `typeof_strip_qual`, GNU `typeof`,
reserved identifiers such as `class`, and QEMU atomic/RCU macro expansion.

## v0.1.7 Memory Viewer / Search

A new **Debug > Memory Viewer / Search** window provides three tabs:

- **Physical Memory** - reads/writes the Xbox physical address space directly.
- **Virtual Memory** - reads/writes through the Xbox CPU virtual address translation.
- **Search / Compare** - scans either physical or virtual memory and refines results.

### Memory viewers

Each viewer shows 0x100 bytes at a time as 16-byte hex/ASCII rows, supports a
hexadecimal **Go To** address, `-100` / `+100` paging, and direct 8/16/32-bit
read/write controls.

### Search / Compare

Supported value types:

- UInt8 / UInt16 / UInt32
- Int8 / Int16 / Int32
- Float32

First scans can use **Exact Value** or **Unknown Initial**. Unknown Initial
captures a snapshot of the chosen range and waits for a subsequent comparison.
Next-scan comparisons include:

- Exact Value
- Not Equal Value
- Changed / Unchanged
- Increased / Decreased
- Greater Than Value / Less Than Value

Scans can use aligned values or byte-by-byte stepping. The range is user
selectable, with quick presets for 64 MB physical RAM, 128 MB physical RAM,
and the `0x80000000-0x83FFFFFF` virtual window. A single scan is capped at
256 MB as a safety guard against accidentally snapshotting most of the 4 GB
virtual address space.

Double-clicking a result sends its address to the matching Physical or Virtual
viewer and selects it for direct reading/writing.

## Current Game Manager (v0.1.8)

`Debug -> Current Game` tracks the XBE currently loaded by xemu and exposes a
shared game identity to the Cheat Engine and Memory Tools.

Tracked fields:

- XBE title name
- 32-bit Title ID
- region flags
- disc number
- XBE version
- XBE base and image size
- PE checksum and timestamp
- SHA-256 of the XBE header block
- a compact revision key: `TITLEID-<first 16 SHA256 hex chars>`

The manager refreshes twice per second and increments an internal generation
counter whenever the Title ID/revision identity changes. The header hash is
computed over xemu's copied XBE header block rather than writable game RAM so
normal gameplay does not change the revision identity.

## CMP-style game files and tree UI (v0.1.9)

The Cheat Engine auto-loads per-game `.txt` files from a `Cheats` folder
beside the running xemu executable. For example:

```text
C:\Emulators\xemu\xemu.exe
C:\Emulators\xemu\Cheats\
```

The folder is resolved from the executable itself, not from the process working
directory and not from xemu's AppData/settings path. If it does not exist, xemu
creates it when current-game cheat loading is attempted.

The filename is for readability. A recommended filename is:

```text
EA009E-<first-16-XBE-SHA256>.txt
```

The file header is authoritative when matching a running XBE:

```text
^1 = Hash: 7A91C23F571082A4
^2 = GameID: EA009E
^3 = NAME: Need for Speed: Carbon
```

`GameID` accepts either the compact CMP-style publisher form or the normal
8-hex-digit Xbox Title ID. These are treated as identical:

```text
^2 = GameID: EA009E
^2 = GameID: 4541009E
```

The hash may be the complete XBE-header SHA-256 or a hexadecimal prefix of it.
Longer matching hashes are preferred if multiple files match the same title.

### Groups

CMP-style groups and nested groups are parsed into the visible cheat tree:

```text
!Player Codes:

+Infinite Health{Keeps player health at maximum.}
%Credits: Skiller
$20012340 00000064

!Money Codes:
+Infinite Money
%Credits: Skiller>Another Author
$20012344 3B9AC9FF
!!

!!
```

`!Name:` opens a group and `!!` closes one group level. Group checkboxes select
or clear every cheat beneath that group, including nested groups.

### Live selection / master Disabled-Enabled button

The cheat list starts **Disabled**. While disabled, the entire tree is gray and
non-selectable, and no cheat block is active. Press the master **Disabled**
button to switch it to **Enabled**. Once enabled, checking or unchecking an
individual cheat immediately activates/deactivates that block. Group checkboxes
apply the same live state recursively to all cheats beneath the group.

Pressing **Enabled** again switches the master state back to **Disabled**, clears
all checkmarks, stops every active cheat, and resets edge-toggle state. The
separate **Options > Engine Enabled** switch defaults ON and controls whether the
runtime engine executes at all.

When the Current Game Manager detects a different XBE, active codes are stopped
and the executable-root `Cheats` folder is searched for a file whose `^1 Hash` and `^2 GameID`
match the new game. The previous game's codes are never left active across a
game change.


## Full RAM dump (v0.1.14)

`Debug -> Memory Viewer / Search -> Dump RAM` now creates one complete physical
RAM image plus a page-table-driven dump of every contiguous guest virtual
region that is actually backed by installed Xbox RAM. Pressing **DUMP PHYSICAL
+ MAPPED VIRTUAL RAM (Pause Game)** stops the guest first and leaves it paused
after dumping so every output file represents the same stopped state.

The physical dump follows the Xbox machine's configured RAM size (normally
64 MB, with larger developer/debug configurations handled automatically):

- Physical dump: `0x00000000` through `RAM_SIZE - 1`.
- Virtual scan: the complete 32-bit guest range `0x00000000-0xFFFFFFFF`.

For the virtual side, xemu synchronizes the CPU state and uses the same QEMU
virtual-to-physical page translation mechanism used by xemu's XBE reader. Each
4 KB virtual page is tested. Pages are included only when they translate to a
physical address inside installed Xbox RAM; MMIO/device mappings are excluded.
Adjacent included virtual pages are merged into one region file. Physical pages
inside a virtual region do not need to be physically contiguous. Aliases are
preserved, so the same physical RAM may legitimately appear in more than one
virtual region.

Files are written beside the running xemu executable in `Ram-Dumps/` (for
example `C:\Emulators\xemu\Ram-Dumps\` when `xemu.exe` is in
`C:\Emulators\xemu\`). The path is resolved from the executable itself,
not the process working directory or the user's AppData/settings path.

A dump set contains:

```text
EA009E-7A91C23F571082A4-20260809-142400-PHYSICAL-00000000.bin
EA009E-7A91C23F571082A4-20260809-142400-VIRTUAL-MAP.txt
EA009E-7A91C23F571082A4-20260809-142400-VIRTUAL-00010000-0038FFFF.bin
EA009E-7A91C23F571082A4-20260809-142400-VIRTUAL-80000000-83FFFFFF.bin
...
```

The exact virtual filenames depend on the mappings present in the running game.
`VIRTUAL-MAP.txt` records every discovered region, its virtual start/end, its
size, and the corresponding `.bin` filename. File offset zero in each virtual
region file corresponds to that region's virtual start address. For example,
file offset `0x1234` in a region beginning at `0x00600000` represents guest
virtual address `0x00601234`.



## Detached desktop tool windows (v0.1.16)

**Debug > Cheat Engine** and **Debug > Memory Viewer / Search** now open as
independent desktop windows instead of drawing over the Xbox framebuffer.
They remain part of the same xemu process and continue using the same cheat,
current-game, memory, search, and RAM-dump state.

The two windows can be moved, resized, minimized, maximized, or placed on
another monitor independently of the main xemu game window. Closing a debug
tool window hides it only; selecting the corresponding Debug menu item opens
that same tool/state again.

The detached windows use their own SDL/OpenGL/ImGui rendering contexts. The
main xemu renderer is restored after each debug-tool frame, and the SDL event
watch now filters expose/resize events by the actual console window so resizing
a detached debugger window is not treated as a game-window resize.

## Memory editor and x86 debugger (v0.1.18)

The detached **Memory Viewer / Search** window now includes an **x86 Debugger**
tab. Disassembly uses QEMU's target-aware monitor disassembler against guest
virtual memory, so addresses are literal Xbox virtual addresses (for example
`006C32C` means virtual `0x006C32C`; no implicit `0x80000000` is added).

The debugger includes execute breakpoints, Pause/Resume, Go to EIP, optional
Follow EIP, a register panel, and a retained register snapshot from the last
execute breakpoint hit. The register display includes the 32-bit GPRs, EIP,
linear PC, EFLAGS, segment selectors, and CR0/CR2/CR3/CR4. Execute breakpoints
use QEMU CPU breakpoints and are compatible with xemu's WHPX execute-breakpoint
implementation as well as TCG's BP_GDB handling.

The old **Read / Write Value** panel was removed. Physical and Virtual memory
views now have **Enable Memory Writing**. When enabled, each byte in the hex
grid becomes editable; press Enter in a byte cell to write `00-FF` directly to
that address. Memory writing starts disabled.

The Search / Compare **First Scan** mode selector and **First Scan** button now
use different ImGui IDs, fixing the duplicate-ID assertion that appeared when
hovering the control.

Read/write watchpoints are not enabled in this revision. xemu's WHPX backend
has explicit support for QEMU execute breakpoints but does not currently expose
a corresponding generic CPU watchpoint path like TCG does.

## x86 debugger target-header build fix (v0.1.19)

The debugger bridge no longer includes `target/i386/cpu.h`. QEMU intentionally
poisons target-only macros such as `TARGET_X86_64` in common compilation units,
which caused the Windows/MXE build to fail when the v0.1.18 bridge included the
i386 target header directly.

Register capture now goes through QEMU's generic GDB register interface. The
per-target i386 gdbstub remains responsible for reading the actual architectural
register state, while the xui bridge only handles plain register names and
32-bit values. This keeps the C/C++ separation intact and avoids leaking target
headers into the common UI build.

## v0.1.21 - x86 disassembler backend fix

The Windows Release workflow now supplies the Capstone x86 decoder explicitly. The detached x86 Debugger disassembles literal guest virtual addresses in Intel syntax and reports unmapped addresses separately from a missing decoder backend.

## v0.1.22 - Capstone MXE archive-tool build fix

The Windows x86_64 Release workflow now resolves the MXE compiler/binutils with `command -v` and passes their absolute paths to Capstone's CMake build. The standalone Capstone build uses the normal MinGW `ar`/`ranlib` pair instead of hard-coding `gcc-ar`/`gcc-ranlib`, preventing CMake from looking for `x86_64-w64-mingw32.static-gcc-ar` under the GitHub workspace. Runtime debugger behavior is unchanged from v0.1.21.

## v0.1.23 - Capstone Meson Dependency Fix

- Fixed the Windows build failure where `CONFIG_CAPSTONE` was enabled but
  `ui/xui/cheat-engine-memory.c` could not resolve `<capstone.h>`.
- The debugger bridge now explicitly inherits Meson's `capstone` dependency,
  which supplies the pkg-config include path and linker flags.
- The Windows workflow now compiles a tiny `<capstone.h>` smoke-test object with
  the MXE cross compiler before configuring xemu.

## Memory Map / Virtual Alias Explorer (v0.1.24)

The detached **Memory Viewer / Search** window now includes a **Memory Map**
tab. It walks the complete 32-bit Xbox guest virtual address space in 4 KB
pages using QEMU's existing debug page translation and keeps only mappings
backed by installed Xbox physical RAM. MMIO/device mappings are excluded.

The map can:

- translate a literal guest virtual address to its current physical RAM address;
- take a physical RAM address and list every guest virtual alias that maps to
  the same physical byte;
- double-click an alias to jump directly to it in the Virtual Memory viewer;
- jump a resolved physical address directly into the Physical Memory viewer;
- display linear mapping regions, merging pages only when both the virtual and
  physical sides remain contiguous;
- show mapped virtual-page count, unique physical-page count, and alias-page
  count for the current snapshot.

The map is a snapshot of the current CPU page tables. It is invalidated when
Current Game Manager detects a different title/revision; press **Refresh Memory
Map** after a game/major executable transition to rebuild it.

Example:

```
Virtual  006C32C -> Physical 021A732C

Physical 021A732C aliases:
  006C32C
  806C32C
  A06C32C
```

Multiple virtual addresses may legitimately resolve to the same physical RAM
page. The reverse alias list is generated by walking all RAM-backed virtual
page mappings; it is not assumed from a fixed 0x80000000 mirror.

## Unified Physical / Memory Map / Virtual Workspace (v0.1.25)

The detached Memory Viewer now combines the previous Physical Memory, Memory
Map, and Virtual Memory tabs into one side-by-side workspace:

```
Physical RAM | Memory Map | Virtual RAM
```

The Physical pane spans the machine's complete installed RAM (64 MB, 128 MB,
or another configured size). The center pane contains the current RAM-backed
virtual-to-physical mappings. Clicking a mapping selects that virtual alias and
positions both memory panes at the corresponding start addresses.

The two memory panes stay synchronized through the selected map. Virtual
navigation always resolves to Physical. Physical navigation keeps the selected
virtual alias while it covers the physical address, then chooses another
current mapping if necessary.

Memory writing is disabled by default. When enabled, a byte can be edited by
clicking its hex cell and typing two hexadecimal digits. The second digit
commits immediately; Enter is not required.

The Cheat Engine Help popup now uses fixed line breaks and indented field
explanations for code formats. Type 9 is shown as:

```
9MCCCCCC  BBBBBBBB
M = Type
    0 = Virtual Base
    1 = Physical Base
    2 = Virtual Pointer Base
    3 = Physical Pointer Base
C = Count
    000000 = Continue Until End of Cheat
B = Offset / Base Address
```


## Linked Compact Memory Grid (v0.1.26)

The unified Memory tab links exact byte selections between Physical and Virtual RAM.
Clicking a byte on either side highlights the translated byte/row on the other side
using the currently selected Memory Map alias. The hex grids use compact fixed-width
columns (`00000000 | 00 | 00 | ...`). Memory Writing uses the selected byte directly:
click a cell and type two hex digits; the second digit writes immediately and advances
to the next byte.

## Memory Navigation / Cross-Highlight Fix (v0.1.27)

Virtual **Go** now searches the complete current RAM-backed virtual map rather
than only the mapping currently selected in the center pane. For example, if
`00579000` maps to physical `0087C000`, entering `00579000` selects that map
region and synchronizes the Physical pane to `0087C000`.

Cross-highlighting now preserves the opposite pane's viewport. If the paired
physical/virtual byte is already visible, only its byte/row highlight changes;
the row stays at its existing screen position. The paired pane scrolls only
when the target address is outside its visible range or when synchronization
requires switching to another mapped virtual region.

## Linked Virtual / Physical x86 Debugger + Clean Continue (v0.1.28)

The x86 Debugger now presents the currently decoded instruction stream in two
linked panes: **Virtual x86 (CPU / EIP)** and **Physical x86 (backing RAM)**.
Each decoded virtual instruction is translated independently through the
current Xbox page tables, so the Physical pane remains correct even when two
consecutive virtual pages are backed by non-contiguous physical pages. Clicking
a row in either pane selects the same instruction in both panes, and their
vertical scrolling is linked.

Execute breakpoints remain Virtual addresses because the x86 CPU executes the
virtual/linear PC. The breakpoint list now also shows each breakpoint's live
Physical translation. When a breakpoint fires, the debugger snapshots both the
Virtual PC and its Physical backing address at the instant of the hit.

**Resume** from an execute breakpoint now performs an invisible one-instruction
single-step before continuing. This prevents TCG from immediately re-triggering
the same execute breakpoint at the unchanged PC, while remaining compatible
with WHPX's own breakpoint step-over path. **Step Into** exposes the same
single-step mechanism to the user and Follow EIP refreshes both disassembly
panes after each completed step or breakpoint hit.

The Virtual-to-Physical helper now reports the exact translated byte address,
including the offset within the 4 KB page. For example, if `00579000` maps to
`0087C000`, then `00579003` is reported as `0087C003`.

## TARGET_PAGE_MASK Compile Fix (v0.1.29)

v0.1.28 added exact byte-offset Virtual-to-Physical translation and referenced
`TARGET_PAGE_MASK` from `ui/xui/cheat-engine-memory.c`. That C translation unit
did not directly include the QEMU header that defines the macro, causing the
Windows/MXE build to fail with `TARGET_PAGE_MASK undeclared`.

v0.1.29 directly includes `exec/target_page.h`. No runtime behavior was changed.

## Memory Writing Input Fix (v0.1.30)

The compact Memory workspace no longer polls Dear ImGui's raw
`InputQueueCharacters` after rendering the window. The selected byte becomes a
compact in-cell `InputText` only while Memory Writing is enabled. Its existing
2-digit value is automatically selected; typing two hexadecimal digits writes
immediately and advances to the next byte. No Enter key or separate value field
is required. The actual Physical and Virtual write backends are unchanged.


## Native Breakpoint Resume Cleanup (v0.1.31)

The debugger continues to use QEMU/xemu CPU execute breakpoints as guest
Virtual addresses and keeps the linked Virtual/Physical disassembly and
breakpoint translation display from v0.1.28. Resume behavior is now selected
by the active accelerator instead of applying the same custom step-over path
to every backend.

- **WHPX:** Resume calls normal VM Continue. The existing WHPX backend detects
  that PC is on an active CPU breakpoint, temporarily restores the original
  instruction, single-steps it exclusively, reinstalls the breakpoint, and
  continues. The Memory Tools window no longer adds a second single-step on
  top of WHPX.
- **TCG:** Resume from an execute breakpoint retains the one-instruction
  single-step helper because TCG checks a CPU breakpoint before executing the
  instruction at the unchanged PC. TCG's own single-step path intentionally
  overrides breakpoints for that instruction, allowing forward progress.
- **Step Into:** remains an explicit architectural single-step on either
  backend.

The x86 Debugger status line displays the active breakpoint backend so test
runs make it clear whether native WHPX or TCG behavior is being used.

## Safe Breakpoint Mutation Fix (v0.1.32)

Execute breakpoint add/remove operations are now serialized against a running
vCPU. The detached debugger UI must not modify `CPUState::breakpoints` while
WHPX is concurrently walking that queue to build its low-level breakpoint
collection.

When the guest is running, breakpoint changes now use this sequence:

```text
Pause all vCPUs
  -> modify QEMU CPU breakpoint list
  -> resume guest
```

When the guest is already paused or stopped by the debugger, the breakpoint
list is changed in place and the guest remains paused. This provides WHPX with
a clean stop/start boundary to translate CPU breakpoints into its native INT1
(0xF1) implementation.

## Source Layout (v0.1.33+)

Custom xemu tooling is isolated under:

```text
ui/xui/debug-tools/
```

The folder owns the RAW Cheat Engine, Current Game manager, detached tool
windows, Physical/Map/Virtual memory workspace, RAM search/dump UI, x86
debugger UI, and the C bridge into QEMU/xemu memory/debug facilities.

The normal xemu source tree only keeps small integration hooks in
`ui/xui/main.cc`, `ui/xui/menubar.cc`, `ui/xui/meson.build`, and the detached
window event filter in `ui/xemu.c`. This separation is intentional so future
upstream xemu updates can be rebased without mixing the custom implementation
throughout the normal XUI files.


## Josh Native Breakpoint Backend Integration (v0.1.34)

The cleaned `debug-tools` layout now contains a target-specific debugger
backend under:

```text
ui/xui/debug-tools/backend/
    xemu-dbg.c
    xemu-dbg.h
```

This follows the architecture in Josh's xemu debugger fork. The C++ Memory
Tools window does not manipulate QEMU breakpoint queues itself. It calls a
plain-C bridge, which delegates Execute and data breakpoint operations to the
target-specific backend.

Execute breakpoints use QEMU's normal `BP_GDB` CPU breakpoint representation:

```text
Virtual address
    -> cpu_breakpoint_insert(... BP_GDB ...)
    -> QEMU CPU breakpoint list
    -> TCG native debugger handling
       or WHPX's existing CPU-breakpoint translation
```

The custom v0.1.32 pause/mutate/resume wrapper is no longer used for Execute
breakpoint insertion/removal. This intentionally tests the same direct native
breakpoint path used by Josh's debugger implementation.

The x86 Debugger exposes data watchpoints under both TCG and WHPX:

```text
Read
Write
Read / Write
Length: 1+ bytes
```

TCG consumes QEMU's `CPUState::watchpoints` directly. Under WHPX, v0.1.38
mirrors those same watchpoints into the x86 hardware debug registers DR0-DR3.
Write and Read/Write use native DR7 data conditions. Read-only uses a paired
Read/Write + Write slot and filters the write case by inspecting DR6. WHPX has
four hardware slots total, so large/unaligned ranges or multiple Read-only
watchpoints can exhaust the available slots.

A watchpoint hit reports both address spaces for the watched data and follows
the x86 instruction that performed the access. For example:

```text
WRITE watchpoint hit:
  data        V 00579000 -> P 0087C000
  instruction V 00123456 -> P 012AB456
```


## v0.1.39 - Full-page breakpoint disassembly

The x86 debugger can now display the complete 4 KiB virtual page containing the
selected address. `Full Page` is the default view, while the previous `Count` mode
remains available. Breakpoint/watchpoint hits with Follow EIP enabled automatically
open the full page and position the stop below the middle of the pane so earlier
instructions remain visible. The architectural debugger stop row is highlighted in
light blue in both the Virtual and Physical panes. For x86 hardware data watchpoints,
this stop EIP is normally the instruction immediately after the memory access because
architectural #DB is reported after the access completes.


## v0.1.40 - Actual watchpoint-access highlight

For Execute breakpoints, the persistent light-blue row remains the instruction
that owns the execute breakpoint. For Read/Write data watchpoints, x86 reports
#DB after the memory-access instruction has completed, so the debugger now
resolves the decoded instruction whose end address equals the architectural
stop EIP and highlights that access instruction instead.

Example:

```text
0009D411  mov [eax+58],ecx   <- LIGHT BLUE: actual WRITE
0009D414  mov eax,ecx        <- Current EIP after #DB
```

The status line reports the watched data address, resolved access-instruction
address, and current EIP separately. If an instruction cannot be resolved
cleanly, the UI falls back to the architectural stop EIP rather than guessing.
All v0.1.40 implementation changes remain inside `ui/xui/debug-tools/`.


## Windows-only v0.1.42 backport - finalized Type F external x86 caves

This backport keeps the v0.1.42 Windows-only source/build layout and adds only the
finalized Type-F runtime design plus the later WHPX one-shot Step Into behavior.

Type F executes real guest x86 from xemu-owned RAM outside the Xbox
`machine->ram_size`. The private pages are mapped into a 1 MiB guest virtual arena
beginning at `0x68000000`; cheat authors do not choose or need to know the cave
address. Caves are allocated automatically on 16-byte boundaries.

A leading `$` is optional on every Type-F line.

### F0 - assembly cave (recommended)

```text
$F0000000 AAAAAAAA
$<32-bit Intel-syntax x86 instructions and labels>
$DEADCODE
```

`A` is the virtual hook address, or an offset under an active Virtual Type-9
context. The low 24 bits in `F0000000` are reserved and must be zero.

Example:

```text
$F0000000 0009D411
$mov dword ptr [eax+58],054C5638
$mov eax,ecx
$DEADCODE
```

Bare F0 numeric literals are hexadecimal. Local labels and relative `JMP`, `CALL`,
and Jcc branches are assembled automatically.

### F0 static data and preservation (v0.1.62)

F0 can attach 32-bit static data to the same cave allocation with `DD`. A label
resolves to the final guest virtual address automatically:

```text
$F0000000 0008C5A2
$PRESERVE ECX, EDX
$mov edx,CarList
$mov ecx,6
$CheckCar:
$cmp eax,dword ptr [edx]
$je Success
$add edx,4
$loop CheckCar
$RESTORE
$jmp Original
$Success:
$RESTORE
$mov eax,1
$mov ecx,[ebp+8]
$mov [ecx],eax
$pop esi
$pop ebp
$ret 8
$Original:
$mov cl,[0046D784]
$DEADCODE

$CarList:
$dd 01D28710, 8B80C5FC, E3BDE8CB
$dd 0001308E, 60D6890E, 0689441B
```

`DD` values are emitted as ordinary little-endian dwords. DD declarations may
appear after `DEADCODE` for readability; xemu still owns them as part of that F0
block and lays the cave out as `code | generated return JMP | DD data`. The cave
allocator sizes the whole layout before installing the hook, so another cave can
never overlap an attached list. If an edited cave grows, only that cave is moved;
other active caves never shift.

F0 also accepts assembler directives for temporary register preservation:

```text
$PRESERVEALL
$PRESERVE EAX, ECX, EDX
$RESTORE
$RESTORE ECX, EDX
```

For cheat-file familiarity, `PRESERVE, EAX` and `RESTORE, EAX` are accepted as
alias spellings for the same selective directives.

`PRESERVEALL` saves EAX, EBX, ECX, EDX, ESI, EDI, EBP, and EFLAGS. ESP and EIP
are intentionally excluded. Adjacent PRESERVE lines accumulate into one frame.
RESTORE with no list restores everything still saved in the top frame; selective
RESTORE restores/clears only the named registers. RESTORE with no active frame is
a no-op.

The existing 1 MiB xemu-owned mapping is split internally:

```text
68000000-680EFFFF   F0/F1 executable code + DD data (960 KiB)
680F0000-680FFFFF   private PRESERVE storage       (64 KiB)
```

Each F0 hook using preservation receives a private block with a depth counter and
16 nested frames. Restored slots are zeroed, fully restored frames are cleared and
reused, and the whole preservation block is zeroed/freed when the owning cave is
disabled and safe to reclaim. This storage remains outside Xbox machine RAM.

### F0 assembler coverage (v0.1.61)

The built-in assembler now accepts the normal 32-bit Xbox general-purpose
registers plus 16-bit (`AX..DI`) and legacy 8-bit (`AL..BH`) register forms.
Common supported families include:

- `MOV`, `MOVZX`, `MOVSX`, `LEA`
- `ADD`, `ADC`, `SUB`, `SBB`, `AND`, `OR`, `XOR`, `CMP`, `TEST`
- `INC`, `DEC`, `NEG`, `NOT`
- `IMUL`, `MUL`, `DIV`, `IDIV`
- `SHL/SAL`, `SHR`, `SAR`, `ROL`, `ROR`, `SHLD`, `SHRD`
- `PUSH`, `POP`, `PUSHAD/POPAD`, `PUSHFD/POPFD`
- `JMP`, `CALL`, Jcc, `LOOP/LOOPE/LOOPNE`, `JECXZ`, `RET`, `RET imm16`
- `SETcc`, `CMOVcc`
- `XCHG`, `XADD`, `CMPXCHG`, `BSWAP`, `BSF`, `BSR`, `BT/BTS/BTR/BTC`
- `LAHF`, `SAHF`, `CLC`, `STC`, `CMC`, `CLD`, `STD`, `CBW`, `CWDE`, `CWD`,
  `CDQ`, `LEAVE`, `INT3`, `IRET`
- `MOVS*`, `STOS*`, `LODS*`, `SCAS*`, `CMPS*` with `REP/REPE/REPNE`

F0 is still deliberately a compact cheat-code assembler rather than a full
general-purpose x86 toolchain. FPU/MMX/SSE/SSE2 or another unsupported form can
still be supplied through F1 raw bytes.

The previous DLC example that required raw F1 can now be written directly as:

```text
$F0000000 0008C5A2
$cmp eax,0689441B
$jne Original
$mov eax,00000001
$mov ecx,dword ptr [ebp+08]
$mov dword ptr [ecx],eax
$pop esi
$pop ebp
$ret 0008
$Original:
$mov cl,byte ptr [0046D784]
$DEADCODE
```

### F1 - aligned raw hexadecimal cave

```text
$F1000000 AAAAAAAA
$XXXXXXXX YYYYYYYY
$XXXXXXXX YYYYYYYY
$DEADCODE 000000NN
```

Every F1 payload line contains exactly eight literal bytes as two 32-bit words.
`NN` must be `01` through `08` and gives the number of meaningful bytes in the
final payload line. Remaining bytes in that final line must be `00` padding and
are not copied to executable memory. The generated return JMP is placed directly
after the final meaningful byte.

For both F0 and F1, xemu uses Capstone at the hook address to span the smallest
number of whole instructions totaling at least five bytes (maximum 32), saves those
bytes, installs a 5-byte `JMP rel32` to the allocated cave, NOP-fills any extra
overwrite bytes, and appends a generated JMP back at `DEADCODE`. Disabling the cave
restores the original hook bytes. Overwritten instructions are not automatically
relocated; reproduce any required original behavior inside the cave.

### Type-F reactivation lifecycle fix

- Deactivated F0/F1 hooks retain their original-byte snapshot across cheat-file reloads for the current XBE.
- Editing a cave value/instruction and reactivating at the same hook reuses the existing external allocation when it fits.
- Larger edited caves allocate a new external slot without losing the original hook bytes.
- Mutually-exclusive Type-F cheats targeting the same hook can hand off the inactive hook state cleanly.
- Hook-install failures now report the exact failed stage instead of only `Type-F Hook Installation Failed`.



## v0.1.60 Windows-only compile fix

The v0.1.59 Windows-only backport exposed `XemuCheatX86Registers` in a private
`CheatEngineWindow` method declaration without declaring that type in
`cheat-engine.hh`.  The type already exists in `cheat-engine-memory.h`; v0.1.60
adds a C++ forward declaration in `cheat-engine.hh`.  This is compile-only and
does not change runtime debugger or Type-F behavior.

## v0.1.59 x86 debugger navigation and live register editing

The Windows-only v0.1.42 backport now includes browser-style control-flow navigation in the x86
Debugger. Select a JMP/Jcc/CALL/RET instruction and press Right to follow its target; Shift+Right
follows fall-through, Left goes Back, and Alt+Right goes Forward. These commands only move the
disassembly view and never execute the Xbox CPU. The x86 context menu exposes the same actions under
`Follow` and `Navigation`. Direct branch/call targets are recognized from disassembly text; while paused,
register-indirect targets, common x86 memory-indirect expressions, and RET targets are resolved from
the live Current Registers/RAM when possible.

Direct JMP/Jcc links are drawn in a small gutter beside the disassembly. Backward/negative links
are baby blue and forward/positive links are yellow. The existing double-click behavior remains
`Break on Exe`.

The left `Current Registers` pane is live and editable whenever the guest is paused or stopped by the
debugger. Double-click a value or right-click it and choose `Edit Register Value`; register writes use
QEMU's GDB register bridge so WHPX receives the dirty architectural state before Resume/Step. The same
right-click menu can copy the register/value or view the value in Memory/x86 Debugger. `PC` remains a
derived read-only display; edit `EIP` to change execution position. `Registers at Last Breakpoint`
remains a read-only historical snapshot.

## v0.1.63 Type-F PDE accessed-bit allocation fix

- Fixes repeated F0/F1 activation/allocation failures after the guest has executed the 0x68000000 Type-F mapping.
- The IA-32 CPU may set the page-directory Accessed bit; Type-F now validates the PDE page-table base and required mapping flags instead of requiring a byte-for-byte pristine PDE value.
- Allocation errors now report whether the guest mapping is unavailable/conflicting versus the 960 KiB code/DD arena actually being exhausted or fragmented.
