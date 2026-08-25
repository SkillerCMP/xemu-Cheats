#!/usr/bin/env python3
# v2.87 current regression ownership.
"""Regression guard for v1.90 OPT Pass 5 recurring-runtime/XBE polling cleanup."""
from __future__ import annotations

import argparse
import hashlib
import pathlib

from v287_source_test_utils import extract_function, extract_member_functions


EXPECTED_CURRENT_GAME_METHOD_COUNT = 22
EXPECTED_CURRENT_GAME_METHOD_DIGEST = (
    "dec840e7fda4d935579a28a7591dccc776bfa7ccb2474038f69676df06494b2f"
)
EXPECTED_CURRENT_GAME_PUBLIC_HEADER_SHA256 = (
    "10964c3501137b4519e491358037df9c4471fe1df0b404e45eb7882ac064b95b"
)


def protected_current_game_digest(text: str) -> tuple[int, str]:
    functions = sorted(
        (item for item in extract_member_functions(text, "CurrentGameManager")
         if item[0] not in {
             "Refresh", "RefreshRunningXbe", "RefreshInternal",
             "Draw", "DrawGameInfoTab",
             "DrawDiscEntry", "DrawDiscContentsTab", "RequestDiscExport",
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

    cc = ((debug / "current-game.cc").read_text(encoding="utf-8") + "\n" + (debug / "current-game-ui.cc").read_text(encoding="utf-8"))
    hh = (debug / "current-game.hh").read_text(encoding="utf-8")
    xbe = (root / "xemu-xbe.c").read_text(encoding="utf-8")

    # Poll cadence is intentionally unchanged. Pass 5 optimizes work performed
    # after each proof-of-identity read; it does not delay game-change detection.
    if "constexpr uint64_t kRefreshIntervalMs = 500;" not in cc:
        raise AssertionError("Current Game 500 ms detection cadence changed")

    refresh_wrapper = extract_function(cc, "void CurrentGameManager::Refresh(bool force)")
    if "RefreshInternal(force, true);" not in refresh_wrapper:
        raise AssertionError("normal Current Game refresh stopped owning full disc refresh")
    refresh = extract_function(cc, "void CurrentGameManager::RefreshInternal(")
    for needle in (
        "struct xbe *xbe = xemu_get_xbe_info();",
        "m_loaded_xbe_headers.size() == xbe->headers_len",
        "std::memcmp(m_loaded_xbe_headers.data(), xbe->headers,",
        "m_loaded_xbe_derived_valid",
        "!force && m_info.valid",
        "RefreshDisc(false);",
        "m_loaded_xbe_headers.assign(xbe->headers,",
        "next.header_sha256 = sha256_headers(xbe);",
        "title_name_from_xbe(xbe, next.title_name)",
        "m_loaded_xbe_headers.clear();",
        "m_loaded_xbe_derived_valid = false;",
        "RefreshDisc(force);",
        "RefreshDisc(true);",
        "RefreshDisc(force || game_changed);",
    ):
        if needle not in refresh:
            raise AssertionError(f"Pass-5 XBE polling invariant missing: {needle}")

    # The fast path must be reached only after xemu_get_xbe_info() has performed
    # the complete header reread. A title/version/base-only fingerprint would not
    # prove equality of the SHA-256 identity used by v1.89.
    get_pos = refresh.index("xemu_get_xbe_info()")
    compare_pos = refresh.index("std::memcmp(")
    sha_pos = refresh.index("sha256_headers(xbe)")
    if not (get_pos < compare_pos < sha_pos):
        raise AssertionError("XBE exact-byte comparison is not between full read and SHA rebuild")
    unchanged = refresh[refresh.index("if (headers_unchanged)") : refresh.index(
        "m_loaded_xbe_headers.assign", refresh.index("if (headers_unchanged)"))]
    if "sha256_headers" in unchanged or "title_name_from_xbe" in unchanged:
        raise AssertionError("unchanged-header fast path still rebuilds derived XBE identity")
    if ("if (refresh_disc)" not in unchanged or
        "RefreshDisc(false);" not in unchanged or "return;" not in unchanged):
        raise AssertionError("unchanged XBE fast path stopped conditionally polling mounted-disc identity")

    # Failed derived work (allocation/conversion/hash failure) may not poison the
    # cache forever; only a fully derived prior state may use the skip path.
    if "title_name_valid && !next.header_sha256.empty()" not in refresh:
        raise AssertionError("derived-cache validity does not retry failed title/SHA work")

    # The generic XBE reader keeps its historical complete read and parsing path,
    # but no longer frees+mallocs the same scratch buffer every successful poll.
    xbe_fn = extract_function(xbe, "struct xbe *xemu_get_xbe_info(void)")
    for needle in (
        "static uint32_t headers_capacity;",
        "headers_capacity < xbe.headers_len",
        "realloc(xbe.headers, xbe.headers_len)",
        "virt_dma_memory_read(hdr_addr_virt,",
        "if (bytes_read != xbe.headers_len)",
        "offsetof(struct xbe_header, m_sizeof_headers)",
        "xbe.headers_len > 8*TARGET_PAGE_SIZE",
        "sizeof(struct xbe_certificate)",
    ):
        if needle not in xbe_fn:
            raise AssertionError(f"reusable XBE scratch/full-read invariant missing: {needle}")
    old_free_block = "if (xbe.headers) {\n        free(xbe.headers);\n        xbe.headers = NULL;\n    }"
    if old_free_block in xbe_fn:
        raise AssertionError("xemu_get_xbe_info still frees its header scratch buffer every poll")

    # Pass 5 adds private cache state only; Current Game's public API and every
    # other member body (except the separately guarded v1.99 detached Draw UI
    # and v2.01 detached-font DrawGameInfoTab UI) remains byte-for-byte v1.89.
    public_header = hh[: hh.index("private:")]
    # v1.99 only adds the detached-render flag to Draw; normalize that QoL-only
    # signature so Pass 5 continues protecting the rest of the public API.
    public_header = public_header.replace("void Draw(bool detached = false);", "void Draw();")
    public_header = public_header.replace("    void RefreshRunningXbe(bool force = false);\n", "")
    # Phase 11 removes four public accessors that had no production caller.
    # Reinsert their historical declarations only for this v1.90 API digest so
    # the old guard keeps freezing every other public-API byte.
    phase11_removed_accessors = """    const std::vector<std::string> &LoadedLabelPacks() const
    {
        return m_loaded_label_packs;
    }
"""
    public_header = public_header.replace(
        "    const XemuXdkLabels::Status &XdkStatus() const { return m_xdk_status; }\n",
        phase11_removed_accessors +
        "    const XemuXdkLabels::Status &XdkStatus() const { return m_xdk_status; }\n")
    public_header = public_header.replace(
        "    const XemuPdbLabels::Status &PdbStatus() const { return m_pdb_status; }\n",
        "    const std::string &LoadedMapPath() const { return m_loaded_map_path; }\n" +
        "    const XemuPdbLabels::Status &PdbStatus() const { return m_pdb_status; }\n")
    public_header = public_header.replace(
        "\n    std::string LabelRootDirectory() const;",
        "    const std::string &LoadedPdbPath() const { return m_loaded_pdb_path; }\n" +
        "    const XemuPdbLabels::Identity &XbePdbIdentity() const { return m_xbe_pdb_identity; }\n\n" +
        "    std::string LabelRootDirectory() const;")
    if hashlib.sha256(public_header.encode("utf-8")).hexdigest() != EXPECTED_CURRENT_GAME_PUBLIC_HEADER_SHA256:
        raise AssertionError("CurrentGameManager public API changed in Pass 5")
    for needle in (
        "std::vector<uint8_t> m_loaded_xbe_headers;",
        "bool m_loaded_xbe_derived_valid = false;",
    ):
        if needle not in hh[hh.index("private:") :]:
            raise AssertionError(f"private Pass-5 cache state missing: {needle}")

    count, digest = protected_current_game_digest(cc)
    if count != EXPECTED_CURRENT_GAME_METHOD_COUNT:
        raise AssertionError(
            f"protected CurrentGameManager method count changed: {count} != {EXPECTED_CURRENT_GAME_METHOD_COUNT}"
        )
    if digest != EXPECTED_CURRENT_GAME_METHOD_DIGEST:
        raise AssertionError("Current Game behavior/UI changed outside Refresh() in Pass 5")

    print("PASS: v1.90 exact-header polling/allocation streamlining invariants")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
