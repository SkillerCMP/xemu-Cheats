#!/usr/bin/env python3
"""Regression guard for v2.01 detached Current Game font-atlas fix."""
from __future__ import annotations

import argparse
from pathlib import Path

from source_test_utils import extract_function


def require(text: str, needle: str, message: str) -> None:
    if needle not in text:
        raise AssertionError(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    args = parser.parse_args()
    root = Path(args.root)
    debug = root / "ui/xui/debug-tools"

    current_cc = (debug / "current-game.cc").read_text(encoding="utf-8")
    current_hh = (debug / "current-game.hh").read_text(encoding="utf-8")
    detached = (debug / "detached-tools.cc").read_text(encoding="utf-8")

    require(current_hh, "void DrawGameInfoTab(bool detached);",
            "Game Info draw helper must know whether it is using a detached ImGui context")

    draw_info = extract_function(
        current_cc, "void CurrentGameManager::DrawGameInfoTab(bool detached)")
    require(draw_info,
            "const bool use_main_fixed_font = !detached && g_font_mgr.m_fixed_width_font != nullptr;",
            "main font atlas must only be used outside detached Current Game")
    require(draw_info, "if (use_main_fixed_font) {\n        ImGui::PushFont(g_font_mgr.m_fixed_width_font);",
            "fixed-width font push must be guarded by detached state")
    require(draw_info, "if (use_main_fixed_font) {\n        ImGui::PopFont();",
            "font pop must match the guarded push")

    draw = extract_function(current_cc, "void CurrentGameManager::Draw(bool detached)")
    require(draw, "DrawGameInfoTab(detached);",
            "Current Game must pass detached context state into Game Info rendering")

    # Detached windows intentionally own independent ImGui font atlases and GL
    # textures.  Current Game must not regress to pushing the main atlas font.
    require(detached, "io.Fonts->AddFontDefault();",
            "detached tools must keep their independent context-local font atlas")
    require(detached, "SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);",
            "detached GL contexts must remain intentionally unshared")

    print("PASS: v2.01 detached Current Game font-atlas guard")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
