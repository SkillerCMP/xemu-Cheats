# xemu — RAW Cheat Engine / Debugger Build
<p align="center">
  <a href="https://github.com/SkillerCMP/xemu-Cheats/releases">
    <img
      alt="GitHub Downloads - All Releases"
      src="https://img.shields.io/github/downloads/SkillerCMP/xemu-Cheats/total?style=social"
    >
  </a>
  <a href="https://github.com/SkillerCMP/xemu-Cheats/releases/latest">
    <img
      alt="GitHub Downloads - Latest Release"
      src="https://img.shields.io/github/downloads/SkillerCMP/xemu-Cheats/latest/total?style=social"
    >

> **This is a custom development build of xemu.**  
> The original emulator is developed and maintained by the **xemu project**. Please support and follow the upstream project:
>
> 🎮 **Original xemu:** [github.com/xemu-project](https://github.com/xemu-project)  
> 🌐 **Official website:** [xemu.app](https://xemu.app)

This build adds an integrated **RAW Cheat Engine, memory tools, x86 debugger, XBE/disc inspection, and reverse-engineering utilities** while keeping the emulator itself based on xemu.

## 📦 Cheats

➡️ **[Download / View the Cheat Collection](https://drive.google.com/open?id=1RYpwpPkwDTe2awIrRy1tJLyE1HnqSbzk)**

## ✨ Main Additions

- 🧩 **RAW Cheat Engine** with custom Xbox cheat-code types and enable/disable support.
- 🧠 **Memory Search & Viewer** for Virtual and Physical memory.
- 🐞 **x86 Debugger** with disassembly, registers, breakpoints, watchpoints, stepping, and navigation.
- 💉 **Inject Tools** — NOP, Change Instruction, and CodeCave creation directly from the disassembler.
- 🏷️ **XBE Auto Labels** shown in both Virtual and Physical disassembly.
- 💿 **Current Game / Disc Tools** including mounted-disc browsing and full `default.xbe` SHA-256.
- 📤 **RAM / Label Dumps** for external analysis.

<details>
<summary><b>🧩 Cheat Engine</b></summary>

<br>

The Cheat Engine is designed for direct memory editing and more advanced code injection.

Highlights include:

- Standard byte / word / dword memory writes.
- Virtual and Physical addressing support.
- Pointer/base-address code support.
- Fill / multi-byte writes.
- **Type-F CodeCaves** with assembly source and labels.
- F0 assembly and F1 raw/hex cave formats.
- Private **T0–T7 temporary registers** and TFLAGS for F0 code.
- Automatic cave allocation and hook installation.
- Enable / disable handling with restoration of original code where supported.

Example F0 code:

```text
+Example Code
$F0000000 0008C5A2
$mov T0, MyData
$mov T1, [T0]
$cmp eax, T1
$je Success
$jmp Original
$Success:
$mov eax, 1
$Original:
$mov cl, [0046D784]
$DEADCODE
$MyData:
$dd 12345678
```

</details>

<details>
<summary><b>🐞 x86 Debugger</b></summary>

<br>

The integrated debugger includes:

- Virtual and Physical x86 disassembly panes.
- Execute breakpoints.
- Read, Write, and Read/Write watchpoints where supported by the active backend.
- Double-click an instruction to add/remove an Execute breakpoint.
- **Conditional breakpoints** using register expressions such as:

```text
EAX == 12345678
ECX != 00000000
ESI >= 00001000
```

Multiple condition lines are treated as **AND** conditions.

- Step Into / Resume handling around active breakpoints.
- Follow branch destination and return to the previous branch source.
- Current Registers and Last Breakpoint register snapshots.
- Additional register tabs for:
  - x87 / FPU
  - MMX
  - SSE / XMM
- Searchable breakpoint and debugger navigation tools.

</details>

<details>
<summary><b>💉 Disassembler Inject Tools</b></summary>

<br>

Right-click an instruction in the x86 disassembler:

```text
Inject >
    NOP
    Change
    CodeCave
```

### NOP
Replaces the selected instruction with the correct number of `90` NOP bytes.

### Change
Edit a single instruction directly.

- Shows Original and Current HEX / ASM.
- Remembers the **first original instruction** for the current xemu session.
- Supports **REVERT TO ORIGINAL** after reopening the Change window.
- Smaller replacements are padded with NOPs so following instructions do not move.
- Direct branch addresses and labels are supported where valid, for example:

```asm
jmp 0008C5A2
jne MyLabel
call 00123456
loop NearbyLabel
```

### CodeCave
Build and test an F0 code cave from the selected instruction.

- Hook address is generated automatically.
- Original overwritten instructions are copied into the starting template.
- Preview shows the planned JMP / return behavior before running.
- Uses the same Type-F engine as normal cheats.

</details>

<details>
<summary><b>🏷️ XBE Auto Labels</b></summary>

<br>

The debugger can generate labels from the currently mounted game's complete `default.xbe`.

Label types include:

- ENTRY
- SECTION
- KERNEL
- STRING
- XREF
- RTTI
- INFERRED

The XBE **Virtual address is the master label address**. The current Physical address is resolved dynamically from the running game.

Features include:

- Labels displayed in both Virtual and Physical disassembly.
- Labels enable / disable toggle.
- **Current Labels** search/filter window.
- Jump directly to a label in Virtual or Physical disassembly.
- Copy label / address actions.
- **DUMP LABELS** text export with Virtual address, current Physical address, type, and label.
- Unmapped labels are reported as `UNMAPPED` instead of being discarded.
- Inferred labels are marked with `~` so they are not confused with confirmed symbols.

</details>

<details>
<summary><b>🧠 Memory Tools & Dumps</b></summary>

<br>

Memory tools include:

- Virtual / Physical memory viewer.
- First Scan / Next Scan searching.
- Multiple integer and floating-point value formats.
- Address navigation between memory and disassembly.

RAM dumping is split into:

```text
DUMP PHYSICAL
DUMP MAPPED VIRTUAL RAM
DUMP PHYSICAL + DUMP MAPPED VIRTUAL RAM
```

Mapped Virtual dumps also include a `VIRTUAL-MAP.txt` describing the captured regions.

</details>

<details>
<summary><b>💿 Current Game / Disc Tools</b></summary>

<br>

The Current Game window can inspect the game currently mounted in xemu.

Features include:

- Loaded XBE header information.
- Header SHA-256.
- Full mounted-disc **`default.xbe` SHA-256**.
- `default.xbe` disc size, starting sector, and disc offset.
- Read-only **Disc Contents** browser for the mounted Xbox XDVDFS filesystem.
- Refresh support when the mounted disc changes.

</details>

## ⚠️ Development Build

These Cheat/Debug additions are experimental development features and are **not part of the official xemu project**. Behavior can vary by platform, accelerator, game, and build configuration.

If you are looking for the official emulator, documentation, compatibility information, or upstream source code, please use:

➡️ **[https://github.com/xemu-project](https://github.com/xemu-project)**  
➡️ **[https://xemu.app](https://xemu.app)**

---

### ❤️ Credits

- **xemu project** — original Xbox emulator and upstream source.
- Xbox community developers, reverse engineers, cheat creators, and testers who contributed research and code examples used by this custom Cheat/Debug build.
