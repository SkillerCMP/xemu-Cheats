#!/usr/bin/env python3
# v2.87 current regression ownership.
"""v2.91.5 modular Debug Tools addition/source-isolation guards."""
from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import sys


def need(text: str, needle: str, where: str) -> None:
    if needle not in text:
        raise AssertionError(f"{where}: missing {needle}")


def forbid(text: str, needle: str, where: str) -> None:
    if needle in text:
        raise AssertionError(f"{where}: unexpected {needle}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    root = pathlib.Path(parser.parse_args().root).resolve()
    dt = root / "ui/xui/debug-tools"

    main_cc = (root / "ui/xui/main.cc").read_text(encoding="utf-8")
    menu = (root / "ui/xui/menubar.cc").read_text(encoding="utf-8")
    actions = (root / "ui/xui/actions.cc").read_text(encoding="utf-8")
    facade = (dt / "debug-tools.cc").read_text(encoding="utf-8")
    facade_h = (dt / "debug-tools.hh").read_text(encoding="utf-8")
    module_h = (dt / "debug-tools-module.hh").read_text(encoding="utf-8")
    detached = (dt / "detached-tools.cc").read_text(encoding="utf-8")
    current_ui = (dt / "current-game-ui.cc").read_text(encoding="utf-8")
    hdd_addon = (dt / "addons/hdd/debug-tools-hdd-addon.cc").read_text(encoding="utf-8")
    mem_addon = (dt / "addons/memory-tools/debug-tools-memory-tools-addon.cc").read_text(encoding="utf-8")
    meson = (dt / "meson.build").read_text(encoding="utf-8")
    docker_bat = (root / "Build-Xemu-DOCKER.bat").read_text(encoding="utf-8")
    docker_driver = (dt / "docker-build-windows.sh").read_text(encoding="utf-8")
    build_wrapper = (dt / "build-xemu.sh").read_text(encoding="utf-8")
    validator = (dt / "validate-project-layout.py").read_text(encoding="utf-8")
    root_build = (root / "build.sh").read_text(encoding="utf-8")

    # Upstream xui sees one facade only.
    for where, text in (("main.cc", main_cc), ("menubar.cc", menu), ("actions.cc", actions)):
        need(text, '#include "debug-tools/debug-tools.hh"', where)
        for feature_header in (
            'debug-tools/cheat-engine.hh',
            'debug-tools/current-game.hh',
            'debug-tools/hdd-directory.hh',
            'debug-tools/memory-tools.hh',
            'debug-tools/guest-kernel-rpc.hh',
            'debug-tools/addons/hdd/hdd-directory.hh',
            'debug-tools/addons/hdd/guest-kernel-rpc.hh',
            'debug-tools/addons/memory-tools/memory-tools.hh',
            'debug-tools/detached-tools.hh',
        ):
            forbid(text, feature_header, where)

    for needle in (
        "debug_tools_init(window, sdl_gl_context);",
        "debug_tools_cleanup();",
        "debug_tools_process_sdl_event(event)",
        "debug_tools_tick();",
        "debug_tools_build_detached_frames();",
        "debug_tools_render_detached_frames();",
    ):
        need(main_cc, needle, "main.cc facade routing")
    need(menu, "debug_tools_draw_menu_items();", "menubar facade routing")
    need(actions, "debug_tools_notify_game_reset();", "Reset facade routing")

    # Main addition stays owned by the facade; optional additions enter through
    # registration entry points and not xemu-facing source.
    for needle in (
        "RegisterMainAddition();",
        "debug_tools_register_hdd_addition();",
        "debug_tools_register_memory_tools_addition();",
        'debug_tools_register_menu_item(100, "Current Game"',
        'debug_tools_register_menu_item(300, "Cheat Engine"',
        "debug_tools_register_tick(200, RefreshCurrentGame);",
        "debug_tools_register_tick(300, TickCheatEngine);",
        "debug_tools_register_reset(200, NotifyCheatEngineReset);",
    ):
        need(facade, needle, "Debug Tools facade")

    # Current Game exposes an extension slot and has no HDD/Kernel-RPC include.
    need(current_ui, "debug_tools_draw_current_game_extension_tabs(", "Current Game extension tabs")
    need(current_ui, "debug_tools_draw_current_game_extension_footer();", "Current Game extension footer")
    forbid(current_ui, '#include "hdd-directory.hh"', "Current Game dependency isolation")
    forbid(current_ui, '#include "guest-kernel-rpc.hh"', "Current Game dependency isolation")

    # Detached host is feature-neutral.
    need(detached, "void detached_tools_register(", "detached registration API")
    need(detached, "std::vector<DetachedToolWindow> g_tools;", "dynamic detached registry")
    for direct in ('#include "cheat-engine.hh"', '#include "current-game.hh"',
                   '#include "hdd-directory.hh"', '#include "memory-tools.hh"'):
        forbid(detached, direct, "detached feature isolation")

    # HDD addition owns all of its former external hooks.
    for needle in (
        'debug_tools_register_menu_item(200, "HDD Directory"',
        "debug_tools_register_tick(100, TickHddAddition);",
        "debug_tools_register_current_game_extension(",
        'ImGui::BeginTabItem("HDD")',
        'ImGui::BeginTabItem("Kernel RPC Diagnostics")',
        "guest_kernel_rpc_manager.Tick();",
        '"xemu - Xbox HDD Directory"',
    ):
        need(hdd_addon, needle, "HDD addition")

    # Memory addition owns the viewer menu/window and debugger Reset callback.
    for needle in (
        'debug_tools_register_menu_item(400, "Memory Viewer / Search"',
        "debug_tools_register_reset(100, NotifyMemoryToolsReset);",
        "xemu_memory_tools_notify_game_reset();",
        '"xemu - Memory Viewer / Search / x86 Debugger"',
    ):
        need(mem_addon, needle, "Memory addition")

    # Local profile controls source inclusion; disabled additions compile stubs.
    for needle in (
        "debug_tools_profile == 'main+hdd'",
        "debug_tools_profile == 'main+memory'",
        "if debug_tools_with_hdd",
        "if debug_tools_with_memory",
        "addons/stubs/debug-tools-hdd-addon-stub.cc",
        "addons/stubs/debug-tools-memory-tools-addon-stub.cc",
        "addons/hdd/debug-tools-hdd-addon.cc",
        "addons/hdd/hdd-directory.cc",
        "addons/hdd/guest-kernel-rpc.cc",
        "addons/memory-tools/debug-tools-memory-tools-addon.cc",
        "addons/memory-tools/memory-tools.cc",
        "addons/memory-tools/breakpoint-conditions.cc",
        "debug_tools_root_include",
    ):
        need(meson, needle, "modular meson profile")

    # v2.91.5 makes the modular split physical as well as compile-time: optional
    # implementations must live below their addition directory, not in the main
    # Debug Tools root.
    root_files = {path.name for path in dt.iterdir() if path.is_file()}
    for leaked in (
        "hdd-directory.cc", "hdd-directory.hh", "fatx-hdd.cc",
        "guest-kernel-rpc.cc", "kernel-rpc-filesystem.cc",
        "memory-tools.cc", "memory-tools.hh", "breakpoint-conditions.cc",
        "register-copy-utils.hh",
    ):
        if leaked in root_files:
            raise AssertionError(f"optional addition leaked into Debug Tools root: {leaked}")
    for required in (
        dt / "addons/hdd/hdd-directory.cc",
        dt / "addons/hdd/guest-kernel-rpc.cc",
        dt / "addons/hdd/kernel-rpc-filesystem.cc",
        dt / "addons/memory-tools/memory-tools.cc",
        dt / "addons/memory-tools/memory-tools-debugger.cc",
        dt / "addons/memory-tools/breakpoint-conditions.cc",
    ):
        if not required.is_file():
            raise AssertionError(f"physically separated addition file missing: {required}")

    # Verify all advertised profile spellings through the same reader Meson uses.
    # The source-file checks explicitly clear the Docker/CI override, then the
    # second pass proves XEMU_DEBUG_TOOLS_PROFILE overrides the file without
    # modifying the source tree.
    reader = dt / "read-build-profile.py"
    profile = dt / "build-profile.txt"
    original = profile.read_text(encoding="utf-8")
    clean_env = os.environ.copy()
    clean_env.pop("XEMU_DEBUG_TOOLS_PROFILE", None)
    try:
        for value in ("main", "main+hdd", "main+memory", "full"):
            profile.write_text(value + "\n", encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(reader), str(profile)],
                text=True, capture_output=True, check=True, env=clean_env,
            )
            if result.stdout.strip() != value:
                raise AssertionError(f"profile reader changed {value!r}: {result.stdout!r}")

        profile.write_text("full\n", encoding="utf-8")
        for value in ("main", "main+hdd", "main+memory", "full"):
            override_env = clean_env.copy()
            override_env["XEMU_DEBUG_TOOLS_PROFILE"] = value
            result = subprocess.run(
                [sys.executable, str(reader), str(profile)],
                text=True, capture_output=True, check=True, env=override_env,
            )
            if result.stdout.strip() != value:
                raise AssertionError(
                    f"profile environment override changed {value!r}: {result.stdout!r}"
                )
    finally:
        profile.write_text(original, encoding="utf-8")

    # Docker builder can choose any modular profile without rewriting the source
    # profile file; the selection is passed through the environment into Meson.
    for needle in (
        "Select Debug Tools build profile:",
        "[1] main",
        "[2] main+hdd",
        "[3] main+memory",
        "[4] full",
        'call :select_debug_tools_profile "%~2"',
        '--env "XEMU_DEBUG_TOOLS_PROFILE=%DEBUG_TOOLS_PROFILE%"',
        "DEBUG_TOOLS_PROFILE.txt",
        "docker-build-windows.sh",
    ):
        need(docker_bat, needle, "Docker selectable profile")
    need(root_build, 'ui/xui/debug-tools/build-xemu.sh', "root one-line Debug Tools build hook")
    need(root_build, 'XEMU_DEBUG_TOOLS_BUILD_WRAPPED', "root build recursion guard")
    need(docker_driver, "bash ./build.sh -p win64-cross", "Docker normal xemu build routing")
    for needle in (
        'TEST_DIR=./ui/xui/debug-tools/tests',
        'if [[ -d "$TEST_DIR" ]]',
        'Debug Tools tests detected; running regression suite.',
        'optional regression tests skipped.',
    ):
        need(docker_driver, needle, "optional Docker regression-test package")
    for needle in (
        '$OUT/source/${PKG_NAME}-source.zip',
        '$OUT/source/${PKG_NAME}.tar.zst',
        'zipfile.ZipFile',
        'source-zip.sha256',
        "printf '%s\\n' \"$XEMU_DEBUG_TOOLS_PROFILE\" > ./ui/xui/debug-tools/build-profile.txt",
        'git add -- "$helper"',
    ):
        need(docker_driver, needle, "Docker source-package output")
    need(docker_bat, r'echo  Source  : %RUN_ROOT%\source', "Docker source-package summary")

    for needle in (
        'TESTS_DIR = "ui/xui/debug-tools/tests"',
        'tests_present = tests_dir.is_dir()',
        'if tests_present:',
        'Optional regression tests: absent; build is allowed',
    ):
        need(validator, needle, "optional project-layout regression-test package")
    need(build_wrapper, "prepare-build-dependencies.sh", "Debug Tools dependency routing")
    need(build_wrapper, 'export XEMU_DEBUG_TOOLS_BUILD_WRAPPED=1', "Debug Tools wrapper recursion guard")
    need(build_wrapper, 'exec bash "$project_root/build.sh"', "xemu build.sh handoff")

    # Public integration header intentionally exposes no feature globals/classes.
    for direct in ("CheatEngineWindow", "CurrentGameManager", "HddDirectoryWindow", "MemoryToolsWindow"):
        forbid(facade_h, direct, "public facade isolation")
    need(module_h, "debug_tools_register_current_game_extension", "addition extension API")

    print("PASS: v2.91.5 modular additions + physical add-on ownership + source ZIP output + optional tests + selectable Docker profiles + one-line xemu build hook")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
