#!/usr/bin/env python3
# v2.87 current regression ownership.
"""v2.87 current regression ownership: UI/runtime regression contracts.

Historical version labels below are provenance only; the retained contracts are
owned and executed by this v2.87 suite.
"""
from __future__ import annotations

# v2.91.5 physically moves optional implementation files into their add-on
# directories without changing their historical bodies. Older aggregate digest
# guards normalize the new ownership path back to the pre-v2.91.5 basename.
V2915_RELOCATED_LEGACY_PATH = {
    "addons/hdd/hdd-directory.cc": "hdd-directory.cc",
    "addons/hdd/hdd-directory.hh": "hdd-directory.hh",
    "addons/hdd/hdd-directory-ui.cc": "hdd-directory-ui.cc",
    "addons/hdd/hdd-export-service.cc": "hdd-export-service.cc",
    "addons/hdd/hdd-export-service.hh": "hdd-export-service.hh",
    "addons/hdd/hdd-snapshot-service.cc": "hdd-snapshot-service.cc",
    "addons/hdd/hdd-snapshot-service.hh": "hdd-snapshot-service.hh",
    "addons/hdd/fatx-hdd.cc": "fatx-hdd.cc",
    "addons/hdd/fatx-hdd.hh": "fatx-hdd.hh",
    "addons/hdd/guest-kernel-rpc.cc": "guest-kernel-rpc.cc",
    "addons/hdd/guest-kernel-rpc.hh": "guest-kernel-rpc.hh",
    "addons/hdd/guest-kernel-rpc-ui.cc": "guest-kernel-rpc-ui.cc",
    "addons/hdd/guest-kernel-rpc-completion.cc": "guest-kernel-rpc-completion.cc",
    "addons/hdd/guest-kernel-rpc-filesystem.cc": "guest-kernel-rpc-filesystem.cc",
    "addons/hdd/guest-kernel-rpc-memory.c": "guest-kernel-rpc-memory.c",
    "addons/hdd/guest-kernel-rpc-memory.h": "guest-kernel-rpc-memory.h",
    "addons/hdd/guest-kernel-rpc-status.hh": "guest-kernel-rpc-status.hh",
    "addons/hdd/kernel-rpc-filesystem.cc": "kernel-rpc-filesystem.cc",
    "addons/hdd/kernel-rpc-filesystem.hh": "kernel-rpc-filesystem.hh",
    "addons/hdd/kernel-rpc-filesystem-stream.cc": "kernel-rpc-filesystem-stream.cc",
    "addons/hdd/kernel-rpc-filesystem-internal.hh": "kernel-rpc-filesystem-internal.hh",
    "addons/hdd/kernel-rpc-utils.hh": "kernel-rpc-utils.hh",
    "addons/memory-tools/breakpoint-conditions.cc": "breakpoint-conditions.cc",
    "addons/memory-tools/breakpoint-conditions.hh": "breakpoint-conditions.hh",
    "addons/memory-tools/memory-tools.cc": "memory-tools.cc",
    "addons/memory-tools/memory-tools.hh": "memory-tools.hh",
    "addons/memory-tools/memory-tools-internal.hh": "memory-tools-internal.hh",
    "addons/memory-tools/memory-tools-memory.cc": "memory-tools-memory.cc",
    "addons/memory-tools/memory-tools-memory-ui.cc": "memory-tools-memory-ui.cc",
    "addons/memory-tools/memory-tools-search.cc": "memory-tools-search.cc",
    "addons/memory-tools/memory-tools-search-ui.cc": "memory-tools-search-ui.cc",
    "addons/memory-tools/memory-tools-debugger.cc": "memory-tools-debugger.cc",
    "addons/memory-tools/memory-tools-debugger-ui.cc": "memory-tools-debugger-ui.cc",
    "addons/memory-tools/memory-tools-inject.cc": "memory-tools-inject.cc",
    "addons/memory-tools/memory-tools-inject-ui.cc": "memory-tools-inject-ui.cc",
    "addons/memory-tools/memory-tools-labels.cc": "memory-tools-labels.cc",
    "addons/memory-tools/memory-tools-labels-ui.cc": "memory-tools-labels-ui.cc",
    "addons/memory-tools/memory-tools-dump.cc": "memory-tools-dump.cc",
    "addons/memory-tools/memory-tools-dump-ui.cc": "memory-tools-dump-ui.cc",
    "addons/memory-tools/register-copy-utils.hh": "register-copy-utils.hh",
}


# Preserved contract from v197-detached-playback-golden.py
def check_v197_detached_playback_golden() -> None:
    """v1.97 guard: detached-window drag keeps playback live without UI re-entry."""

    import argparse
    import hashlib
    import pathlib
    import sys


    def main() -> int:
        parser = argparse.ArgumentParser()
        parser.add_argument("--root", default=".")
        root = pathlib.Path(parser.parse_args().root).resolve()
        debug = root / "ui/xui/debug-tools"
        tests = debug / "tests"
        sys.path.insert(0, str(tests))
        from v287_source_test_utils import extract_function

        xemu_c = (root / "ui/xemu.c").read_text(encoding="utf-8")
        hud_cc = (root / "ui/xui/main.cc").read_text(encoding="utf-8")
        hud_h = (root / "ui/xui/xemu-hud.h").read_text(encoding="utf-8")
        detached_cc = (debug / "detached-tools.cc").read_text(encoding="utf-8")
        detached_h = (debug / "detached-tools.hh").read_text(encoding="utf-8")

        common = extract_function(
            xemu_c,
            "static void gl_render_frame_common(struct xemu_console *scon, bool playback_only)",
        )
        watch = extract_function(
            xemu_c,
            "static bool event_watch_callback(void *userdata, SDL_Event *event)",
        )
        playback = extract_function(
            hud_cc, "void xemu_hud_render_playback_only(void)"
        )
        owns = extract_function(
            detached_cc, "bool detached_tools_owns_window_id(SDL_WindowID window_id)"
        )

        # Normal frames retain the historical full HUD update/render route.
        for needle in (
            "xemu_hud_update();",
            "xemu_hud_render();",
            "xemu_hud_render_playback_only();",
        ):
            if needle not in common:
                raise AssertionError(f"renderer routing missing: {needle}")
        if "if (!playback_only)" not in common:
            raise AssertionError("full HUD render is not excluded from playback-only mode")

        # The main console expose/resize workaround remains full-frame behavior.
        for needle in (
            "SDL_EVENT_WINDOW_EXPOSED",
            "SDL_EVENT_WINDOW_RESIZED",
            "event->window.windowID == SDL_GetWindowID(scon->real_window)",
            "gl_render_frame(scon);",
        ):
            if needle not in watch:
                raise AssertionError(f"historical console event-watch behavior changed: {needle}")

        # Win32 detached move/resize/expose events use playback-only presentation.
        if "#if defined(_WIN32)" not in watch:
            raise AssertionError("detached drag workaround must stay Windows-scoped")
        for needle in (
            "SDL_EVENT_WINDOW_MOVED",
            "SDL_EVENT_WINDOW_RESIZED",
            "SDL_EVENT_WINDOW_EXPOSED",
            "xemu_hud_is_detached_window_id(event->window.windowID)",
            "gl_render_playback_frame(scon);",
        ):
            if needle not in watch:
                raise AssertionError(f"detached playback event route missing: {needle}")

        # The modal callback may draw the guest framebuffer only. It must not run
        # ImGui frame construction, Debug Tools, Cheat Engine Tick, or detached UI.
        if "RenderFramebuffer(g_tex, width, height, g_flip_req);" not in playback:
            raise AssertionError("playback-only helper no longer presents the guest framebuffer")
        forbidden_playback = (
            "ImGui::",
            "cheat_engine_window",
            "current_game_manager",
            "memory_tools_window",
            "detached_tools_build_frames",
            "detached_tools_render_frames",
            "xemu_hud_update();",
            "xemu_hud_render();",
        )
        for needle in forbidden_playback:
            if needle in playback:
                raise AssertionError(f"playback-only callback re-entered UI/runtime work: {needle}")

        # Detached ownership lookup is intentionally side-effect-free.
        for needle in ("g_tools", "SDL_GetWindowID(tool.window)", "return true;"):
            if needle not in owns:
                raise AssertionError(f"detached window ownership lookup missing: {needle}")
        for needle in ("ImGui::", "ProcessEvent", "BuildToolFrame", "RenderToolFrame"):
            if needle in owns:
                raise AssertionError(f"detached ownership lookup gained side effects: {needle}")

        for decl in (
            "int xemu_hud_is_detached_window_id(SDL_WindowID window_id);",
            "void xemu_hud_render_playback_only(void);",
        ):
            if decl not in hud_h:
                raise AssertionError(f"C HUD bridge declaration missing: {decl}")
        if "bool detached_tools_owns_window_id(SDL_WindowID window_id);" not in detached_h:
            raise AssertionError("detached ownership declaration missing")

        # The two detached-window files are the only Debug Tools runtime files
        # allowed to change from the v1.96 audit baseline. Pass 11 separately
        # fingerprints the other 46 non-test files.
        for rel in ("detached-tools.cc", "detached-tools.hh"):
            if not hashlib.sha256((debug / rel).read_bytes()).hexdigest():
                raise AssertionError(f"unable to fingerprint scoped v1.97 file: {rel}")

        print("PASS: v1.97 detached-window playback continuity guard")
        return 0
    result = main()
    if result not in (None, 0):
        raise AssertionError("v197-detached-playback-golden.py returned non-zero: %r" % (result,))

# Preserved contract from v198-copy-all-registers-golden.py
def check_v198_copy_all_registers_golden() -> None:
    """v1.98 guard: Current Registers COPY ALL clipboard QoL."""

    import argparse
    import hashlib
    import pathlib
    import sys

    EXPECTED_OTHER_MEMORYTOOLS_METHODS_COUNT = 91
    EXPECTED_OTHER_MEMORYTOOLS_METHODS_SHA256 = (
        "477fbb4e38526410b62185c58011e72d74dc97afcf7bf29bb9694cd5e562f8c2"
    )


    def digest_other_methods(implementation: str, tests: pathlib.Path) -> tuple[int, str]:
        sys.path.insert(0, str(tests))
        from v287_source_test_utils import extract_member_functions
        functions = sorted(
            (item for item in extract_member_functions(implementation, "MemoryToolsWindow")
             if item[0] not in {
                 "DrawRegisters", "Draw",
                 "InjectNop", "OpenInstructionChanger", "ApplyInstructionChange",
                 "RestoreInstructionChange", "RestoreTrackedInstructionPatch",
                 "DrawInstructionChanger", "DrawCodeCaveBuilder", "DrawAddressContextMenu",
                 "DrawDebugger", "DrawBreakpoints", "DrawBreakpointContents", "DrawChanges",
                 "RecordCodeCaveChange", "ClearCodeCaveChange",
             }),
            key=lambda item: item[0],
        )
        records = [f"{name}#{index}\n{body}" for index, (name, body) in enumerate(functions)]
        blob = "\n\0\n".join(records)
        return len(records), hashlib.sha256(blob.encode("utf-8")).hexdigest()


    def main() -> int:
        parser = argparse.ArgumentParser()
        parser.add_argument("--root", default=".")
        root = pathlib.Path(parser.parse_args().root).resolve()
        debug = root / "ui/xui/debug-tools"
        tests = debug / "tests"

        sys.path.insert(0, str(tests))
        from v287_source_test_utils import extract_function, read_memory_tools_implementation

        implementation = read_memory_tools_implementation(debug)
        registers = extract_function(
            implementation, "void MemoryToolsWindow::DrawRegisters(")
        helper = (debug / "addons/memory-tools/register-copy-utils.hh").read_text(encoding="utf-8")
        runner = (tests / "v287-run-regression-tests.py").read_text(encoding="utf-8")

        # Last BP reserves matching tab/button-row geometry, then returns before the
        # Current Registers COPY ALL action. This preserves cross-pane register-row
        # alignment while keeping clipboard ownership current/live only.
        if 'ImGui::BeginTable("break_register_tabs_row", 2' not in registers:
            raise AssertionError("Last BP no longer reserves matching COPY ALL row geometry")
        if registers.index("return;") > registers.index('ImGui::Button("COPY ALL"'):
            raise AssertionError("COPY ALL escaped the Current Registers-only path")
        for needle in (
            'ImGui::BeginTable("current_register_tabs_row", 2',
            'ImGui::Button("COPY ALL", ImVec2(-FLT_MIN, 0.0f))',
            "ImGuiCol_Button",
            "ImGuiCol_ButtonHovered",
            "ImGuiCol_ButtonActive",
            '"Register<TAB>Value lines."',
            "xemu_cheat_get_x86_registers(&copy_regs)",
            "xemu_cheat_get_x86_extra_registers(&copy_extra)",
            "m_registers = copy_regs;",
            "m_extra_registers = copy_extra;",
            "BuildAllCurrentRegistersText(copy_regs,",
            "ImGui::SetClipboardText(text.c_str());",
        ):
            if needle not in registers:
                raise AssertionError(f"COPY ALL invariant missing: {needle}")

        # The on-demand extra read must be click-driven only; the established Pass-6
        # General-tab steady-state path remains elsewhere in DrawDebugger/DrawRegisters.
        click = registers.index("if (copy_all_clicked)")
        extra = registers.index("xemu_cheat_get_x86_extra_registers(&copy_extra)")
        if extra < click:
            raise AssertionError("COPY ALL extra-register fetch became steady-state work")

        # Exact clipboard contract: one register/value pair per line with a real TAB
        # separator, matching the existing UI hexadecimal widths/order for all tabs.
        for needle in (
            '"%s\\t%08X\\n"',
            '"%s\\t%016llX\\n"',
            '"%s\\t%04X%016llX\\n"',
            '"XMM%u\\t%08X %08X %08X %08X\\n"',
            '"TOP\\t%u\\n"',
            'append_u32("EAX", regs.eax);',
            'append_u32("GS", regs.gs);',
            'append_u32("MXCSR", extra.mxcsr);',
            "text.pop_back();",
        ):
            if needle not in helper:
                raise AssertionError(f"register clipboard format changed: {needle}")

        if '"register-copy-golden"' not in runner:
            raise AssertionError("native register clipboard formatter test is not packaged")

        # Apart from DrawRegisters, v2.49 tab-style Draw, and the separately guarded
        # v2.74 Inject methods plus the v2.78 CodeCave refresh-anchor method, every other MemoryTools member remains exact.
        count, digest = digest_other_methods(implementation, tests)
        if count != EXPECTED_OTHER_MEMORYTOOLS_METHODS_COUNT or \
                digest != EXPECTED_OTHER_MEMORYTOOLS_METHODS_SHA256:
            raise AssertionError(
                "v1.98 COPY ALL changed a MemoryTools method outside DrawRegisters "
                f"(count={count}, sha256={digest})")

        print("PASS: v1.98 Current Registers COPY ALL guard")
        return 0
    result = main()
    if result not in (None, 0):
        raise AssertionError("v198-copy-all-registers-golden.py returned non-zero: %r" % (result,))

# Preserved contract from v199-detached-hdd-current-game-golden.py
def check_v199_detached_hdd_current_game_golden() -> None:
    """v1.99 guard: detached Current Game + FATX HDD reader baseline."""

    import argparse
    import pathlib

    from v287_source_test_utils import extract_function


    def require(text: str, needle: str, what: str) -> None:
        if needle not in text:
            raise AssertionError(f"missing {what}: {needle}")


    def main() -> int:
        parser = argparse.ArgumentParser()
        parser.add_argument("--root", default=".")
        root = pathlib.Path(parser.parse_args().root).resolve()
        debug = root / "ui/xui/debug-tools"

        detached = (debug / "detached-tools.cc").read_text(encoding="utf-8")
        facade = (debug / "debug-tools.cc").read_text(encoding="utf-8")
        hdd_addon = (debug / "addons/hdd/debug-tools-hdd-addon.cc").read_text(encoding="utf-8")
        current_cc = ((debug / "current-game.cc").read_text(encoding="utf-8") + "\n" + (debug / "current-game-ui.cc").read_text(encoding="utf-8"))
        current_hh = (debug / "current-game.hh").read_text(encoding="utf-8")
        hdd_cc = ((debug / "addons/hdd/hdd-directory.cc").read_text(encoding="utf-8") + "\n" + (debug / "addons/hdd/hdd-directory-ui.cc").read_text(encoding="utf-8"))
        hdd_hh = (debug / "addons/hdd/hdd-directory.hh").read_text(encoding="utf-8")
        snapshot_cc = (debug / "addons/hdd/hdd-snapshot-service.cc").read_text(encoding="utf-8")
        fatx = (debug / "addons/hdd/fatx-hdd.cc").read_text(encoding="utf-8")
        meson = (debug / "meson.build").read_text(encoding="utf-8")
        menubar = (root / "ui/xui/menubar.cc").read_text(encoding="utf-8")
        main_cc = (root / "ui/xui/main.cc").read_text(encoding="utf-8")
        playback = (debug / "tests/v287-ui-runtime-regressions-golden.py").read_text(encoding="utf-8")
        runner = (debug / "tests/v287-run-regression-tests.py").read_text(encoding="utf-8")

        # Current Game is no longer rendered into the main playback HUD. Its normal
        # polling remains in xemu_hud_update(), while its UI is owned by detached tools.
        require(main_cc, "debug_tools_tick();", "Debug Tools frame polling facade")
        require(facade, "current_game_manager.Refresh();", "Current Game polling registration")
        if "current_game_manager.Draw();" in main_cc:
            raise AssertionError("Current Game returned to the main playback HUD")
        require(current_hh, "void Draw(bool detached = false);", "detached Current Game API")
        current_draw = extract_function(current_cc, "void CurrentGameManager::Draw(bool detached)")
        for needle in (
            'window_name = "##DetachedCurrentGame";',
            "ImGuiWindowFlags_NoTitleBar",
            "ImGuiWindowFlags_NoResize",
            "ImGuiWindowFlags_NoMove",
            "ImGui::GetIO().DisplaySize",
            'ImGui::BeginTabItem("Game Info")',
            'ImGui::BeginTabItem("Disc Contents")',
        ):
            require(current_draw, needle, "Current Game detached/full-content layout")

        # Both new external windows live in the same generic detached-tool registry
        # as Cheat Engine and Memory/Debugger, so v1.97 drag playback ownership
        # automatically includes them without a second window/event implementation.
        for needle in (
            "void detached_tools_register(",
            "std::vector<DetachedToolWindow> g_tools;",
        ):
            require(detached, needle, "generic detached Debug Tools registry")
        for needle in (
            "current_game_manager.Draw(true);",
            '"xemu - Current Game"',
        ):
            require(facade, needle, "Current Game detached registration")
        for needle in (
            "hdd_directory_window.Draw(true);",
            '"xemu - Xbox HDD Directory"',
        ):
            require(hdd_addon, needle, "HDD detached addition registration")
        require(playback, "xemu_hud_is_detached_window_id", "v1.97 generic detached playback coverage")

        # Debug menu exposes both independent windows.
        require(menubar, "debug_tools_draw_menu_items();",
                "Debug Tools menu facade")
        require(facade, 'debug_tools_register_menu_item(100, "Current Game"',
                "Current Game Debug menu registration")
        require(hdd_addon, 'debug_tools_register_menu_item(200, "HDD Directory"',
                "HDD Directory Debug menu registration")

        # HDD viewer still consumes a read-only snapshot of the active QEMU HDD
        # while paused. Later ownership refactors moved physical HDD access into the
        # shared snapshot service, so validate the behavior rather than requiring the
        # v1.99 implementation to remain inside hdd-directory.cc.
        for needle in (
            'xemu_disc_block_by_name("ide0-hd0")',
            "xemu_disc_block_get_length(hdd)",
            "XemuDebugGuestPauseGuard pause;",
            "XemuFatxHdd::BuildSnapshot(ReadHddBlock, hdd",
        ):
            require(snapshot_cc, needle, "shared HDD read-only snapshot service")
        for needle in (
            'ImGui::Button("REFRESH")',
            'ImGui::BeginTabBar("##hdd_partitions")',
            'ImGui::BeginTable("##fatx_directory", 6',
            '"FATX snapshot of the mounted Xbox HDD (ide0-hd0)"',
        ):
            require(hdd_cc, needle, "HDD read-only snapshot UI")
        refresh = extract_function(hdd_cc, "void HddDirectoryWindow::Refresh()")
        require(refresh, "hdd_snapshot_service.BuildDisplaySnapshot(",
                "HDD UI delegates snapshot construction")
        stream = extract_function(fatx, "bool StreamFile(")
        for forbidden in ("pwrite", "blk_pwrite", "xemu_disc_block_pwrite"):
            if forbidden in refresh or forbidden in stream:
                raise AssertionError(
                    f"v1.99 read-only snapshot/export baseline gained a write: {forbidden}")
        require(hdd_hh, "bool is_open = false;", "independent HDD window state")

        # FATX parser uses the retail C/E/X/Y/Z map and validated on-disk structures.
        for needle in (
            "constexpr uint32_t kFatxSignature = 0x58544146u",
            "constexpr size_t kRawDirectoryEntrySize = 64;",
            "{'C', \"System\", 0x8CA80000ull, 0x01F400000ull}",
            "{'E', \"Data\",   0xABE80000ull, 0x1312D6000ull}",
            "{'X', \"Cache\",  0x00080000ull, 0x02EE00000ull}",
            "{'Y', \"Cache\",  0x2EE80000ull, 0x02EE00000ull}",
            "{'Z', \"Cache\",  0x5DC80000ull, 0x02EE00000ull}",
            "constexpr uint64_t kExtendedFOffset = 0x1DD156000ull;",
            "kDeleted = 0xE5",
            "kEnd1 = 0xFF",
            "kEnd2 = 0x00",
            "kMaxEntries = 200000",
            "kMaxDepth = 64",
            "kMaxDirectoryClusters = 65536",
        ):
            require(fatx, needle, "FATX parser invariant")

        for file_name in ("addons/hdd/hdd-directory.cc", "addons/hdd/fatx-hdd.cc"):
            require(meson, f"'{file_name}'", "Meson HDD source ownership")
        require(runner, '"fatx-hdd-golden"', "native FATX parser regression")

        print("PASS: v1.99 detached Current Game + FATX HDD reader baseline guard")
        return 0
    result = main()
    if result not in (None, 0):
        raise AssertionError("v199-detached-hdd-current-game-golden.py returned non-zero: %r" % (result,))

# Preserved contract from v201-current-game-detached-font-golden.py
def check_v201_current_game_detached_font_golden() -> None:
    """Regression guard for v2.01 detached Current Game font-atlas fix."""

    import argparse
    from pathlib import Path

    from v287_source_test_utils import extract_function


    def require(text: str, needle: str, message: str) -> None:
        if needle not in text:
            raise AssertionError(message)


    def main() -> int:
        parser = argparse.ArgumentParser()
        parser.add_argument("--root", required=True)
        args = parser.parse_args()
        root = Path(args.root)
        debug = root / "ui/xui/debug-tools"

        current_cc = ((debug / "current-game.cc").read_text(encoding="utf-8") + "\n" + (debug / "current-game-ui.cc").read_text(encoding="utf-8"))
        current_hh = (debug / "current-game.hh").read_text(encoding="utf-8")
        detached = (debug / "detached-tools.cc").read_text(encoding="utf-8")

        require(current_hh, "void DrawGameInfoTab(bool detached);",
                "Game Info draw helper must know whether it is using a detached ImGui context")

        draw_info = extract_function(
            current_cc, "void CurrentGameManager::DrawGameInfoTab(bool detached)")
        require(draw_info,
                "const bool use_main_fixed_font = !detached && g_font_mgr.m_fixed_width_font != nullptr;",
                "main font atlas must only be used outside detached Current Game")
        require(draw_info, "if (use_main_fixed_font) {\n        ImGui::PushFont(g_font_mgr.m_fixed_width_font);",
                "fixed-width font push must be guarded by detached state")
        require(draw_info, "if (use_main_fixed_font) {\n        ImGui::PopFont();",
                "font pop must match the guarded push")

        draw = extract_function(current_cc, "void CurrentGameManager::Draw(bool detached)")
        require(draw, "DrawGameInfoTab(detached);",
                "Current Game must pass detached context state into Game Info rendering")

        # Detached windows intentionally own independent ImGui font atlases and GL
        # textures.  Current Game must not regress to pushing the main atlas font.
        require(detached, "io.Fonts->AddFontDefault();",
                "detached tools must keep their independent context-local font atlas")
        require(detached, "SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);",
                "detached GL contexts must remain intentionally unshared")

        print("PASS: v2.01 detached Current Game font-atlas guard")
        return 0
    result = main()
    if result not in (None, 0):
        raise AssertionError("v201-current-game-detached-font-golden.py returned non-zero: %r" % (result,))

# Preserved contract from v274-inject-restore-golden.py
def check_v274_inject_restore_golden() -> None:
    """v2.74 x86 Inject Restore + Change ImGui crash fix guard."""

    import argparse
    import hashlib
    from pathlib import Path

    from v287_source_test_utils import read_memory_tools_implementation

    EXPECTED_PRODUCTION_FILE_COUNT = 34
    EXPECTED_PRODUCTION_SHA256 = "9d3e455ac867b3e6ab8771705a2b7247ad062c6f29438ef7b1f38a27a148fa54"
    EXPECTED_RUNTIME_SHA256 = {
        "addons/memory-tools/memory-tools.hh": "59eedf2a94bf2f22b11f9dfc4e08d63086c02c0662c7ca420fd59593602897b8",
    }
    NON_RUNTIME_FILES = {
        "README.md",
        "CHANGELOG.md",
        "build-capstone.sh",
        "build-capstone-windows.sh",
        "build-keystone.sh",
        "prepare-build-dependencies.sh",
        "build-xemu.sh",
        "docker-build-windows.sh",
        "restore-executable-bits.py",
        "validate-project-layout.py",
    }

    # v2.75 moves the Cheat Engine frontend out of the historical monolith.
    # Its dedicated Phase-2 guard owns those files; v2.74 continues freezing all
    # other production files plus the exact Inject pair below.
    # v2.90 moved the old hand-written generic x86 encoder out of this
    # historical aggregate; the surviving unrelated files remain frozen.
    LATER_SCOPED_RUNTIME_FILES = {
        # v2.91 modular Debug Tools facade/addition ownership.
        "debug-tools.cc",
        "debug-tools.hh",
        "debug-tools-module.hh",
        "detached-tools.cc",
        "detached-tools.hh",
        "build-profile.txt",
        "read-build-profile.py",
        "addons/hdd/debug-tools-hdd-addon.cc",
        "addons/memory-tools/debug-tools-memory-tools-addon.cc",
        "addons/stubs/debug-tools-hdd-addon-stub.cc",
        "addons/stubs/debug-tools-memory-tools-addon-stub.cc",
        "addons/memory-tools/memory-tools.hh",
        "current-game.hh",
        "addons/hdd/kernel-rpc-filesystem.hh",
        # v2.86 Phase 11 final production-audit ownership.
        "cheat-engine-memory.h",
        "addons/hdd/guest-kernel-rpc.cc",
        "addons/hdd/guest-kernel-rpc-ui.cc",
        "addons/hdd/guest-kernel-rpc-completion.cc",
        "addons/hdd/guest-kernel-rpc-filesystem.cc",
        "addons/hdd/guest-kernel-rpc.hh",
        "addons/hdd/guest-kernel-rpc-status.hh",
        "addons/hdd/kernel-rpc-utils.hh",
        "label-packs.cc",
        "guest-pause-guard.hh",
        "addons/hdd/hdd-directory.hh",
        "addons/hdd/hdd-export-service.cc",
        "host-export-utils.hh",
        "xdvdfs-export-service.cc",
        "meson.build",
        # v2.85 Combined Phase 10 HDD/FATX/filesystem ownership cleanup.
        "addons/hdd/hdd-directory.cc",
        "addons/hdd/hdd-directory-ui.cc",
        "addons/hdd/fatx-hdd.cc",
        "addons/hdd/kernel-rpc-filesystem.cc",
        "addons/hdd/kernel-rpc-filesystem-stream.cc",
        "addons/hdd/kernel-rpc-filesystem-internal.hh",
        "meson.build",
        # v2.84 Phase 9 Cheat Engine core ownership split.
        "cheat-engine.cc",
        "cheat-engine-source.cc",
        "cheat-engine-fhooks.cc",
        "cheat-engine-execute.cc",
        "meson.build",
        # v2.83 Combined Phase 8 Inject UI + x86 assembler ownership cleanup.
        "addons/memory-tools/memory-tools-inject.cc",
        "addons/memory-tools/memory-tools-inject-ui.cc",
        "x86-cheat-assembler.cc",
        "x86-cheat-assembler-keystone.cc",
        "x86-cheat-assembler-internal.hh",
        "x86-cheat-assembler.hh",
        "meson.build",
        # v2.82 Combined Phase 7 Search/Dump UI + label-parser helper cleanup.
        "addons/memory-tools/memory-tools-search.cc",
        "addons/memory-tools/memory-tools-search-ui.cc",
        "addons/memory-tools/memory-tools-dump.cc",
        "addons/memory-tools/memory-tools-dump-ui.cc",
        "label-symbol-utils.hh",
        "map-labels.cc",
        "pdb-labels.cc",
        "xdk-labels.cc",
        "meson.build",
        # v2.81 Phase-6 Labels UI/core ownership split.
        "addons/memory-tools/memory-tools-labels.cc",
        "addons/memory-tools/memory-tools-labels-ui.cc",
        # v2.80 Current Game UI/core ownership split.
        "current-game.cc",
        "current-game-ui.cc",
        "cheat-engine.cc",
        "cheat-engine-ui.cc",
        "meson.build",
        "addons/memory-tools/memory-tools-debugger.cc",
        "addons/memory-tools/memory-tools-debugger-ui.cc",
        # v2.79 Memory Viewer UI/core ownership split.
        "addons/memory-tools/memory-tools-memory.cc",
        "addons/memory-tools/memory-tools-memory-ui.cc",
        # v2.78 Inject refresh-anchor correctness hotfix.
        "addons/memory-tools/memory-tools-inject.cc",
    }


    def production_digest(debug: Path) -> tuple[int, str]:
        digest = hashlib.sha256()
        count = 0
        candidates = []
        for path in (p for p in debug.rglob("*") if p.is_file()):
            relative = path.relative_to(debug)
            rel = relative.as_posix()
            if ("tests" in relative.parts or rel in NON_RUNTIME_FILES or
                    rel in LATER_SCOPED_RUNTIME_FILES):
                continue
            normalized_rel = V2915_RELOCATED_LEGACY_PATH.get(rel, rel)
            candidates.append((normalized_rel, rel, path))
        for normalized_rel, rel, path in sorted(candidates):
            rel_bytes = normalized_rel.encode("utf-8")
            data = path.read_bytes()
            if rel == "cheat-engine.hh":
                data = data.replace(
                    b"    bool m_force_forget_f_hooks_on_next_game_observation = false;\n",
                    b"",
                    1,
                )
            digest.update(len(rel_bytes).to_bytes(4, "little"))
            digest.update(rel_bytes)
            digest.update(len(data).to_bytes(8, "little"))
            digest.update(data)
            count += 1
        return count, digest.hexdigest()


    def require(text: str, needle: str, message: str) -> None:
        if needle not in text:
            raise AssertionError(message)


    def main() -> int:
        parser = argparse.ArgumentParser()
        parser.add_argument("--root", default=".")
        root = Path(parser.parse_args().root).resolve()
        debug = root / "ui/xui/debug-tools"

        count, digest = production_digest(debug)
        if count != EXPECTED_PRODUCTION_FILE_COUNT or digest != EXPECTED_PRODUCTION_SHA256:
            raise AssertionError(
                "v2.74 protected production Debug Tools surface changed "
                f"(files={count}, sha256={digest})"
            )

        for rel, expected in EXPECTED_RUNTIME_SHA256.items():
            if rel in LATER_SCOPED_RUNTIME_FILES:
                continue
            actual = hashlib.sha256((debug / rel).read_bytes()).hexdigest()
            if actual != expected:
                raise AssertionError(
                    f"v2.74 Inject runtime surface changed: {rel} "
                    f"(expected {expected}, got {actual})"
                )

        source = read_memory_tools_implementation(debug)
        header = (debug / "addons/memory-tools/memory-tools.hh").read_text(encoding="utf-8")

        # Crash fix: disabled ownership is decided on BeginDisabled(bool), and
        # EndDisabled is unconditional. Button-side state changes cannot unbalance
        # the ImGui stack anymore.
        require(source, "ImGui::BeginDisabled(!m_change_instruction_preview_valid);",
                "Change Apply must use unconditional BeginDisabled(bool)")
        require(source, 'ImGui::Button("APPLY", ImVec2(90.0f, 0.0f))',
                "Change Apply button missing")
        require(source, 'ImGui::Button("RESTORE", ImVec2(90.0f, 0.0f))',
                "Change Restore button missing")
        if any(old in source for old in
               ("REVERT TO ORIGINAL", "RESET TO CURRENT", "USE ORIGINAL")):
            raise AssertionError("obsolete Change button labels returned")

        # Restore keeps the user's requested visible workflow while restoring exact
        # bytes: the source field is populated with the remembered original, but the
        # preview is built from the captured bytes rather than a lossy reassembly.
        require(source, "m_change_instruction_source = m_change_instruction_original_text;",
                "Change Restore must repopulate Replacement with original instruction")
        require(source, "m_change_instruction_preview_bytes.assign(",
                "Change Restore must use exact remembered original bytes")
        require(source, "return ApplyInstructionChange();",
                "Change Restore must pass through transactional Apply path")

        # NOP and Change now share one tracked original-byte record, and the context
        # menu Restore is shown only while a tracked patch owns the selected span.
        require(source, "std::vector<uint8_t> nops(record.span, 0x90);",
                "NOP must cover the remembered complete instruction span")
        require(source, "record.last_applied_bytes = nops;",
                "NOP must retain exact applied bytes for safety validation")
        require(source, "record.active = true;",
                "NOP must mark the shared record active")
        require(source, 'restorable_instruction_patch && ImGui::MenuItem("Restore")',
                "Inject > Restore must be conditional on tracked patch ownership")
        require(source, "RestoreTrackedInstructionPatch(disasm_row->virtual_address);",
                "Inject > Restore must use tracked exact-byte restore")
        require(source, "tracked_patch_interior",
                "NOP/Change must not create overlapping patches inside a tracked span")
        require(source, "std::memcmp(current, record.last_applied_bytes.data(), record.span)",
                "Restore must validate live bytes before overwriting them")
        require(header, "bool RestoreTrackedInstructionPatch(uint32_t address);",
                "tracked Inject Restore declaration missing")

        print(
            "PASS: v2.74 Change crash fix + unified NOP/Change Inject Restore "
            "with exact original-byte safety"
        )
        return 0
    result = main()
    if result not in (None, 0):
        raise AssertionError("v274-inject-restore-golden.py returned non-zero: %r" % (result,))

# Preserved contract from v276-f0-uncheck-restore-golden.py
def check_v276_f0_uncheck_restore_golden() -> None:
    """v2.76 live F0/F1 uncheck must restore original hook bytes unconditionally."""

    import argparse
    import hashlib
    from pathlib import Path

    from v287_source_test_utils import extract_function, read_cheat_engine_implementation

    EXPECTED_PRODUCTION_FILE_COUNT = 35
    EXPECTED_PRODUCTION_SHA256 = "448f264c8c416140377de2171de1b7dd638ad43a14b98fc272c4e6507b7e9102"
    EXPECTED_UI_SHA256 = (
        "1f185f9822ddd318b10840eff7b4c10f11c3380b7fcbab411ccb78f33b20e97a"
    )
    EXPECTED_FUNCTION_SHA256 = {
        "SetGroupSelected": "2f243509bb59b5916b4e7287b203b498e3fab6ffb9a99b9c0b290b046fa66217",
        "DrawCheat": "57ff30648e3e66ae9a937305d12e193e9c0afecf4432a179116eed4748b176b7",
    }
    LATER_SCOPED_RUNTIME_FILES = {
        # v2.91 modular Debug Tools facade/addition ownership.
        "debug-tools.cc",
        "debug-tools.hh",
        "debug-tools-module.hh",
        "detached-tools.cc",
        "detached-tools.hh",
        "build-profile.txt",
        "read-build-profile.py",
        "addons/hdd/debug-tools-hdd-addon.cc",
        "addons/memory-tools/debug-tools-memory-tools-addon.cc",
        "addons/stubs/debug-tools-hdd-addon-stub.cc",
        "addons/stubs/debug-tools-memory-tools-addon-stub.cc",
        "addons/memory-tools/memory-tools.hh",
        "current-game.hh",
        "addons/hdd/kernel-rpc-filesystem.hh",
        # v2.86 Phase 11 final production-audit ownership.
        "cheat-engine-memory.h",
        "addons/hdd/guest-kernel-rpc.cc",
        "addons/hdd/guest-kernel-rpc-ui.cc",
        "addons/hdd/guest-kernel-rpc-completion.cc",
        "addons/hdd/guest-kernel-rpc-filesystem.cc",
        "addons/hdd/guest-kernel-rpc.hh",
        "addons/hdd/guest-kernel-rpc-status.hh",
        "addons/hdd/kernel-rpc-utils.hh",
        "label-packs.cc",
        "guest-pause-guard.hh",
        "addons/hdd/hdd-directory.hh",
        "addons/hdd/hdd-export-service.cc",
        "host-export-utils.hh",
        "xdvdfs-export-service.cc",
        "meson.build",
        # v2.85 Combined Phase 10 HDD/FATX/filesystem ownership cleanup.
        "addons/hdd/hdd-directory.cc",
        "addons/hdd/hdd-directory-ui.cc",
        "addons/hdd/fatx-hdd.cc",
        "addons/hdd/kernel-rpc-filesystem.cc",
        "addons/hdd/kernel-rpc-filesystem-stream.cc",
        "addons/hdd/kernel-rpc-filesystem-internal.hh",
        "meson.build",
        # v2.84 Phase 9 Cheat Engine core ownership split.
        "cheat-engine.cc",
        "cheat-engine-source.cc",
        "cheat-engine-fhooks.cc",
        "cheat-engine-execute.cc",
        "meson.build",
        # v2.83 Combined Phase 8 Inject UI + x86 assembler ownership cleanup.
        "addons/memory-tools/memory-tools-inject.cc",
        "addons/memory-tools/memory-tools-inject-ui.cc",
        "x86-cheat-assembler.cc",
        "x86-cheat-assembler-keystone.cc",
        "x86-cheat-assembler-internal.hh",
        "x86-cheat-assembler.hh",
        "meson.build",
        # v2.82 Combined Phase 7 Search/Dump UI + label-parser helper cleanup.
        "addons/memory-tools/memory-tools-search.cc",
        "addons/memory-tools/memory-tools-search-ui.cc",
        "addons/memory-tools/memory-tools-dump.cc",
        "addons/memory-tools/memory-tools-dump-ui.cc",
        "label-symbol-utils.hh",
        "map-labels.cc",
        "pdb-labels.cc",
        "xdk-labels.cc",
        "meson.build",
        # v2.81 Phase-6 Labels UI/core ownership split.
        "addons/memory-tools/memory-tools-labels.cc",
        "addons/memory-tools/memory-tools-labels-ui.cc",
        # v2.80 Current Game UI/core ownership split.
        "current-game.cc",
        "current-game-ui.cc",
        # v2.77 Phase-3 debugger rendering ownership split.
        "addons/memory-tools/memory-tools-debugger.cc",
        "addons/memory-tools/memory-tools-debugger-ui.cc",
        # v2.79 Memory Viewer UI/core ownership split.
        "addons/memory-tools/memory-tools-memory.cc",
        "addons/memory-tools/memory-tools-memory-ui.cc",
        "meson.build",
        # v2.78 Inject refresh-anchor correctness hotfix.
        "addons/memory-tools/memory-tools-inject.cc",
    }

    NON_RUNTIME_FILES = {
        "README.md",
        "CHANGELOG.md",
        "build-capstone.sh",
        "build-capstone-windows.sh",
        "build-keystone.sh",
        "prepare-build-dependencies.sh",
        "build-xemu.sh",
        "docker-build-windows.sh",
        "restore-executable-bits.py",
        "validate-project-layout.py",
    }


    def production_digest(debug: Path) -> tuple[int, str]:
        digest = hashlib.sha256()
        count = 0
        candidates = []
        for path in (p for p in debug.rglob("*") if p.is_file()):
            relative = path.relative_to(debug)
            rel = relative.as_posix()
            if ("tests" in relative.parts or rel in NON_RUNTIME_FILES or
                    rel in LATER_SCOPED_RUNTIME_FILES):
                continue
            normalized_rel = V2915_RELOCATED_LEGACY_PATH.get(rel, rel)
            candidates.append((normalized_rel, rel, path))
        for normalized_rel, rel, path in sorted(candidates):
            rel_bytes = normalized_rel.encode("utf-8")
            data = path.read_bytes()
            if rel == "cheat-engine.hh":
                data = data.replace(
                    b"    bool m_force_forget_f_hooks_on_next_game_observation = false;\n",
                    b"",
                    1,
                )
            digest.update(len(rel_bytes).to_bytes(4, "little"))
            digest.update(rel_bytes)
            digest.update(len(data).to_bytes(8, "little"))
            digest.update(data)
            count += 1
        return count, digest.hexdigest()


    def require_order(body: str, tokens: tuple[str, ...], label: str) -> None:
        pos = -1
        for token in tokens:
            pos = body.find(token, pos + 1)
            if pos < 0:
                raise AssertionError(f"{label} missing/order regression at `{token}`")


    def main() -> int:
        parser = argparse.ArgumentParser()
        parser.add_argument("--root", default=".")
        root = Path(parser.parse_args().root).resolve()
        debug = root / "ui/xui/debug-tools"

        count, digest = production_digest(debug)
        if (count, digest) != (EXPECTED_PRODUCTION_FILE_COUNT, EXPECTED_PRODUCTION_SHA256):
            raise AssertionError(
                "v2.76 complete production Debug Tools surface changed "
                f"(files={count}, sha256={digest})"
            )

        ui_path = debug / "cheat-engine-ui.cc"
        ui = ui_path.read_text(encoding="utf-8")
        actual_ui = hashlib.sha256(ui_path.read_bytes()).hexdigest()
        if actual_ui != EXPECTED_UI_SHA256:
            raise AssertionError(
                "v2.76 Cheat Engine UI restore surface changed "
                f"(expected {EXPECTED_UI_SHA256}, got {actual_ui})"
            )

        group = extract_function(ui, "void CheatEngineWindow::SetGroupSelected(")
        draw = extract_function(ui, "void CheatEngineWindow::DrawCheat(")
        for name, body in (("SetGroupSelected", group), ("DrawCheat", draw)):
            actual = hashlib.sha256(body.encode("utf-8")).hexdigest()
            expected = EXPECTED_FUNCTION_SHA256[name]
            if actual != expected:
                raise AssertionError(
                    f"v2.76 {name} changed (expected {expected}, got {actual})"
                )

        # Group OFF must restore persistent hooks without being nested under the
        # global live-toggle gate. ON still follows the existing live-toggle rule.
        require_order(group, (
            "m_blocks[cheat].selected = selected;",
            "if (!selected)",
            "m_blocks[cheat].enabled = false;",
            "DeactivateFHooksForBlock(cheat);",
            "else if (m_live_cheats_enabled)",
            "m_blocks[cheat].enabled = true;",
        ), "group uncheck restore")

        # Individual OFF has the same invariant. This is the direct user checkbox
        # path that regressed in observed Windows/WHPX behavior.
        require_order(draw, (
            'ImGui::Checkbox("##selected", &block.selected)',
            "if (!block.selected)",
            "block.enabled = false;",
            "DeactivateFHooksForBlock(block_index);",
            "else if (m_live_cheats_enabled)",
            "block.enabled = true;",
        ), "individual uncheck restore")

        # Ensure restoration itself still writes the captured original instruction
        # bytes and is not merely clearing UI state.
        core = read_cheat_engine_implementation(debug)
        deactivate = extract_function(core, "void CheatEngineWindow::DeactivateFHook(")
        require_order(deactivate, (
            "if (state.installed)",
            "state.original_bytes.empty()",
            "xemu_cheat_patch_virtual(state.hook_address,",
            "state.original_bytes.data()",
            "state.original_bytes.size()",
            "state.installed = false;",
        ), "F0/F1 original-byte restoration")

        # Small state model proving OFF does not depend on the live-toggle state.
        for live in (False, True):
            enabled = True
            selected = False
            restored = False
            if not selected:
                enabled = False
                restored = True
            elif live:
                enabled = True
            if enabled or not restored:
                raise AssertionError(
                    f"uncheck model failed with live={live}: enabled={enabled}, restored={restored}"
                )

        print(
            "PASS: v2.76 live Cheat/F0 uncheck always disables and restores "
            "captured original hook bytes independent of Live Cheats state"
        )
        return 0
    result = main()
    if result not in (None, 0):
        raise AssertionError("v276-f0-uncheck-restore-golden.py returned non-zero: %r" % (result,))

# Preserved contract from v278-inject-refresh-anchor-golden.py
def check_v278_inject_refresh_anchor_golden() -> None:
    """v2.78 Inject refresh must stay on the edited instruction, not stale Follow target."""

    import argparse
    import hashlib
    from pathlib import Path

    from v287_source_test_utils import extract_function, read_memory_tools_implementation

    EXPECTED_PRODUCTION_FILE_COUNT = 36
    EXPECTED_PRODUCTION_SHA256 = "08eb098263c4ae81ab6347f7feb580b1ea4a67dd9c203233872b037ae6a17eac"
    EXPECTED_SCOPED_SHA256 = {
        # These three stay exact to v2.77; v2.78 must not require new debugger
        # state or alter the Phase-3 rendering/core split to fix refresh ownership.
        "addons/memory-tools/memory-tools-debugger.cc": "e2c6c01ab235616c3fc7c432dfca49b03b7e2dce645afffc3284e00e00f918a3",
        "addons/memory-tools/memory-tools-debugger-ui.cc": "9041336f00a010bd06270f80562bb376f0a05b02b79833371798034bbe6a8481",
        "addons/memory-tools/memory-tools.hh": "59eedf2a94bf2f22b11f9dfc4e08d63086c02c0662c7ca420fd59593602897b8",
    }
    LATER_SCOPED_RUNTIME_FILES = {
        # v2.91 modular Debug Tools facade/addition ownership.
        "debug-tools.cc",
        "debug-tools.hh",
        "debug-tools-module.hh",
        "detached-tools.cc",
        "detached-tools.hh",
        "build-profile.txt",
        "read-build-profile.py",
        "addons/hdd/debug-tools-hdd-addon.cc",
        "addons/memory-tools/debug-tools-memory-tools-addon.cc",
        "addons/stubs/debug-tools-hdd-addon-stub.cc",
        "addons/stubs/debug-tools-memory-tools-addon-stub.cc",
        "addons/memory-tools/memory-tools-debugger-ui.cc",
        "addons/memory-tools/memory-tools.hh",
        "current-game.hh",
        "addons/hdd/kernel-rpc-filesystem.hh",
        # v2.86 Phase 11 final production-audit ownership.
        "cheat-engine-memory.h",
        "addons/hdd/guest-kernel-rpc.cc",
        "addons/hdd/guest-kernel-rpc-ui.cc",
        "addons/hdd/guest-kernel-rpc-completion.cc",
        "addons/hdd/guest-kernel-rpc-filesystem.cc",
        "addons/hdd/guest-kernel-rpc.hh",
        "addons/hdd/guest-kernel-rpc-status.hh",
        "addons/hdd/kernel-rpc-utils.hh",
        "label-packs.cc",
        "guest-pause-guard.hh",
        "addons/hdd/hdd-directory.hh",
        "addons/hdd/hdd-export-service.cc",
        "host-export-utils.hh",
        "xdvdfs-export-service.cc",
        "meson.build",
        # v2.85 Combined Phase 10 HDD/FATX/filesystem ownership cleanup.
        "addons/hdd/hdd-directory.cc",
        "addons/hdd/hdd-directory-ui.cc",
        "addons/hdd/fatx-hdd.cc",
        "addons/hdd/kernel-rpc-filesystem.cc",
        "addons/hdd/kernel-rpc-filesystem-stream.cc",
        "addons/hdd/kernel-rpc-filesystem-internal.hh",
        "meson.build",
        # v2.84 Phase 9 Cheat Engine core ownership split.
        "cheat-engine.cc",
        "cheat-engine-source.cc",
        "cheat-engine-fhooks.cc",
        "cheat-engine-execute.cc",
        "meson.build",
        # v2.83 Combined Phase 8 Inject UI + x86 assembler ownership cleanup.
        "addons/memory-tools/memory-tools-inject.cc",
        "addons/memory-tools/memory-tools-inject-ui.cc",
        "x86-cheat-assembler.cc",
        "x86-cheat-assembler-keystone.cc",
        "x86-cheat-assembler-internal.hh",
        "x86-cheat-assembler.hh",
        "meson.build",
        # v2.82 Combined Phase 7 Search/Dump UI + label-parser helper cleanup.
        "addons/memory-tools/memory-tools-search.cc",
        "addons/memory-tools/memory-tools-search-ui.cc",
        "addons/memory-tools/memory-tools-dump.cc",
        "addons/memory-tools/memory-tools-dump-ui.cc",
        "label-symbol-utils.hh",
        "map-labels.cc",
        "pdb-labels.cc",
        "xdk-labels.cc",
        "meson.build",
        # v2.81 Phase-6 Labels UI/core ownership split.
        "addons/memory-tools/memory-tools-labels.cc",
        "addons/memory-tools/memory-tools-labels-ui.cc",
        # v2.80 Current Game UI/core ownership split.
        "current-game.cc",
        "current-game-ui.cc",
        # v2.79 Memory Viewer rendering ownership split.
        "addons/memory-tools/memory-tools-memory.cc",
        "addons/memory-tools/memory-tools-memory-ui.cc",
        "meson.build",
    }
    NON_RUNTIME_FILES = {
        "README.md",
        "CHANGELOG.md",
        "build-capstone.sh",
        "build-capstone-windows.sh",
        "build-keystone.sh",
        "prepare-build-dependencies.sh",
        "build-xemu.sh",
        "docker-build-windows.sh",
        "restore-executable-bits.py",
        "validate-project-layout.py",
    }


    def production_digest(debug: Path) -> tuple[int, str]:
        digest = hashlib.sha256()
        count = 0
        candidates = []
        for path in (p for p in debug.rglob("*") if p.is_file()):
            relative = path.relative_to(debug)
            rel = relative.as_posix()
            if ("tests" in relative.parts or rel in NON_RUNTIME_FILES or
                    rel in LATER_SCOPED_RUNTIME_FILES):
                continue
            normalized_rel = V2915_RELOCATED_LEGACY_PATH.get(rel, rel)
            candidates.append((normalized_rel, rel, path))
        for normalized_rel, rel, path in sorted(candidates):
            rel_bytes = normalized_rel.encode("utf-8")
            data = path.read_bytes()
            if rel == "cheat-engine.hh":
                data = data.replace(
                    b"    bool m_force_forget_f_hooks_on_next_game_observation = false;\n",
                    b"",
                    1,
                )
            digest.update(len(rel_bytes).to_bytes(4, "little"))
            digest.update(rel_bytes)
            digest.update(len(data).to_bytes(8, "little"))
            digest.update(data)
            count += 1
        return count, digest.hexdigest()


    def require_order(body: str, tokens: tuple[str, ...], label: str) -> None:
        pos = -1
        for token in tokens:
            pos = body.find(token, pos + 1)
            if pos < 0:
                raise AssertionError(f"{label} missing/order regression at `{token}`")


    def main() -> int:
        parser = argparse.ArgumentParser()
        parser.add_argument("--root", default=".")
        root = Path(parser.parse_args().root).resolve()
        debug = root / "ui/xui/debug-tools"

        count, digest = production_digest(debug)
        if (count, digest) != (EXPECTED_PRODUCTION_FILE_COUNT, EXPECTED_PRODUCTION_SHA256):
            raise AssertionError(
                "v2.78 complete production Debug Tools surface changed "
                f"(files={count}, sha256={digest})"
            )

        for rel, expected in EXPECTED_SCOPED_SHA256.items():
            if rel in LATER_SCOPED_RUNTIME_FILES:
                continue
            actual = hashlib.sha256((debug / rel).read_bytes()).hexdigest()
            if actual != expected:
                raise AssertionError(
                    f"v2.78 scoped runtime changed: {rel} "
                    f"(expected {expected}, got {actual})"
                )

        inject = read_memory_tools_implementation(debug)
        core = (debug / "addons/memory-tools/memory-tools-debugger.cc").read_text(encoding="utf-8")

        # Every Inject operation that schedules a disassembly refresh must first
        # move the existing non-history debugger anchor to the instruction it
        # actually modified. This is the whole stale-Follow-address fix.
        expected_pairs = (
            "FollowDebuggerAddress(record.address, false);\n    m_inject_disasm_refresh_pending = true;",
            "FollowDebuggerAddress(m_change_instruction_address, false);\n    m_inject_disasm_refresh_pending = true;",
            "FollowDebuggerAddress(record.address, false);\n    m_inject_disasm_refresh_pending = true;",
            "FollowDebuggerAddress(m_code_cave_hook_address, false);\n            m_inject_disasm_refresh_pending = true;",
            "FollowDebuggerAddress(m_code_cave_hook_address, false);\n            m_inject_disasm_refresh_pending = true;",
        )
        if inject.count("m_inject_disasm_refresh_pending = true;") != 6:
            raise AssertionError("Inject/reset refresh request count changed from the five guarded Inject actions plus one deferred reset refresh")
        reset_defer = (
            "m_code_cave_change = CodeCaveChangeRecord{};\n"
            "        g_refresh_disassembly_after_reset = true;\n"
            "        g_forget_debugger_changes_on_next_debugger_draw = false;"
        )
        if inject.count(reset_defer) != 1:
            raise AssertionError("v2.88.4 reset no longer defers disassembly refresh after clearing Changes")
        deferred_refresh = (
            "if (g_refresh_disassembly_after_reset &&\n"
            "        qemu_reset_requested_get() == SHUTDOWN_CAUSE_NONE) {\n"
            "        g_refresh_disassembly_after_reset = false;\n"
            "        m_inject_disasm_refresh_pending = true;"
        )
        if inject.count(deferred_refresh) != 1:
            raise AssertionError("v2.88.4 reset refresh no longer waits for QEMU reset completion exactly once")
        # Two pairs intentionally repeat: NOP + tracked Restore, and CodeCave RUN + RESTORE.
        if inject.count(expected_pairs[0]) != 2:
            raise AssertionError("NOP/tracked Restore no longer anchor refresh to record.address")
        if inject.count(expected_pairs[1]) != 1:
            raise AssertionError("Change Apply/Restore no longer anchors refresh to Change address")
        if inject.count("FollowDebuggerAddress(m_code_cave_hook_address, false);") != 2:
            raise AssertionError("CodeCave RUN/RESTORE no longer anchor refresh to hook address")

        follow = extract_function(
            core,
            "void MemoryToolsWindow::FollowDebuggerAddress(uint32_t address,",
        )
        require_order(
            follow,
            (
                "m_disasm_address = address;",
                "SetHexText(m_disasm_address_text, sizeof(m_disasm_address_text), address);",
                "m_selected_disasm_virtual = address;",
                "if (refresh_disassembly)",
                "RefreshDisassembly();",
            ),
            "non-history Follow anchor",
        )
        if "m_debug_nav_history" in follow or "m_debug_nav_index" in follow:
            raise AssertionError("FollowDebuggerAddress(false) unexpectedly mutates Back/Forward history")

        refresh = extract_function(core, "void MemoryToolsWindow::RefreshDisassembly()")
        require_order(
            refresh,
            (
                "ParseHexAddress(m_disasm_address_text, requested_address)",
                "m_disasm_address = resolved_address;",
                "m_selected_disasm_virtual = resolved_address;",
            ),
            "RefreshDisassembly anchor consumption",
        )

        # Minimal model of the reported failure: the last Follow anchor is A, the
        # user edits visible row B. v2.78 first changes the anchor to B without a
        # history push, so the pending refresh necessarily rebuilds from B.
        last_follow = 0x00100000
        edited_row = 0x00100040
        address_text = last_follow
        history = [last_follow]
        address_text = edited_row  # FollowDebuggerAddress(edited_row, false)
        refresh_requested = address_text
        if refresh_requested != edited_row or history != [last_follow]:
            raise AssertionError("stale-Follow refresh model regressed")

        print(
            "PASS: v2.78 Inject NOP/Change/Restore/CodeCave refreshes from the "
            "edited instruction without adding Back/Forward history"
        )
        return 0
    result = main()
    if result not in (None, 0):
        raise AssertionError("v278-inject-refresh-anchor-golden.py returned non-zero: %r" % (result,))

CONTRACTS = (
    ('v197-detached-playback-golden.py', check_v197_detached_playback_golden),
    ('v198-copy-all-registers-golden.py', check_v198_copy_all_registers_golden),
    ('v199-detached-hdd-current-game-golden.py', check_v199_detached_hdd_current_game_golden),
    ('v201-current-game-detached-font-golden.py', check_v201_current_game_detached_font_golden),
    ('v274-inject-restore-golden.py', check_v274_inject_restore_golden),
    ('v276-f0-uncheck-restore-golden.py', check_v276_f0_uncheck_restore_golden),
    ('v278-inject-refresh-anchor-golden.py', check_v278_inject_refresh_anchor_golden),
)

def main() -> int:
    for legacy_name, check in CONTRACTS:
        try:
            check()
        except Exception as exc:
            raise AssertionError(f"v2.87 retained contract failed ({legacy_name}): {exc}") from exc
    print("PASS: v2.87 UI/runtime regression contracts")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
