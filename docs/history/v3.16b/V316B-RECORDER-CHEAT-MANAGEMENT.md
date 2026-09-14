# XEMU Cheats v3.16b — recorder text and cheat management

Source/build test candidate based on the exact v3.16a package. No precompiled
XEMU executable is included. The Windows v3.16a memalign include is retained.

## Changes

### Recorder code panel
The recorded active-code panel now rasterizes the existing embedded Roboto
Condensed resource into a private CPU atlas, caches its grayscale glyph coverage,
and alpha-blends that coverage at the recording's native text size. Code names,
groups and credits share the same readable layout. Long names wrap at word
boundaries; long tokens wrap without horizontal squeezing. The existing colors,
left/right panel selection, game picture, audio and encoders are unchanged.
No additional font binary or runtime dependency is introduced. The renderer
owns copied glyph bytes rather than a pointer into the UI's rebuildable atlas.
The end-of-panel overflow marker reserves space instead of overwriting credits.

### Confirmed Delete Code
Right-click the code NAME in Cheat Engine -> Cheats or Patch and choose
**Delete Code...**. A modal identifies the exact code and source TXT. Cancel is
the default action; opening the menu alone never writes a file. Stale selections
are refused. The menu is available even with the master/live engine disabled.

Deletion removes only the owned +Code source range, including metadata, RAW
commands and Type-F executable/post-DEADCODE data. Group/header boundaries and
neighbouring definitions are checked before deletion. Implicit RAW-only entries
without an explicit +Code header are not deleted through this menu.

A unique `original.txt.delete-<uuid>.bak` is written beside the TXT, then the
new TXT is committed through GLib's consistent/durable replacement. The source
is checked before and after backup; detected external edits, read-only files,
backup errors and replacement errors are refused. This is not a filesystem
compare-and-swap against arbitrary simultaneous external editors.

Installed hooks belonging to the removed code are restored through the existing
pause/retirement path; failure to restore prevents deletion. Other codes retain
selection, active hook ownership, cached assembly and applied PREENTRY state.
Their indexes and duplicate-name ordinals are remapped rather than globally
reloading/restoring every hook. A failed disk commit keeps definitions intact;
a hook already safely restored during preparation may reinstall on the next
normal enabled tick. No other code's hook is removed for this operation.

Deleting a definition does NOT undo arbitrary RAW writes or an already-applied
startup patch. The dialog warns to reset afterwards; active deletion sets a
Reset Required indication. Safe hook retirement still protects in-flight guest
return addresses. Backups are not .txt files and are not auto-loaded as cheats.

### Lazy Type-F loading and diagnostics
Source loading now collects F0/F1/F2 definitions without invoking Keystone.
On first execution of a selected block, every Type-F definition it owns is
prevalidated before any RAW writes or hook installs in that block. Probe
success/failure is cached until a source reload. Final relocation/installation
still uses the original hook allocator and assembler paths.

Invalid live Type-F codes are deselected/disabled and report the source line;
they do not retry on every tick. A later explicit selection can retry an install
conflict, while cached malformed source still requires correcting and reloading
the TXT. Overlapping installed hook ranges identify the owning code (or debugger
cave) and refuse the new hook; this does not infer game-specific dependencies.
Selected valid codes still require compilation when first activated. This is
not a claim that every possible assembler/runtime failure has been eliminated.

Label definitions use a trailing colon; jump/call references do not:

```text
$jne LinkHybridNativeInitDone
$LinkHybridNativeInitDone:
$popad
$popfd
```

Undefined plain symbolic branch/call targets are checked before the generic
F0 encoder, with a useful colon-definition message. No silent colon insertion
is performed. F0 safety limits are 16,384 source lines and 4,096 characters per
instruction. Existing F1/F2 byte/relocation behavior is preserved.

### Saved selections
Type-F selection hashes now derive from normalized source, not compiled bytes.
This is necessary to restore selections without eagerly compiling disabled
codes. Newly saved state uses version 3 and `RAW+TYPE-F-SOURCE-V1` content hashes.
Legacy binary-hash Type-F selections are left OFF once on migration rather than
trusted by name alone; reselect the desired Type-F codes and save normally.
Ordinary RAW state matching remains available. This version number belongs to
saved cheat selections, not a further File Replace manifest-format change.

## Installation

For an existing v3.16a builder, extract the FIX-only update over its builder
root so `FIX/CMP-Official-Debug-Tools.zip` is replaced. The included matching
Patch 300 is unchanged. Do not unpack the inner Debug Tools ZIP into FIX.
Rebuild using the normal build command. No dependency-cache purge is required
by these source changes. The FIX update contains no Cheats folder and does not
overwrite the user's active TXT or File Replace packages.

Alternatively extract the full builder into a fresh directory. It preserves
the same bundled v3.16 SCII manifest/assets as the original v3.16a full builder;
keep the user's newer/custom runtime Cheats folder rather than overwriting it.
Existing manifest-v3 File Replace sets do not need conversion for v3.16b.

## Validation status

See V316B-VALIDATION.md and V316B-CHANGE-MANIFEST.json. Native tests use explicitly
labelled guest/assembler/UI seams. No full Windows/Linux/macOS XEMU executable
build, real ImGui-window interaction, in-game crash reproduction, or actual
encoded-video capture was performed here. This is the next build/runtime test
candidate, not a runtime-confirmed release.
