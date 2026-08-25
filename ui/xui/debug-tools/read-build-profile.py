#!/usr/bin/env python3
"""Read and validate the Debug Tools build profile for Meson.

The normal source-tree default comes from build-profile.txt. Local/CI builders
may override it without editing the tree by setting XEMU_DEBUG_TOOLS_PROFILE.
"""
from __future__ import annotations

import os
import pathlib
import sys

VALID = {"main", "main+hdd", "main+memory", "full"}
ENV_NAME = "XEMU_DEBUG_TOOLS_PROFILE"


def validate(value: str, source: str) -> str:
    value = value.strip().lower()
    if value not in VALID:
        print(
            f"invalid Debug Tools profile {value!r} from {source}; expected one of: "
            + ", ".join(sorted(VALID)),
            file=sys.stderr,
        )
        raise SystemExit(2)
    return value


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: read-build-profile.py <build-profile.txt>", file=sys.stderr)
        return 2

    override = os.environ.get(ENV_NAME)
    if override is not None and override.strip():
        print(validate(override, ENV_NAME))
        return 0

    path = pathlib.Path(sys.argv[1])
    try:
        value = path.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"cannot read {path}: {exc}", file=sys.stderr)
        return 2

    print(validate(value, str(path)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
