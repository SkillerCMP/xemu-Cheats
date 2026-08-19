#!/usr/bin/env python3
"""Regression guard for v2.00 guest-pause header build fix."""
from __future__ import annotations

import argparse
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    args = parser.parse_args()
    root = Path(args.root)
    hdr = root / "ui/xui/debug-tools/guest-pause-guard.hh"
    text = hdr.read_text(encoding="utf-8")

    osdep = '#include "qemu/osdep.h"'
    runstate = '#include "system/runstate.h"'
    if osdep not in text or runstate not in text:
        raise AssertionError("guest pause guard must include qemu/osdep.h and system/runstate.h")
    if text.index(osdep) > text.index(runstate):
        raise AssertionError("qemu/osdep.h must precede system/runstate.h")
    if 'extern "C" {\n#include "system/runstate.h"\n}' not in text:
        raise AssertionError("system/runstate.h must retain C linkage in the C++ guard")
    if '#include <glib.h>' in text:
        raise AssertionError("do not paper over QEMU header prerequisites with a direct glib include")

    print("v2.00 guest-pause header build-fix guard: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
