#!/usr/bin/env python3
# v2.87 current regression ownership.
"""v2.87 current regression ownership: platform/build/test-infrastructure regression contracts.

Historical version labels below are provenance only; the retained contracts are
owned and executed by this v2.87 suite.
"""
from __future__ import annotations


# Preserved contract from v200-guest-pause-build-fix-golden.py
def check_v200_guest_pause_build_fix_golden() -> None:
    """Regression guard for guest-pause QEMU header ownership.

    v2.00 originally placed qemu/osdep.h in the header to satisfy runstate
    prerequisites. The v2.71 Windows filebuf hotfix deliberately supersedes that
    shape: QEMU's osdep header now belongs only to guest-pause-guard.cc so its
    Windows `close` macro cannot leak through the C++ interface into <fstream>.
    """

    import argparse
    from pathlib import Path


    def main() -> int:
        parser = argparse.ArgumentParser()
        parser.add_argument("--root", required=True)
        args = parser.parse_args()
        root = Path(args.root)
        debug = root / "ui/xui/debug-tools"
        hdr = (debug / "guest-pause-guard.hh").read_text(encoding="utf-8")
        impl = (debug / "guest-pause-guard.cc").read_text(encoding="utf-8")
        meson = (debug / "meson.build").read_text(encoding="utf-8")

        for forbidden in ('#include "qemu/osdep.h"', '#include "system/runstate.h"'):
            if forbidden in hdr:
                raise AssertionError(
                    "guest pause C++ header must not leak QEMU osdep/runstate headers"
                )

        osdep = '#include "qemu/osdep.h"'
        runstate = '#include "system/runstate.h"'
        if osdep not in impl or runstate not in impl:
            raise AssertionError("guest pause implementation must own qemu/osdep.h and runstate.h")
        if impl.index(osdep) > impl.index('#include "guest-pause-guard.hh"'):
            raise AssertionError("qemu/osdep.h must remain first in the implementation")
        if 'extern "C" {\n#include "system/runstate.h"\n}' not in impl:
            raise AssertionError("system/runstate.h must retain C linkage in the C++ implementation")
        if '#include <glib.h>' in hdr or '#include <glib.h>' in impl:
            raise AssertionError("do not paper over QEMU header prerequisites with a direct glib include")
        if "'guest-pause-guard.cc'," not in meson:
            raise AssertionError("out-of-line guest pause implementation must be compiled by Meson")

        print("v2.00/v2.71 guest-pause header ownership guard: PASS")
        return 0
    result = main()
    if result not in (None, 0):
        raise AssertionError("v200-guest-pause-build-fix-golden.py returned non-zero: %r" % (result,))

# Preserved contract from v266-heavy-model-phase-split-golden.py
def check_v266_heavy_model_phase_split_golden() -> None:
    import argparse,pathlib
    ap=argparse.ArgumentParser(); ap.add_argument('--root',default='.'); root=pathlib.Path(ap.parse_args().root).resolve(); t=root/'ui/xui/debug-tools/tests'
    r=(t/'v287-run-regression-tests.py').read_text(); d=(t/'v287-debugger-streamlining-golden.py').read_text(); f=(t/'v287-f0-steady-state-golden.py').read_text()
    assert 'HEAVY_MODEL_TESTS' in r and '"v287-debugger-streamlining-golden.py"' in r and '"v287-f0-steady-state-golden.py"' in r
    assert '"--root", str(root), "--heavy"' in r
    assert 'randomized_iterations = 50_000 if args.heavy else 500' in d
    assert 'randomized_iterations = 100_000 if args.heavy else 1_000' in f
    print('PASS: v2.66 static keeps quick deterministic models while Heavy owns the full randomized iterations')

# Preserved contract from v267-regression-runner-qol-golden.py
def check_v267_regression_runner_qol_golden() -> None:
    import argparse,pathlib
    ap=argparse.ArgumentParser(); ap.add_argument('--root',default='.'); root=pathlib.Path(ap.parse_args().root).resolve(); r=(root/'ui/xui/debug-tools/tests/v287-run-regression-tests.py').read_text()
    for t in ('ThreadPoolExecutor(max_workers=jobs)','--jobs','time.perf_counter()','fatx-hdd-{compiler_index}.o','kernel-rpc-filesystem-{compiler_index}.o','-c",\n             str(debug_tools / "addons/hdd/fatx-hdd.cc")'):
        assert t in r,t
    assert 'kernel_rpc_fs_obj, kernel_rpc_fs_stream_obj, fatx_hdd_obj' in r
    assert 'kernel-rpc-filesystem-stream-{compiler_index}.o' in r
    print('PASS: v2.67 regression runner reports command timings, supports parallel static jobs, and reuses shared native objects')

# Preserved contract from v268-semantic-history-normalization-golden.py
def check_v268_semantic_history_normalization_golden() -> None:
    import argparse,hashlib,pathlib
    from v287_source_test_utils import strip_preentry_cheat_header_additions
    ap=argparse.ArgumentParser(); ap.add_argument('--root',default='.'); root=pathlib.Path(ap.parse_args().root).resolve(); h=(root/'ui/xui/debug-tools/cheat-engine.hh').read_text()
    stripped=strip_preentry_cheat_header_additions(h)
    expected='3580c3193a0bfbe0e3ffbf6fc72b8c46f92a21614759e6f73304b59a96fbb6c1'
    actual=hashlib.sha256(stripped.encode()).hexdigest()
    assert actual==expected,(actual,expected)
    utils=(root/'ui/xui/debug-tools/tests/v287_source_test_utils.py').read_text()
    assert 're.subn' in utils and '_strip_once' in utils
    print('PASS: v2.68 semantic PREENTRY header normalization reproduces the historical v1.94 header byte-for-byte')

# Preserved contract from v269-documentation-split-golden.py
def check_v269_documentation_split_golden() -> None:
    import argparse,pathlib
    ap=argparse.ArgumentParser(); ap.add_argument('--root',default='.'); root=pathlib.Path(ap.parse_args().root).resolve(); d=root/'ui/xui/debug-tools'
    r=(d/'README.md').read_text(); c=(d/'CHANGELOG.md').read_text()
    assert '## Release history' in r and 'CHANGELOG.md' in r
    assert '## RAW Cheat Engine and Patch lifecycle' in r and 'Create/Open -> Write -> Flush -> Close -> fresh FATX Verify' in r
    assert '## v2.53 Debug Tools additions audit / tab-state QoL' in c
    assert '## v0.1.65 private TFLAGS' in c
    assert '## v2.69 README / CHANGELOG documentation split' in c
    assert '## v2.53 Debug Tools additions audit / tab-state QoL' not in r
    print('PASS: v2.69 README is current-state documentation and CHANGELOG preserves release history')

# Preserved contract from v270-temporary-git-index-golden.py
def check_v270_temporary_git_index_golden() -> None:
    import argparse,hashlib,pathlib,stat,subprocess,sys,tempfile
    ap=argparse.ArgumentParser(); ap.add_argument('--root',default='.'); root=pathlib.Path(ap.parse_args().root).resolve()
    helper=root/'ui/xui/debug-tools/restore-executable-bits.py'
    text=helper.read_text()
    for t in ('--temporary-git-index','GIT_INDEX_FILE','temporary_index_environment','update_git_index'): assert t in text,t
    with tempfile.TemporaryDirectory(prefix='xemu-v270-git-') as td:
        repo=pathlib.Path(td); subprocess.run(['git','init','-q',str(repo)],check=True)
        f=repo/'script.py'; f.write_text('#!/usr/bin/env python3\nprint("ok")\n'); f.chmod(0o644)
        subprocess.run(['git','-C',str(repo),'add','script.py'],check=True)
        index=repo/'.git/index'; before=hashlib.sha256(index.read_bytes()).hexdigest()
        before_mode=subprocess.check_output(['git','-C',str(repo),'ls-files','-s','script.py'],text=True).split()[0]
        assert before_mode=='100644'
        subprocess.run([sys.executable,str(helper),'--root',str(repo),'--temporary-git-index'],check=True)
        after=hashlib.sha256(index.read_bytes()).hexdigest(); after_mode=subprocess.check_output(['git','-C',str(repo),'ls-files','-s','script.py'],text=True).split()[0]
        assert before==after and after_mode=='100644'
        assert f.stat().st_mode & stat.S_IXUSR
    print('PASS: v2.70 temporary-index validation repairs working-tree mode without mutating the real Git index')

# Preserved contract from v271-imgui-tab-style-stack-hotfix-golden.py
def check_v271_imgui_tab_style_stack_hotfix_golden() -> None:
    import argparse
    from pathlib import Path

    p = argparse.ArgumentParser()
    p.add_argument("--root", required=True)
    a = p.parse_args()
    d = Path(a.root) / "ui/xui/debug-tools"

    style = (d / "tab-style.hh").read_text()
    assert '#include <imgui.h>' in style
    assert '#include "../common.hh"' not in style
    for needle in (
        "~ScopedTabStyle()",
        "void Restore()",
        "if (!m_active)",
        "ImGui::PopStyleColor(5);",
        "m_active = false;",
        "bool m_active = true;",
    ):
        assert needle in style, needle

    files = (
        "current-game-ui.cc",
        "addons/memory-tools/memory-tools.cc",
        "cheat-engine-ui.cc",
        "addons/memory-tools/memory-tools-debugger-ui.cc",
        "addons/hdd/hdd-directory-ui.cc",
    )
    scoped_total = 0
    restore_total = 0
    for name in files:
        text = (d / name).read_text()
        scoped_total += text.count("XemuDebugUi::ScopedTabStyle tab_style;")
        restore_total += text.count("tab_style.Restore();")

        # Every styled tab bar must restore the colors immediately after its
        # BeginTabBar/EndTabBar scope, before drawing any surrounding UI/window.
        pos = 0
        while True:
            pos = text.find("XemuDebugUi::ScopedTabStyle tab_style;", pos)
            if pos < 0:
                break
            end = text.find("ImGui::EndTabBar();", pos)
            assert end >= 0, f"{name}: styled tab bar has no EndTabBar"
            restore = text.find("tab_style.Restore();", end)
            assert restore >= 0, f"{name}: styled tab bar has no Restore"
            boundaries = [
                p for marker in ("ImGui::End();", "ImGui::EndTable();")
                if (p := text.find(marker, end)) >= 0
            ]
            if boundaries:
                assert restore < min(boundaries), (
                    f"{name}: tab colors survive until an outer ImGui End"
                )
            pos = end + 1

    assert scoped_total == 6, scoped_total
    assert restore_total == scoped_total, (restore_total, scoped_total)
    print("v2.71 ImGui tab-style stack hotfix guard: PASS")

# Preserved contract from v271-targeted-format-pruning-golden.py
def check_v271_targeted_format_pruning_golden() -> None:
    import argparse,pathlib
    ap=argparse.ArgumentParser(); ap.add_argument('--root',default='.'); root=pathlib.Path(ap.parse_args().root).resolve(); d=root/'ui/xui/debug-tools'
    for path in d.rglob('*'):
        if not path.is_file() or path.suffix not in {'.cc','.hh','.py','.md'}:
            continue
        for number,line in enumerate(path.read_text(encoding='utf-8',errors='ignore').splitlines(),1):
            if line != line.rstrip():
                raise AssertionError(f'trailing whitespace: {path.relative_to(d)}:{number}')
    assert not any(p.name=='__pycache__' for p in d.rglob('__pycache__'))
    helper=(d/'restore-executable-bits.py').read_text(); assert 'real Git index' in helper and '--temporary-git-index exercises' in helper
    changelog=(d/'CHANGELOG.md').read_text(); assert '## v2.71 targeted formatting / pruning cleanup' in changelog and '## v2.70 temporary Git-index validation' in changelog
    print('PASS: v2.71 targeted Debug Tools formatting/pruning cleanup has no behavior-bearing changes')

# Preserved contract from v271-windows-filebuf-macro-hotfix-golden.py
def check_v271_windows_filebuf_macro_hotfix_golden() -> None:
    """v2.71 Windows filebuf/qemu_close_wrap compile-link hotfix guard."""

    from v287_source_test_utils import read_cheat_engine_implementation

    import argparse
    from pathlib import Path


    def main() -> int:
        ap = argparse.ArgumentParser()
        ap.add_argument("--root", default=".")
        root = Path(ap.parse_args().root).resolve()
        debug = root / "ui/xui/debug-tools"

        header = (debug / "guest-pause-guard.hh").read_text(encoding="utf-8")
        impl = (debug / "guest-pause-guard.cc").read_text(encoding="utf-8")
        cheat = read_cheat_engine_implementation(debug)
        meson = (debug / "meson.build").read_text(encoding="utf-8")
        native = (debug / "tests/v287-run-regression-tests.py").read_text(encoding="utf-8")

        assert '#include "qemu/osdep.h"' not in header
        assert '#include "system/runstate.h"' not in header
        assert '#include "qemu/osdep.h"' in impl
        assert '#include "guest-pause-guard.hh"' in impl
        assert impl.index('#include "qemu/osdep.h"') < impl.index('#include "guest-pause-guard.hh"')
        assert 'extern "C" {\n#include "system/runstate.h"\n}' in impl
        assert "'guest-pause-guard.cc'," in meson
        # Cheat Engine must not instantiate std::basic_filebuf at all: several UI
        # headers legitimately pull qemu/osdep.h/common.hh on Windows, where close is
        # a qemu_close_wrap macro. The bounded CMP-header probe uses stdio instead.
        assert '#include <fstream>' not in cheat
        assert 'std::ifstream' not in cheat
        assert 'g_fopen(path.c_str(), "rb")' in cheat
        assert 'std::fread(' in cheat and 'std::fclose(input)' in cheat
        assert 'guest-pause-fstream-order-golden' in native
        assert 'debug_tools / "guest-pause-guard.cc"' in native

        print("v2.71 Windows qemu_close_wrap/filebuf macro hotfix guard: PASS")
        return 0
    result = main()
    if result not in (None, 0):
        raise AssertionError("v271-windows-filebuf-macro-hotfix-golden.py returned non-zero: %r" % (result,))

# Current fork contract: reusable xemu build/release workflows remain untouched.
def check_upstream_workflow_preservation_golden() -> None:
    """Debug Tools preserves reusable upstream build/release workflows and limits build.sh to one redirect line."""
    from pathlib import Path
    import hashlib

    root = Path(__file__).resolve().parents[4]
    expected = {
        ".github/workflows/build.yml": "44ada01c457ecf5f0195e71b71db17a0df4700ceb5ef3f424ce5d3dc5e1d616e",
        ".github/workflows/build-windows.yml": "fa6e7bdb576d4f9491ee1a12da2c8394849a9ca1a6e7e12bb4b003a5c0f54604",
        ".github/workflows/build-linux.yml": "86bf7d1b97b42258e193e5a319ad58fbde01a94a29bace186d25014530969ea7",
        ".github/workflows/build-macos.yml": "21bed54422c484dd9dffb604c8e68985b0c3370113d963d92d4d50857d240aa9",
        ".github/workflows/release.yml": "1ebd3de33da42c27cceb5a40c57b1727eff2c3193dd31a2046a3bb83c42b025f",
    }
    texts = {}
    for rel, want in expected.items():
        path = root / rel
        got = hashlib.sha256(path.read_bytes()).hexdigest()
        assert got == want, (rel, got, want)
        texts[rel] = path.read_text(encoding="utf-8")

    build = texts[".github/workflows/build.yml"]
    windows = texts[".github/workflows/build-windows.yml"]
    linux = texts[".github/workflows/build-linux.yml"]
    macos = texts[".github/workflows/build-macos.yml"]
    assert "linux:" in build and "macos:" in build and "windows:" in build
    assert "arch: [x86_64, arm64]" in windows
    assert "configuration: [debug, release]" in windows
    assert "arch: [x86_64, aarch64]" in linux
    assert "arch: [x86_64, arm64]" in macos
    for rel, body in texts.items():
        assert "ui/xui/debug-tools" not in body, rel

    build_sh_path = root / "build.sh"
    build_sh = build_sh_path.read_text(encoding="utf-8")
    capstone = (root / "ui/xui/debug-tools/build-capstone.sh").read_text(encoding="utf-8")
    wrapper = (root / "ui/xui/debug-tools/build-capstone-windows.sh").read_text(encoding="utf-8")
    keystone = (root / "ui/xui/debug-tools/build-keystone.sh").read_text(encoding="utf-8")
    prepare = (root / "ui/xui/debug-tools/prepare-build-dependencies.sh").read_text(encoding="utf-8")
    build_wrapper = (root / "ui/xui/debug-tools/build-xemu.sh").read_text(encoding="utf-8")
    docker_driver = (root / "ui/xui/debug-tools/docker-build-windows.sh").read_text(encoding="utf-8")

    hook_line = b'[[ "${XEMU_DEBUG_TOOLS_BUILD_WRAPPED:-0}" == "1" || ! -f "${project_source_dir}/ui/xui/debug-tools/build-xemu.sh" ]] || exec bash "${project_source_dir}/ui/xui/debug-tools/build-xemu.sh" "$@"\n'
    build_bytes = build_sh_path.read_bytes()
    assert build_bytes.count(hook_line) == 1
    upstream_bytes = build_bytes.replace(hook_line, b"", 1)
    assert hashlib.sha256(upstream_bytes).hexdigest() == "b6ccae50441b2a953bce072b7e36e63a3dac401488c74ef20ceb2291c39a2b84"
    for forbidden in (
        "debug_tools_capstone_helper", "debug_tools_keystone_helper",
        "XEMU_DEBUG_TOOLS_SKIP_CAPSTONE_BOOTSTRAP",
        "XEMU_DEBUG_TOOLS_SKIP_KEYSTONE_BOOTSTRAP",
    ):
        assert forbidden not in build_sh, forbidden

    for token in (
        'build-capstone.sh', 'build-keystone.sh',
        'XEMU_CAPSTONE_PKG_CONFIG_PATH', 'XEMU_KEYSTONE_PKG_CONFIG_PATH',
        'PKG_CONFIG_PATH', 'XEMU_DEBUG_TOOLS_DEPENDENCIES_READY',
    ):
        assert token in prepare, token
    for token in (
        'prepare-build-dependencies.sh', '--enable-capstone',
        'exec bash "$project_root/build.sh"',
        'export XEMU_DEBUG_TOOLS_BUILD_WRAPPED=1',
        'XEMU_DEBUG_TOOLS_SKIP_DEPENDENCY_BOOTSTRAP',
    ):
        assert token in build_wrapper, token
    assert 'bash ./build.sh -p win64-cross' in docker_driver
    assert 'build-capstone-windows.sh' not in docker_driver
    assert 'build-xemu.sh -p win64-cross' not in docker_driver

    for token in (
        'win64-cross', 'Darwin', 'Linux', 'DEB_HOST_GNU_TYPE',
        'CMAKE_OSX_ARCHITECTURES', 'CAPSTONE_X86_SUPPORT=ON',
        'probe_capstone', 'CAPSTONE_PKG_CONFIG',
    ):
        assert token in capstone, token
    assert 'build-capstone.sh' in wrapper
    assert 'cmake -S' not in wrapper

    for token in (
        'win64-cross', 'Darwin', 'Linux', 'DEB_HOST_GNU_TYPE',
        'CMAKE_OSX_ARCHITECTURES', 'LLVM_TARGETS_TO_BUILD=X86',
        'probe_keystone', 'KEYSTONE_PKG_CONFIG',
        'KEYSTONE_SOURCE_SHA256',
        'KEYSTONE_PKGCONFIG_MIN_VERSION="${KEYSTONE_PKGCONFIG_MIN_VERSION:-0.9}"',
        'pkg-config reports Keystone version:',
        'patch_windows_keystone_pkgconfig',
        'smoke_output="${smoke_dir}/keystone-smoke"',
        'smoke_output="${smoke_output}.exe"',
        'expected output is missing or empty:',
        'Libs.private: ',
        '-lshell32',
        '-lole32',
        '-luuid',
        'keystone_source_root',
        '$source_dir/src',
        'native CMake source root was not found',
        '#include <cstdint>',
        'cmake_minimum_required(VERSION 3.10.0)',
    ):
        assert token in keystone, token

    print("PASS: upstream workflows preserved; build.sh differs by one Debug Tools redirect line; dependency wrapper remains target-aware")

# Preserved contract from v2851-windows-clamp-include-hotfix-golden.py
def check_v2851_windows_clamp_include_hotfix_golden() -> None:
    """v2.85.1 Windows compile hotfix: std::clamp owns <algorithm>."""
    from pathlib import Path
    import argparse, hashlib

    EXPECTED_SHA256 = "b8a32d2995cd3371b8ad3490308a384987074da911e182a502a784c9ef9da4a4"

    def main():
        ap=argparse.ArgumentParser(); ap.add_argument("--root", default=".")
        root=Path(ap.parse_args().root).resolve()
        p=root/"ui/xui/debug-tools/addons/hdd/hdd-directory-ui.cc"
        text=p.read_text(encoding="utf-8")
        if "std::clamp(" not in text:
            raise AssertionError("HDD UI no longer exercises the guarded std::clamp path")
        if "#include <algorithm>" not in text:
            raise AssertionError("std::clamp requires explicit <algorithm> ownership in hdd-directory-ui.cc")
        got=hashlib.sha256(p.read_bytes()).hexdigest()
        if got != EXPECTED_SHA256:
            raise AssertionError(f"v2.85.1 HDD UI hotfix surface changed: {got}")
        print("PASS: v2.85.1 Windows HDD UI std::clamp compile dependency is explicitly owned by <algorithm>")
        return 0
    result = main()
    if result not in (None, 0):
        raise AssertionError("v2851-windows-clamp-include-hotfix-golden.py returned non-zero: %r" % (result,))

# Preserved contract from v2861-current-game-namespace-hotfix-golden.py
def check_v2861_current_game_namespace_hotfix_golden() -> None:
    """v2.86.1: Current Game anonymous-namespace Windows compile hotfix guard."""

    import argparse
    import pathlib


    def main() -> int:
        ap = argparse.ArgumentParser()
        ap.add_argument("--root", default=".")
        root = pathlib.Path(ap.parse_args().root).resolve()
        current = (root / "ui/xui/debug-tools/current-game.cc").read_text(encoding="utf-8")

        namespace_pos = current.find("namespace {")
        manager_pos = current.find("bool CurrentGameManager::SameIdentity")
        if namespace_pos < 0 or manager_pos < 0 or manager_pos <= namespace_pos:
            raise AssertionError("Current Game namespace/manager layout not found")

        helper_tail = (
            "static bool read_local_text_file(const std::string &path, uint64_t max_bytes,\n"
            "                                 std::string &out, std::string &error)\n"
            "{\n"
            "    return read_local_file(path, max_bytes, out, error);\n"
            "}\n\n"
            "}\n\n"
            "bool CurrentGameManager::SameIdentity"
        )
        if helper_tail not in current:
            raise AssertionError(
                "anonymous helper namespace must close before CurrentGameManager member definitions"
            )

        print("PASS: v2.86.1 Current Game anonymous-namespace Windows compile hotfix guard")
        return 0
    result = main()
    if result not in (None, 0):
        raise AssertionError("v2861-current-game-namespace-hotfix-golden.py returned non-zero: %r" % (result,))


def check_manual_only_github_actions_golden() -> None:
    """Fork-facing workflow entry points must never auto-run on repository events."""
    from pathlib import Path

    root = Path(__file__).resolve().parents[4]
    workflows = (
        ".github/workflows/build-xemu-win64-toolchain.yml",
        ".github/workflows/bump-subproject-wraps.yml",
        ".github/workflows/ci.yml",
        ".github/workflows/prerelease.yml",
        ".github/workflows/release-on-tag.yml",
        ".github/workflows/release-published.yml",
        ".github/workflows/release-on-dispatch.yml",
    )
    for rel in workflows:
        lines = (root / rel).read_text(encoding="utf-8").splitlines()
        start = next(i for i, line in enumerate(lines) if line == "on:") + 1
        trigger_lines = []
        for line in lines[start:]:
            if line and not line.startswith((" ", "\t", "#")):
                break
            trigger_lines.append(line)
        trigger = "\n".join(trigger_lines)
        assert "  workflow_dispatch:" in trigger, rel
        for forbidden in ("  push:", "  pull_request:", "  schedule:", "  release:"):
            assert forbidden not in trigger, (rel, forbidden)
    print("PASS: GitHub Actions entry points are manual-only")

CONTRACTS = (
    ('v200-guest-pause-build-fix-golden.py', check_v200_guest_pause_build_fix_golden),
    ('v266-heavy-model-phase-split-golden.py', check_v266_heavy_model_phase_split_golden),
    ('v267-regression-runner-qol-golden.py', check_v267_regression_runner_qol_golden),
    ('v268-semantic-history-normalization-golden.py', check_v268_semantic_history_normalization_golden),
    ('v269-documentation-split-golden.py', check_v269_documentation_split_golden),
    ('v270-temporary-git-index-golden.py', check_v270_temporary_git_index_golden),
    ('v271-imgui-tab-style-stack-hotfix-golden.py', check_v271_imgui_tab_style_stack_hotfix_golden),
    ('v271-targeted-format-pruning-golden.py', check_v271_targeted_format_pruning_golden),
    ('v271-windows-filebuf-macro-hotfix-golden.py', check_v271_windows_filebuf_macro_hotfix_golden),
    ('upstream-workflow-preservation-golden', check_upstream_workflow_preservation_golden),
    ('manual-only-github-actions-golden', check_manual_only_github_actions_golden),
    ('v2851-windows-clamp-include-hotfix-golden.py', check_v2851_windows_clamp_include_hotfix_golden),
    ('v2861-current-game-namespace-hotfix-golden.py', check_v2861_current_game_namespace_hotfix_golden),
)

def main() -> int:
    for legacy_name, check in CONTRACTS:
        try:
            check()
        except Exception as exc:
            raise AssertionError(f"v2.87 retained contract failed ({legacy_name}): {exc}") from exc
    print("PASS: v2.87 platform/build/test-infrastructure regression contracts")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
