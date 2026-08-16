#!/usr/bin/env python3
"""Static invariants for the v1.72 XBE label UI/integration."""
from __future__ import annotations
import argparse
from pathlib import Path


def need(text: str, token: str, where: str) -> None:
    if token not in text:
        raise SystemExit(f"FAIL: missing {token!r} in {where}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    root = Path(ap.parse_args().root).resolve()
    dt = root / "ui/xui/debug-tools"
    meson = (dt / "meson.build").read_text()
    current = (dt / "current-game.cc").read_text()
    mem = (dt / "memory-tools.cc").read_text()
    labels = (dt / "xbe-labels.cc").read_text()

    need(meson, "'xbe-labels.cc'", "meson.build")
    need(current, "XemuXbeLabels::Build", "current-game.cc")
    need(mem, 'ImGui::Checkbox("Labels"', "memory-tools.cc")
    need(mem, 'ImGui::Button("LABELS")', "memory-tools.cc")
    need(mem, 'ImGui::Button("DUMP LABELS")', "memory-tools.cc")
    need(mem, "PrimaryLabelAt(row.virtual_address)", "memory-tools.cc")
    need(mem, "xemu_cheat_virtual_to_physical(label.virtual_address", "memory-tools.cc")
    need(mem, "UNMAPPED", "memory-tools.cc")
    need(labels, '"~" + candidate.label_stem', "xbe-labels.cc")
    need(labels, '"kernel_"', "xbe-labels.cc")
    need(labels, 'section.name != ".text"', "xbe-labels.cc")
    print("PASS: v1.72 XBE label UI/source invariants")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
