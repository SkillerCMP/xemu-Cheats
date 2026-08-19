#!/usr/bin/env python3
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


MEMORY_TOOLS_IMPLEMENTATION_FILES = (
    "memory-tools-internal.hh",
    "memory-tools.cc",
    "memory-tools-memory.cc",
    "memory-tools-search.cc",
    "memory-tools-debugger.cc",
    "memory-tools-inject.cc",
    "memory-tools-labels.cc",
    "memory-tools-dump.cc",
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
