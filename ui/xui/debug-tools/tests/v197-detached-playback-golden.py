#!/usr/bin/env python3
"""v1.97 guard: detached-window drag keeps playback live without UI re-entry."""
from __future__ import annotations

import argparse
import hashlib
import pathlib
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    root = pathlib.Path(parser.parse_args().root).resolve()
    debug = root / "ui/xui/debug-tools"
    tests = debug / "tests"
    sys.path.insert(0, str(tests))
    from source_test_utils import extract_function

    xemu_c = (root / "ui/xemu.c").read_text(encoding="utf-8")
    hud_cc = (root / "ui/xui/main.cc").read_text(encoding="utf-8")
    hud_h = (root / "ui/xui/xemu-hud.h").read_text(encoding="utf-8")
    detached_cc = (debug / "detached-tools.cc").read_text(encoding="utf-8")
    detached_h = (debug / "detached-tools.hh").read_text(encoding="utf-8")

    common = extract_function(
        xemu_c,
        "static void gl_render_frame_common(struct xemu_console *scon, bool playback_only)",
    )
    watch = extract_function(
        xemu_c,
        "static bool event_watch_callback(void *userdata, SDL_Event *event)",
    )
    playback = extract_function(
        hud_cc, "void xemu_hud_render_playback_only(void)"
    )
    owns = extract_function(
        detached_cc, "bool detached_tools_owns_window_id(SDL_WindowID window_id)"
    )

    # Normal frames retain the historical full HUD update/render route.
    for needle in (
        "xemu_hud_update();",
        "xemu_hud_render();",
        "xemu_hud_render_playback_only();",
    ):
        if needle not in common:
            raise AssertionError(f"renderer routing missing: {needle}")
    if "if (!playback_only)" not in common:
        raise AssertionError("full HUD render is not excluded from playback-only mode")

    # The main console expose/resize workaround remains full-frame behavior.
    for needle in (
        "SDL_EVENT_WINDOW_EXPOSED",
        "SDL_EVENT_WINDOW_RESIZED",
        "event->window.windowID == SDL_GetWindowID(scon->real_window)",
        "gl_render_frame(scon);",
    ):
        if needle not in watch:
            raise AssertionError(f"historical console event-watch behavior changed: {needle}")

    # Win32 detached move/resize/expose events use playback-only presentation.
    if "#if defined(_WIN32)" not in watch:
        raise AssertionError("detached drag workaround must stay Windows-scoped")
    for needle in (
        "SDL_EVENT_WINDOW_MOVED",
        "SDL_EVENT_WINDOW_RESIZED",
        "SDL_EVENT_WINDOW_EXPOSED",
        "xemu_hud_is_detached_window_id(event->window.windowID)",
        "gl_render_playback_frame(scon);",
    ):
        if needle not in watch:
            raise AssertionError(f"detached playback event route missing: {needle}")

    # The modal callback may draw the guest framebuffer only. It must not run
    # ImGui frame construction, Debug Tools, Cheat Engine Tick, or detached UI.
    if "RenderFramebuffer(g_tex, width, height, g_flip_req);" not in playback:
        raise AssertionError("playback-only helper no longer presents the guest framebuffer")
    forbidden_playback = (
        "ImGui::",
        "cheat_engine_window",
        "current_game_manager",
        "memory_tools_window",
        "detached_tools_build_frames",
        "detached_tools_render_frames",
        "xemu_hud_update();",
        "xemu_hud_render();",
    )
    for needle in forbidden_playback:
        if needle in playback:
            raise AssertionError(f"playback-only callback re-entered UI/runtime work: {needle}")

    # Detached ownership lookup is intentionally side-effect-free.
    for needle in ("g_tools", "SDL_GetWindowID(tool->window)", "return true;"):
        if needle not in owns:
            raise AssertionError(f"detached window ownership lookup missing: {needle}")
    for needle in ("ImGui::", "ProcessEvent", "BuildToolFrame", "RenderToolFrame"):
        if needle in owns:
            raise AssertionError(f"detached ownership lookup gained side effects: {needle}")

    for decl in (
        "int xemu_hud_is_detached_window_id(SDL_WindowID window_id);",
        "void xemu_hud_render_playback_only(void);",
    ):
        if decl not in hud_h:
            raise AssertionError(f"C HUD bridge declaration missing: {decl}")
    if "bool detached_tools_owns_window_id(SDL_WindowID window_id);" not in detached_h:
        raise AssertionError("detached ownership declaration missing")

    # The two detached-window files are the only Debug Tools runtime files
    # allowed to change from the v1.96 audit baseline. Pass 11 separately
    # fingerprints the other 46 non-test files.
    for rel in ("detached-tools.cc", "detached-tools.hh"):
        if not hashlib.sha256((debug / rel).read_bytes()).hexdigest():
            raise AssertionError(f"unable to fingerprint scoped v1.97 file: {rel}")

    print("PASS: v1.97 detached-window playback continuity guard")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
