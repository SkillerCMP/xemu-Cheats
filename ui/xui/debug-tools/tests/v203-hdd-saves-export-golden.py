#!/usr/bin/env python3
"""v2.03 guard: FATX metadata names + Current Game HDD + read-only export."""
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

    fatx_hh = (debug / "fatx-hdd.hh").read_text(encoding="utf-8")
    fatx_cc = (debug / "fatx-hdd.cc").read_text(encoding="utf-8")
    hdd_hh = (debug / "hdd-directory.hh").read_text(encoding="utf-8")
    hdd_cc = (debug / "hdd-directory.cc").read_text(encoding="utf-8")
    current = (debug / "current-game.cc").read_text(encoding="utf-8")
    native = (debug / "tests/fatx-hdd-golden.cpp").read_text(encoding="utf-8")

    # Friendly names are display-only: raw FATX names stay authoritative.
    for needle in (
        "std::string friendly_name;",
        "bool PopulateXboxMetadata(",
        'FindChildNoCase(title.children, "TitleMeta.xbx")',
        'MetadataValue(bytes, "TitleName")',
        'FindChildNoCase(save.children, "SaveMeta.xbx")',
        'MetadataValue(bytes, "Name")',
        'return entry.name + " - " + entry.friendly_name;',
    ):
        require(fatx_hh + fatx_cc, needle, "metadata/friendly-name support")

    # Export is HDD-read-only and streams file data rather than buffering a
    # whole save/DLC tree in RAM. The selected path is re-resolved from a fresh
    # snapshot inside the pause transaction before any file bytes are copied.
    for needle in (
        "using WriteCallback = bool (*)(void *opaque, const void *buffer, size_t size);",
        "bool StreamFile(",
        "XemuDebugGuestPauseGuard pause;",
        "XemuFatxHdd::Snapshot fresh;",
        "XemuFatxHdd::BuildSnapshot(ReadHddBlock, hdd",
        "XemuFatxHdd::FindEntry(*partition, target.path)",
        "XemuFatxHdd::StreamFile(",
        '"EXPORT FILE..."',
        '"EXPORT FOLDER..."',
        '"EXPORT SAVE FOLDER..."',
        "ShowOpenFolderDialog(nullptr",
    ):
        require(fatx_hh + fatx_cc + hdd_cc, needle, "read-only export path")
    for forbidden in (
        "xemu_disc_block_pwrite",
        "blk_pwrite",
        "FATX import",
        "IMPORT SAVE",
        "IMPORT FOLDER",
    ):
        if forbidden in fatx_cc or forbidden in hdd_cc:
            raise AssertionError(f"v2.03 unexpectedly gained HDD write/import path: {forbidden}")

    refresh = extract_function(hdd_cc, "void HddDirectoryWindow::Refresh()")
    if refresh.find("BuildSnapshot") > refresh.find("PopulateXboxMetadata"):
        raise AssertionError("metadata names must be populated after the coherent FATX snapshot")
    require(refresh, "XemuDebugGuestPauseGuard pause;",
            "metadata read under the same coherent pause snapshot")

    # Current Game gets an HDD tab and filters E:\\UDATA / E:\\TDATA using the
    # running XBE Title ID, while the standalone HDD window remains independent.
    draw = extract_function(current, "void CurrentGameManager::Draw(bool detached)")
    for needle in (
        'ImGui::BeginTabItem("HDD")',
        "hdd_directory_window.DrawCurrentGameHdd(",
        "m_info.valid ? m_info.title_id : 0",
    ):
        require(draw, needle, "Current Game HDD tab")
    for needle in (
        "void DrawCurrentGameHdd(uint32_t title_id);",
        'ImGui::BeginTabItem("Saves / UDATA")',
        'ImGui::BeginTabItem("DLC / TDATA")',
        'FindChildNoCase(data->entries, "UDATA")',
        'FindChildNoCase(data->entries, "TDATA")',
        'std::snprintf(title_text, sizeof(title_text), "%08X", title_id);',
        '"EXPORT ALL SAVES..."',
        '"EXPORT TITLE DATA..."',
    ):
        require(hdd_hh + hdd_cc, needle, "Current Game Title-ID HDD filtering/export")

    # Native synthetic image covers both metadata decoding and byte-accurate
    # streamed export in addition to the original retail layout/tree parser.
    for needle in (
        'Utf16Le("TitleName=Ratatouille\\r\\n")',
        'Utf16Le("Name=Save Game 1\\r\\n")',
        "XemuFatxHdd::PopulateXboxMetadata(",
        "XemuFatxHdd::StreamFile(",
        'friendly_name == "Ratatouille"',
        'friendly_name == "Save Game 1"',
        "assert(exported == foo_data);",
    ):
        require(native, needle, "native FATX metadata/export coverage")

    print("PASS: v2.03 FATX save metadata + Current Game HDD + export guard")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
