#!/usr/bin/env python3
# v2.87 current regression ownership.
"""Shared source-inspection helpers for Debug Tools golden tests."""
from __future__ import annotations

import re


def extract_function(text: str, signature: str) -> str:
    """Return a brace-balanced C/C++ function beginning at *signature*."""
    start = text.find(signature)
    if start < 0:
        raise AssertionError(f"missing function signature: {signature}")
    brace = text.find("{", start)
    if brace < 0:
        raise AssertionError(f"function opening brace missing: {signature}")
    depth = 0
    for pos in range(brace, len(text)):
        ch = text[pos]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return text[start : pos + 1]
    raise AssertionError(f"function closing brace missing: {signature}")


def extract_member_functions(text: str, class_name: str) -> list[tuple[str, str]]:
    """Return brace-balanced definitions for *class_name* member functions."""
    pattern = re.compile(
        rf"(?m)^([^\n]*?{re.escape(class_name)}::([A-Za-z_~][A-Za-z0-9_]*)"
        r"\s*\([^;]*?\)\s*(?:const\s*)?\n?\{)"
    )
    functions: list[tuple[str, str]] = []
    for match in pattern.finditer(text):
        brace = text.find("{", match.start())
        depth = 0
        for pos in range(brace, len(text)):
            if text[pos] == "{":
                depth += 1
            elif text[pos] == "}":
                depth -= 1
                if depth == 0:
                    functions.append((match.group(2), text[match.start() : pos + 1]))
                    break
        else:
            raise AssertionError(
                f"unterminated {class_name} function: {match.group(2)}"
            )
    return functions


CHEAT_ENGINE_IMPLEMENTATION_FILES = (
    "cheat-engine.cc",
    "cheat-engine-source.cc",
    "cheat-engine-fhooks.cc",
    "cheat-engine-execute.cc",
    "cheat-engine-ui.cc",
)


def read_cheat_engine_implementation(debug_dir) -> str:
    """Return the complete split CheatEngineWindow implementation as one view.

    Phase-2 moved UI/frontend methods into a dedicated translation unit; Phase-9
    further separates source/PREENTRY, F-hook lifecycle, and RAW execution while
    preserving method bodies byte-for-byte. Source-level golden tests should
    follow implementation ownership rather than assume an older monolith.
    """
    return "\n\n".join(
        (debug_dir / name).read_text(encoding="utf-8")
        for name in CHEAT_ENGINE_IMPLEMENTATION_FILES
    )


MEMORY_TOOLS_IMPLEMENTATION_FILES = (
    "addons/memory-tools/memory-tools-internal.hh",
    "addons/memory-tools/memory-tools.cc",
    "addons/memory-tools/memory-tools-memory.cc",
    "addons/memory-tools/memory-tools-memory-ui.cc",
    "addons/memory-tools/memory-tools-search.cc",
    "addons/memory-tools/memory-tools-search-ui.cc",
    "addons/memory-tools/memory-tools-debugger.cc",
    "addons/memory-tools/memory-tools-debugger-ui.cc",
    "addons/memory-tools/memory-tools-inject.cc",
    "addons/memory-tools/memory-tools-inject-ui.cc",
    "addons/memory-tools/memory-tools-labels.cc",
    "addons/memory-tools/memory-tools-labels-ui.cc",
    "addons/memory-tools/memory-tools-dump.cc",
    "addons/memory-tools/memory-tools-dump-ui.cc",
)


def read_memory_tools_implementation(debug_dir) -> str:
    """Return the complete split MemoryTools implementation as one source view.

    Source-level golden tests should validate behavior/ownership tokens rather
    than depend on which translation unit owns a moved method.
    """
    return "\n\n".join(
        (debug_dir / name).read_text(encoding="utf-8")
        for name in MEMORY_TOOLS_IMPLEMENTATION_FILES
    )

# PREENTRY/Patch intentionally adds state and declarations to the historical
# CheatEngine header. Older freeze tests remove these separately guarded
# additions semantically so harmless comments/formatting do not churn the
# historical fingerprints.
PREENTRY_CHEAT_HEADER_ADDITIONS = (
    "unordered_set include",
    "NotifyGameResetRequested declaration",
    "PreEntryLifecycle enum",
    "CheatBlock PREENTRY/identity fields",
    "PREENTRY window state",
    "PREENTRY helper declarations",
)


def _strip_once(text: str, pattern: str, description: str) -> str:
    text, count = re.subn(pattern, "", text, count=1, flags=re.MULTILINE | re.DOTALL)
    if count != 1:
        raise AssertionError(f"PREENTRY/Patch header scope changed unexpectedly: {description}")
    return text


def strip_preentry_cheat_header_additions(text: str) -> str:
    """Remove separately guarded PREENTRY/Patch header additions semantically."""
    text = _strip_once(text, r'^#include <unordered_set>\n', "unordered_set include")
    text = _strip_once(text, r'^    void NotifyGameResetRequested\(\);\n',
                       "NotifyGameResetRequested declaration")
    text = _strip_once(
        text,
        r'^    enum class PreEntryLifecycle \{\n.*?^    \};\n\n',
        "PreEntryLifecycle enum",
    )

    # Fields added to CheatBlock by PREENTRY and its duplicate-safe identity.
    for field in (
        "bool preentry = false;",
        "bool preentry_applied = false;",
        "std::string preentry_error;",
        "std::string group_path;",
        "uint32_t identity_ordinal = 0;",
    ):
        text = _strip_once(text, rf'^        {re.escape(field)}\n', field)

    for field in (
        "bool m_seen_game_valid = false;",
        "bool m_force_forget_f_hooks_on_next_game_observation = false;",
        "PreEntryLifecycle m_preentry_lifecycle = PreEntryLifecycle::Idle;",
        "std::string m_last_preentry_message;",
        "std::string m_seen_game_identity;",
    ):
        text = _strip_once(text, rf'^    {re.escape(field)}\n', field)

    text = _strip_once(
        text,
        r'^    /\* Only selected PREENTRY identities.*?^    std::unordered_set<std::string> m_selected_preentry_keys;\n',
        "selected PREENTRY session state",
    )

    declarations = (
        r'void ObserveRequestedGameReset\(\);',
        r'void ApplySelectedPreEntryPatches\(\);',
        r'std::string BlockIdentityKey\(const CheatBlock &block\) const;',
        r'std::string PreEntrySelectionKey\(const CheatBlock &block\) const;',
        r'bool IsPreEntrySelected\(const CheatBlock &block\) const;',
        r'void RememberPreEntrySelection\(CheatBlock &block\);',
        r'void DrawPatchGroup\(int group_index\);',
        r'void DeactivateLiveFHooks\(\);',
        r'static bool ConsumePreEntryPrefix\(std::string &spec\);',
        r'void ForgetFHookOwnershipForNewGuest\(\);',
        r'void DrawPatch\(size_t block_index\);',
        r'void SetPatchGroupSelected\(int group_index, bool selected\);',
    )
    for declaration in declarations:
        text = _strip_once(text, rf'^    {declaration}\n', declaration)
    text = _strip_once(
        text,
        r'^    void CountPatchGroupSelection\(int group_index, size_t &selected,\n'
        r'^                                  size_t &total\) const;\n',
        "CountPatchGroupSelection declaration",
    )
    return text
