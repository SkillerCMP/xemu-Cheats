#!/usr/bin/env python3
# v2.87 current regression ownership.
"""Regression/model guard for v1.85 OPT Pass 1 streamlining."""
from __future__ import annotations

import argparse
import pathlib
import random

from v287_source_test_utils import extract_function, read_memory_tools_implementation, read_cheat_engine_implementation

PAGE = 0x1000
ROW = 16
CACHE_SLOTS = 4


def cached_read(memory: bytes, address: int, size: int,
                cache: list[tuple[int, bytes]], fetches: list[int]) -> bytes:
    """Model the fixed four-page frame-local ring used by the Memory Viewer."""
    out = bytearray()
    cursor = address
    remaining = size
    replacement = 0
    while remaining:
        base = cursor & ~(PAGE - 1)
        off = cursor - base
        chunk = min(remaining, PAGE - off)
        page = None
        for cached_base, cached_bytes in cache:
            if cached_base == base:
                page = cached_bytes
                break
        if page is None:
            fetches[0] += 1
            page = memory[base:base + PAGE]
            if len(cache) < CACHE_SLOTS:
                cache.append((base, page))
            else:
                cache[replacement % CACHE_SLOTS] = (base, page)
                replacement += 1
        if off + chunk > len(page):
            return memory[address:address + size]
        out += page[off:off + chunk]
        cursor += chunk
        remaining -= chunk
    return bytes(out)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    args = parser.parse_args()
    root = pathlib.Path(args.root).resolve()
    debug = root / "ui/xui/debug-tools"

    memory_tools = read_memory_tools_implementation(debug)
    cheat_cc = read_cheat_engine_implementation(debug)
    cheat_hh = (debug / "cheat-engine.hh").read_text(encoding="utf-8")
    bridge = (debug / "cheat-engine-memory.c").read_text(encoding="utf-8")

    viewer = extract_function(
        memory_tools, "bool MemoryToolsWindow::DrawScrollableMemoryPane(")
    for needle in (
        "struct ViewerPageSnapshot",
        "std::array<ViewerPageSnapshot, 4> viewer_page_cache;",
        "get_viewer_page",
        "read_viewer_cached",
        "return Read(space, original_address, out, original_size);",
        "read_viewer_cached(row_address, bytes",
    ):
        if needle not in viewer:
            raise AssertionError(f"missing Memory Viewer Pass-1 invariant: {needle}")
    if "const bool ok = Read(space, row_address, bytes, bytes_to_read);" in viewer:
        raise AssertionError("Memory Viewer regressed to one guest read per row")

    # Equivalent byte results for ordinary rows and page-boundary crossings.
    rng = random.Random(0x18501)
    memory = bytes(rng.randrange(256) for _ in range(PAGE * 12))
    for _ in range(10000):
        address = rng.randrange(len(memory) - ROW)
        size = rng.randrange(1, ROW + 1)
        cache: list[tuple[int, bytes]] = []
        fetches = [0]
        got = cached_read(memory, address, size, cache, fetches)
        expected = memory[address:address + size]
        if got != expected:
            raise AssertionError((address, size, got, expected))

    # A large normal viewport wholly within one page should fetch that page once.
    cache = []
    fetches = [0]
    for row in range(128):
        address = 0x3000 + row * ROW
        got = cached_read(memory, address, ROW, cache, fetches)
        if got != memory[address:address + ROW]:
            raise AssertionError("viewer model mismatch")
    if fetches[0] != 1:
        raise AssertionError(f"expected one page fetch, got {fetches[0]}")

    # F0 bank metadata is borrowed from a lifecycle-invalidated cache rather
    # than copied/sorted from m_f_hooks every debugger frame.
    if "const std::vector<FTempBankInfo> &GetActiveF0TempBanks() const;" not in cheat_hh:
        raise AssertionError("F0 temp-bank getter is not a borrowed const cache")
    getter = extract_function(
        cheat_cc, "CheatEngineWindow::GetActiveF0TempBanks() const")
    for needle in (
        "if (!m_f_temp_bank_cache_dirty)",
        "return m_f_temp_bank_cache;",
        "std::sort(m_f_temp_bank_cache.begin()",
        "m_f_temp_bank_cache_dirty = false;",
    ):
        if needle not in getter:
            raise AssertionError(f"missing F0 bank cache invariant: {needle}")
    draw_t = extract_function(memory_tools, "void MemoryToolsWindow::DrawF0TempRegisters()")
    if "const auto &banks = cheat_engine_window.GetActiveF0TempBanks();" not in draw_t:
        raise AssertionError("debugger still copies/rebuilds F0 bank metadata")

    for signature in (
        "void CheatEngineWindow::ParseSource(bool preserve_states)",
        "bool CheatEngineWindow::InstallFHook(",
        "void CheatEngineWindow::DeactivateFHook(uint64_t key)",
    ):
        body = extract_function(cheat_cc, signature)
        if "InvalidateFTempBankCache();" not in body:
            raise AssertionError(f"F0 bank cache invalidation missing from {signature}")

    # v2.50 moved title-change ownership invalidation into one separately
    # guarded helper. Preserve the original Pass-1 cache invalidation contract
    # without requiring that cleanup to remain textually inline forever.
    auto_load = extract_function(
        cheat_cc, "void CheatEngineWindow::MaybeAutoLoadCurrentGame()")
    if "ForgetFHookOwnershipForNewGuest();" not in auto_load:
        raise AssertionError("title-change F0 ownership cleanup helper is not called")
    forget = extract_function(
        cheat_cc, "void CheatEngineWindow::ForgetFHookOwnershipForNewGuest()")
    if "InvalidateFTempBankCache();" not in forget:
        raise AssertionError("F0 bank cache invalidation missing from title-change cleanup")

    # RAM mapping collection now makes one bounded allocation from list.num;
    # it must not realloc once per accepted mapping.
    mappings = extract_function(
        bridge, "int xemu_cheat_collect_ram_virtual_mappings(")
    if "result = g_new(XemuCheatVirtualMapping, list.num);" not in mappings:
        raise AssertionError("mapping snapshot is missing one-shot allocation")
    if "result = g_renew(" in mappings:
        raise AssertionError("mapping snapshot regressed to per-entry reallocations")
    if "if (result_count == 0)" not in mappings or "result = NULL;" not in mappings:
        raise AssertionError("empty mapping result must retain canonical NULL output")

    print("PASS: v1.85 OPT Pass-1 streamlining regression checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
