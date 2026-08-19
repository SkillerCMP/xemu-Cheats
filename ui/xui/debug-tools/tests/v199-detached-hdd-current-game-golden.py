#!/usr/bin/env python3
"""v1.99 guard: detached Current Game + read-only FATX HDD Directory window."""
from __future__ import annotations

import argparse
import pathlib

from source_test_utils import extract_function


def require(text: str, needle: str, what: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing {what}: {needle}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    root = pathlib.Path(parser.parse_args().root).resolve()
    debug = root / "ui/xui/debug-tools"

    detached = (debug / "detached-tools.cc").read_text(encoding="utf-8")
    current_cc = (debug / "current-game.cc").read_text(encoding="utf-8")
    current_hh = (debug / "current-game.hh").read_text(encoding="utf-8")
    hdd_cc = (debug / "hdd-directory.cc").read_text(encoding="utf-8")
    hdd_hh = (debug / "hdd-directory.hh").read_text(encoding="utf-8")
    fatx = (debug / "fatx-hdd.cc").read_text(encoding="utf-8")
    meson = (debug / "meson.build").read_text(encoding="utf-8")
    menubar = (root / "ui/xui/menubar.cc").read_text(encoding="utf-8")
    main_cc = (root / "ui/xui/main.cc").read_text(encoding="utf-8")
    playback = (debug / "tests/v197-detached-playback-golden.py").read_text(encoding="utf-8")
    runner = (debug / "tests/run-regression-tests.py").read_text(encoding="utf-8")

    # Current Game is no longer rendered into the main playback HUD. Its normal
    # polling remains in xemu_hud_update(), while its UI is owned by detached tools.
    require(main_cc, "current_game_manager.Refresh();", "Current Game polling")
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
        "current_game_manager.Draw(true);",
        "hdd_directory_window.Draw(true);",
        '"xemu - Current Game"',
        '"xemu - Xbox HDD Directory"',
        "&g_current_game_tool,",
        "&g_hdd_tool,",
        "&g_cheat_tool,",
        "&g_memory_tool,",
    ):
        require(detached, needle, "detached Debug Tools registration")
    require(playback, "xemu_hud_is_detached_window_id", "v1.97 generic detached playback coverage")

    # Debug menu exposes both independent windows.
    require(menubar,
            'ImGui::MenuItem("Current Game", NULL, &current_game_manager.is_open);',
            "Current Game Debug menu item")
    require(menubar,
            'ImGui::MenuItem("HDD Directory", NULL, &hdd_directory_window.is_open);',
            "HDD Directory Debug menu item")

    # HDD viewer snapshots the active QEMU HDD backend read-only while paused.
    for needle in (
        'xemu_disc_block_by_name("ide0-hd0")',
        "xemu_disc_block_get_length(hdd)",
        "XemuDebugGuestPauseGuard pause;",
        "XemuFatxHdd::BuildSnapshot(ReadHddBlock, hdd",
        'ImGui::Button("REFRESH")',
        'ImGui::BeginTabBar("##hdd_partitions")',
        'ImGui::BeginTable("##fatx_directory", 6',
        '"Read-only snapshot of the mounted Xbox HDD (ide0-hd0)"',
    ):
        require(hdd_cc, needle, "HDD read-only snapshot UI")
    for forbidden in ("pwrite", "blk_pwrite", "xemu_disc_block_pwrite"):
        if forbidden in hdd_cc or forbidden in fatx:
            raise AssertionError(f"HDD viewer gained a write path: {forbidden}")
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

    for file_name in ("hdd-directory.cc", "fatx-hdd.cc"):
        require(meson, f"'{file_name}'", "Meson HDD source ownership")
    require(runner, '"fatx-hdd-golden"', "native FATX parser regression")

    print("PASS: v1.99 detached Current Game + read-only HDD Directory guard")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
