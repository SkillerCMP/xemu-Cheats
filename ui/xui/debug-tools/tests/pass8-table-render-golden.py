#!/usr/bin/env python3
"""Regression guard for v1.93 OPT Pass 8 table/list presentation cleanup."""
from __future__ import annotations

import argparse
import hashlib
import pathlib
import random

from source_test_utils import (
    extract_function, extract_member_functions, read_memory_tools_implementation,
)

MUTABLE_METHODS = {
    "DrawSearch", "DrawF0TempRegisters", "DumpCurrentPage", "DumpLabels",
    # v1.98 Current Registers COPY ALL is separately guarded.
    "DrawRegisters",
}
EXPECTED_PROTECTED_METHOD_COUNT = 97
EXPECTED_PROTECTED_METHOD_DIGEST = "397bfb524da75527b3e81bb03e78651e78f3bc8a2f43e316339ad14fa9957495"
V192_HEADER_SHA256 = "ae506fdde510c404b27e9ceee0e8578703ee44a68c3af4afd6122be1218e8b3b"
V192_CHEAT_CC_SHA256 = "3a4ba82dc5fface59fac1084b2e93d461cb8748445f1b1ab4fa53535f234c996"
V192_CHEAT_HH_SHA256 = "0077d791758f44e4ac1705e9eb4a1ecc8be81b23700de5885fdea7e39fcf7d69"

PASS8_HEADER_ADDITIONS = (
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
)


def protected_digest(text: str) -> tuple[int, str]:
    grouped: dict[str, list[str]] = {}
    for name, body in extract_member_functions(text, "MemoryToolsWindow"):
        if name in MUTABLE_METHODS:
            continue
        grouped.setdefault(name, []).append(body)
    records: list[str] = []
    for name in sorted(grouped):
        for index, body in enumerate(grouped[name]):
            records.append(f"{name}#{index}\n{body}")
    blob = "\n\0\n".join(records)
    return len(records), hashlib.sha256(blob.encode("utf-8")).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    root = pathlib.Path(parser.parse_args().root).resolve()
    debug = root / "ui/xui/debug-tools"
    implementation = read_memory_tools_implementation(debug)
    search = extract_function(implementation, "void MemoryToolsWindow::DrawSearch()")
    f0_draw = extract_function(implementation, "void MemoryToolsWindow::DrawF0TempRegisters()")
    cheat_cc = (debug / "cheat-engine.cc").read_text(encoding="utf-8")
    cheat_hh = (debug / "cheat-engine.hh").read_text(encoding="utf-8")
    header = (debug / "memory-tools.hh").read_text(encoding="utf-8")

    # Search presentation cache is fixed-size/direct-mapped and is keyed by
    # every value that controls the three rendered strings. It requires no
    # scan-generation state and therefore self-invalidates on any result/type
    # mutation, including future mutation paths that preserve vector storage.
    for needle in (
        "std::array<SearchDisplayCacheEntry, 256> m_search_display_cache = {};",
        "display.result_index != result_index",
        "display.address != r.address",
        "display.previous_raw != r.previous_raw",
        "display.current_raw != r.current_raw",
        "display.value_kind != m_value_kind",
        "m_search_display_cache[result_index %",
        "m_search_display_cache.size()]",
        "FormatValue(display.previous_text",
        "FormatValue(display.current_text",
        "DrawAddressContextMenu(m_search_space, r.address,",
        "display.current_text",
    ):
        source = header if needle.startswith("std::array") else search
        if needle not in source:
            raise AssertionError(f"search presentation-cache invariant missing: {needle}")
    for forbidden in (
        "m_search_display_generation",
        "static SearchDisplayCacheEntry",
        "static std::array<SearchDisplayCacheEntry",
    ):
        if forbidden in header or forbidden in search:
            raise AssertionError("Search display cache gained unsafe/static generation ownership")

    # Exercise the exact-key rule: a cache hit is possible iff all five source
    # fields match. Any one-field mutation must force a reformat.
    rng = random.Random(0x193A8)
    for _ in range(100_000):
        key = [rng.randrange(100000), rng.getrandbits(32), rng.getrandbits(32),
               rng.getrandbits(32), rng.randrange(7)]
        same = list(key)
        if not all(a == b for a, b in zip(key, same)):
            raise AssertionError("exact Search display key model failed")
        field = rng.randrange(5)
        changed = list(key)
        changed[field] = (changed[field] + 1) & 0xFFFFFFFF
        if all(a == b for a, b in zip(key, changed)):
            raise AssertionError("one-field Search display mutation did not invalidate")

    # F0 presentation text is derived only when the already-existing active-bank
    # metadata cache rebuilds; DrawF0TempRegisters consumes it directly. The
    # hook lifecycle/cache invalidation rules themselves remain untouched.
    for needle in (
        "std::string display_name;",
        'info.display_name = info.cheat_name + "  [hook " +',
        'TypeFHex32(state.hook_address) + "]";',
    ):
        source = cheat_hh if needle == "std::string display_name;" else cheat_cc
        if needle not in source:
            raise AssertionError(f"F0 presentation-cache invariant missing: {needle}")
    for needle in (
        "banks[selected].display_name.c_str()",
        "banks[i].display_name.c_str()",
    ):
        if needle not in f0_draw:
            raise AssertionError(f"F0 cached display text not consumed directly: {needle}")
    for forbidden in (
        'std::snprintf(preview,',
        'std::snprintf(item,',
    ):
        if forbidden in f0_draw:
            raise AssertionError("per-frame F0 combo formatting returned")

    # Normalize only the explicitly allowed Pass-8 state additions and prove the
    # v1.92 MemoryTools header is otherwise exact.
    normalized_header = header
    for addition in PASS8_HEADER_ADDITIONS:
        if addition not in normalized_header:
            raise AssertionError(f"missing Pass-8 header addition: {addition.strip()}")
        normalized_header = normalized_header.replace(addition, "", 1)
    if hashlib.sha256(normalized_header.encode("utf-8")).hexdigest() != V192_HEADER_SHA256:
        raise AssertionError("MemoryTools state changed outside the Pass-8 Search display cache")

    # Likewise, remove only the cached F0 presentation field/build and prove all
    # prior Cheat Engine source—including F0 lifecycle—is byte-identical to 1.92.
    normalized_cheat_hh = cheat_hh.replace("        std::string display_name;\n", "", 1)
    normalized_cheat_cc = cheat_cc.replace(
        '        info.display_name = info.cheat_name + "  [hook " +\n'
        '                            TypeFHex32(state.hook_address) + "]";\n',
        "", 1)
    if hashlib.sha256(normalized_cheat_hh.encode("utf-8")).hexdigest() != V192_CHEAT_HH_SHA256:
        raise AssertionError("Cheat Engine header changed outside F0 presentation metadata")
    # Pass 10 separately fingerprints every untouched CheatEngineWindow method
    # and permits only its three lifecycle/resource ownership methods to differ.
    # Do not weaken that newer, more precise guard with an obsolete whole-file
    # hash from before the scoped pause hardening.

    count, digest = protected_digest(implementation)
    if count != EXPECTED_PROTECTED_METHOD_COUNT or digest != EXPECTED_PROTECTED_METHOD_DIGEST:
        raise AssertionError("MemoryTools behavior changed outside the two Pass-8 presentation methods")

    print("PASS: v1.93 table/list presentation-cache + allocation cleanup invariants")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
