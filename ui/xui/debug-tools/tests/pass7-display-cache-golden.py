#!/usr/bin/env python3
"""Regression guard for v1.92 OPT Pass 7 display/label cache cleanup."""
from __future__ import annotations

import argparse
import bisect
import hashlib
import pathlib
import random

from source_test_utils import (
    extract_function, extract_member_functions, read_memory_tools_implementation,
)


EXPECTED_PROTECTED_METHOD_COUNT = 93
EXPECTED_PROTECTED_METHOD_DIGEST = (
    "aa41d65ea9599e90f43e7360e715cbb337a210e49883793eed477665ceda07ad"
)
PASS7_MUTABLE_METHODS = {
    "DrawDebugger",
    "DrawDisassemblyPane",
    "DrawLabelBrowser",
    "RebuildDisassemblyRenderCache",
    # Later Pass 8 presentation-cache-only methods are separately guarded.
    "DrawSearch",
    "DrawF0TempRegisters",
    # Pass 10 lifecycle-only pause ownership changes are separately guarded.
    "DumpCurrentPage",
    "DumpLabels",
    # v1.98 Current Registers COPY ALL is separately guarded.
    "DrawRegisters",
}
PROTECTED_FILE_SHA256 = {
    "memory-tools.hh": "ae506fdde510c404b27e9ceee0e8578703ee44a68c3af4afd6122be1218e8b3b",
    "memory-tools-memory.cc": "cc342f973e9ecbc2f667b147f41686fd56063aa558a68035583d4f12e5987cda",
    "memory-tools-inject.cc": "07ca236adbe9b879b69b0557cd11189e1deaa50e0509fb7525a82d418d9d4609",
    "cheat-engine.hh": "0077d791758f44e4ac1705e9eb4a1ecc8be81b23700de5885fdea7e39fcf7d69",
    "cheat-engine-memory.c": "64f7027881f3f29a0c95a4c1fcf350bf39d06e42f4508955b9bdeefde50a359d",
    "cheat-engine-memory.h": "e47bb779c456cc58bc3bd6e7852c1f72332af4c35d8d82f6195eb1e62bc79d90",
    "external-code-memory.c": "149d9da2cbe8187b6af0448d61024ca5f9ec2d9cc7364ce56188fd6a4eaf580d",
}


def protected_digest(text: str) -> tuple[int, str]:
    functions = sorted(
        (item for item in extract_member_functions(text, "MemoryToolsWindow")
         if item[0] not in PASS7_MUTABLE_METHODS),
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
    implementation = read_memory_tools_implementation(debug)

    rebuild = extract_function(
        implementation, "void MemoryToolsWindow::RebuildDisassemblyRenderCache()")
    pane = extract_function(
        implementation, "bool MemoryToolsWindow::DrawDisassemblyPane(bool physical)")
    draw = extract_function(implementation, "void MemoryToolsWindow::DrawDebugger()")
    labels = extract_function(implementation, "void MemoryToolsWindow::DrawLabelBrowser()")

    # The two panes consume one immutable text cache. Cache freshness is tested
    # once before the paired table, not independently inside both pane draws.
    for needle in (
        "const uint64_t disassembly_label_generation =",
        "current_game_manager.LabelGeneration();",
        "m_disassembly_virtual_text.size() != m_disassembly_rows.size()",
        "m_disassembly_physical_text.size() != m_disassembly_rows.size()",
        "m_disassembly_label_generation != disassembly_label_generation",
        "m_disassembly_cached_labels_enabled != m_labels_enabled",
        "RebuildDisassemblyRenderCache();",
    ):
        if needle not in draw:
            raise AssertionError(f"single-frame disassembly cache guard missing: {needle}")
    for forbidden in ("LabelGeneration()", "RebuildDisassemblyRenderCache();"):
        if forbidden in pane:
            raise AssertionError("paired pane repeated shared render-cache validation")

    # Rebuilding display text retains exact primary-label semantics, but uses
    # one lower_bound followed by a forward cursor over the already-sorted label
    # database instead of a binary lookup for every decoded instruction row.
    for needle in (
        "const auto &label_database = current_game_manager.Labels();",
        "auto label_it = label_database.labels.begin();",
        "label_it = std::lower_bound(",
        "m_disassembly_rows.front().virtual_address",
        "while (label_it != label_end &&",
        "label_it->virtual_address < row.virtual_address",
        "label_it->virtual_address == row.virtual_address",
    ):
        if needle not in rebuild:
            raise AssertionError(f"linear primary-label cursor invariant missing: {needle}")
    if "current_game_manager.PrimaryLabelAt(" in rebuild:
        raise AssertionError("per-row PrimaryLabelAt lookup returned to render-cache rebuild")

    # Model the cursor against the old per-row lower_bound semantics, including
    # duplicate labels at the same Virtual address. The primary label remains
    # the first sorted label at an exact address.
    rng = random.Random(0x192D15A7)
    for _ in range(25_000):
        labels_model = sorted(rng.randrange(0, 0x4000) for _ in range(rng.randrange(0, 160)))
        rows_model = sorted(set(rng.randrange(0, 0x4000) for _ in range(rng.randrange(0, 120))))
        cursor = bisect.bisect_left(labels_model, rows_model[0]) if rows_model else 0
        for address in rows_model:
            old_pos = bisect.bisect_left(labels_model, address)
            old = labels_model[old_pos] if old_pos < len(labels_model) and labels_model[old_pos] == address else None
            while cursor < len(labels_model) and labels_model[cursor] < address:
                cursor += 1
            new = labels_model[cursor] if cursor < len(labels_model) and labels_model[cursor] == address else None
            if new != old:
                raise AssertionError("linear label cursor changed PrimaryAt exact-address semantics")

    # Label Physical addresses remain live. Page reuse exists only for this one
    # browser draw while stopped; running state keeps the old direct translator.
    for needle in (
        "const bool can_translate = !database.labels.empty() &&",
        "xemu_cheat_prepare_virtual_map() != 0;",
        "const bool cache_physical_pages = can_translate && !runstate_is_running();",
        "std::array<LabelPageTranslation, 32> label_page_translations = {};",
        "if (!cache_physical_pages)",
        "return xemu_cheat_virtual_to_physical(address, &physical) != 0;",
        "const uint32_t virtual_page = address & 0xFFFFF000u;",
        "xemu_cheat_virtual_to_physical(virtual_page, &physical_page)",
        "physical_page + (uint64_t)(address - virtual_page)",
        "translate_label_address(label.virtual_address, physical)",
        "translate_label_address(selected_label->virtual_address,",
    ):
        if needle not in labels:
            raise AssertionError(f"frame-local label translation invariant missing: {needle}")
    for forbidden in (
        "m_label_page_translation",
        "static std::array<LabelPageTranslation",
        "static LabelPageTranslation",
    ):
        if forbidden in labels or forbidden in implementation:
            raise AssertionError("label translation cache escaped DrawLabelBrowser frame ownership")

    count, digest = protected_digest(implementation)
    if count != EXPECTED_PROTECTED_METHOD_COUNT or digest != EXPECTED_PROTECTED_METHOD_DIGEST:
        raise AssertionError("MemoryTools behavior changed outside the four Pass-7 display/cache methods")

    for filename, expected in PROTECTED_FILE_SHA256.items():
        data = (debug / filename).read_text(encoding="utf-8")
        if filename == "memory-tools.hh":
            for addition in (
                "#include <array>\n",
                """    /* Small direct-mapped presentation cache for clipped Search-result rows.
     * Every entry carries its complete render key, so reuse is allowed only
     * when index/address/raw values/value kind are all still identical. */
    struct SearchDisplayCacheEntry {
        size_t result_index = SIZE_MAX;
        uint32_t address = 0;
        uint32_t previous_raw = 0;
        uint32_t current_raw = 0;
        ValueKind value_kind = ValueKind::U32;
        char address_text[16] = {};
        char previous_text[64] = {};
        char current_text[64] = {};
    };

""",
                "    std::array<SearchDisplayCacheEntry, 256> m_search_display_cache = {};\n",
            ):
                data = data.replace(addition, "", 1)
        elif filename == "cheat-engine.hh":
            data = data.replace("        std::string display_name;\n", "", 1)
        elif filename == "cheat-engine.cc":
            data = data.replace(
                "        info.display_name = info.cheat_name + \"  [hook \" +\n"
                "                            TypeFHex32(state.hook_address) + \"]\";\n",
                "", 1)
        actual = hashlib.sha256(data.encode("utf-8")).hexdigest()
        if actual != expected:
            raise AssertionError(f"protected 1.91 file changed in Pass 7: {filename}")

    print("PASS: v1.92 disassembly/label display-cache streamlining invariants")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
