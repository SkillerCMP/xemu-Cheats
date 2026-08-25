#!/usr/bin/env python3
# v2.87 current regression ownership.
"""v2.87 current regression ownership: PREENTRY/Patch/Cheat regression contracts.

Historical version labels below are provenance only; the retained contracts are
owned and executed by this v2.87 suite.
"""
from __future__ import annotations


# Preserved contract from v245-preentry-patch-tab-golden.py
def check_v245_preentry_patch_tab_golden() -> None:
    """v2.45 PREENTRY Patch-tab lifecycle/source regression guards."""

    import argparse
    import pathlib

    from v287_source_test_utils import read_cheat_engine_implementation


    def extract_function(text: str, signature: str) -> str:
        start = text.index(signature)
        brace = text.index("{", start)
        depth = 0
        for pos in range(brace, len(text)):
            ch = text[pos]
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return text[start : pos + 1]
        raise AssertionError(f"unterminated function: {signature}")


    def require(haystack: str, needle: str, label: str) -> None:
        if needle not in haystack:
            raise AssertionError(f"missing {label}: {needle}")


    def forbid(haystack: str, needle: str, label: str) -> None:
        if needle in haystack:
            raise AssertionError(f"unexpected {label}: {needle}")


    def main() -> int:
        parser = argparse.ArgumentParser()
        parser.add_argument("--root", default=".")
        args = parser.parse_args()
        root = pathlib.Path(args.root).resolve()

        cc = read_cheat_engine_implementation(root / "ui/xui/debug-tools")
        hh = (root / "ui/xui/debug-tools/cheat-engine.hh").read_text(encoding="utf-8")
        actions = (root / "ui/xui/actions.cc").read_text(encoding="utf-8")

        # v2.45 feature ownership retained: standalone PREENTRY remains explicit and
        # block-scoped, and the Patch UI stays separate from live Cheats controls.
        parse = extract_function(cc, "void CheatEngineWindow::ParseSource(")
        require(parse, 'Upper(trimmed) == kPreEntryPrefix', "standalone PREENTRY directive")
        require(parse, "bool next_block_preentry = false;", "one-block pending marker")
        require(parse, "block.preentry = next_block_preentry;", "next-block classification")
        require(parse, "next_block_preentry = false;", "marker consumption")
        require(parse, "current && current->codes.empty()", "post-name PREENTRY form")
        require(parse, "current->preentry = true;", "current empty-block classification")
        require(parse, "block.enabled = false;", "PREENTRY excluded from live enable state")

        require(hh, "m_selected_preentry_keys", "PREENTRY selected-key cache")
        identity = extract_function(cc, "std::string CheatEngineWindow::BlockIdentityKey(")
        for needle in ("m_loaded_path", "block.group_path", "block.name",
                       "block.identity_ordinal"):
            require(identity, needle, "stable per-file/group/ordinal patch key")
        key = extract_function(cc, "std::string CheatEngineWindow::PreEntrySelectionKey(")
        require(key, "return BlockIdentityKey(block);",
                "PREENTRY patch key delegates to stable identity")
        selected = extract_function(cc, "bool CheatEngineWindow::IsPreEntrySelected(")
        require(selected, "m_selected_preentry_keys.find(PreEntrySelectionKey(block))",
                "patch selection lookup")
        remember = extract_function(cc, "void CheatEngineWindow::RememberPreEntrySelection(")
        require(remember, "m_selected_preentry_keys.insert(key);",
                "selected patch persistence")
        require(remember, "m_selected_preentry_keys.erase(key);",
                "unselected patch pruning")

        draw_patch = extract_function(cc, "void CheatEngineWindow::DrawPatch(")
        require(draw_patch, "RememberPreEntrySelection(block);", "checkbox stages patch")
        forbid(draw_patch, "ExecuteBlock(", "immediate PREENTRY execution")

        # Reset notification still belongs to the tiny actions.cc bridge and must be
        # armed before QEMU receives the reset request. v2.46 separately guards the
        # corrected completion/retry mechanism.
        action_reset = extract_function(actions, "void ActionReset(")
        require(action_reset, "debug_tools_notify_game_reset();",
                "Debug Tools Reset facade")
        notify_pos = action_reset.index("debug_tools_notify_game_reset();")
        reset_pos = action_reset.index("qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);")
        if notify_pos >= reset_pos:
            raise AssertionError("PREENTRY reset notification facade must arm before reset request")
        facade = (root / "ui/xui/debug-tools/debug-tools.cc").read_text(encoding="utf-8")
        require(facade, "cheat_engine_window.NotifyGameResetRequested();",
                "PREENTRY reset callback remains Debug Tools-owned")
        require(facade, "debug_tools_register_reset(200, NotifyCheatEngineReset);",
                "PREENTRY reset callback registration")

        # Live-cheat behavior remains isolated from PREENTRY blocks.
        set_group = extract_function(cc, "void CheatEngineWindow::SetGroupSelected(")
        require(set_group, "!m_blocks[cheat].preentry", "live group filter")
        disable = extract_function(cc, "void CheatEngineWindow::DisableAllCheats(")
        require(disable, "DeactivateLiveFHooks();", "live-only hook teardown helper")
        require(disable, "if (block.preentry)", "PREENTRY selection isolation")
        live_hooks = extract_function(cc, "void CheatEngineWindow::DeactivateLiveFHooks(")
        require(live_hooks, "if (!has_preentry_blocks)", "historical no-PREENTRY fast path")
        require(live_hooks, "DeactivateAllFHooks();", "historical hook deactivation path")
        require(live_hooks, "!m_blocks[owner].preentry", "PREENTRY hook retention")
        tick = extract_function(cc, "void CheatEngineWindow::Tick(")
        require(tick, "owner_block.preentry", "PREENTRY F-hook lifecycle branch")
        require(tick, "? !owner_block.preentry_applied", "applied PREENTRY hook retention")
        require(tick, "!m_blocks[i].preentry && m_blocks[i].enabled",
                "live execution excludes PREENTRY")

        draw = extract_function(cc, "void CheatEngineWindow::Draw(bool detached)")
        require(draw, 'ImGui::BeginTabItem("Cheats")', "Cheats tab")
        require(draw, 'ImGui::BeginTabItem("Patch")', "Patch tab")
        require(draw, "Codes activated here require a game reset to fully activate",
                "Patch warning")
        require(draw_patch, "PreEntryStatusText(", "central Patch status renderer")
        require(draw_patch, "[ERROR - RESET REQUIRED]",
                "per-Patch error status extension")
        status_helper = extract_function(cc, "static const char *PreEntryStatusText(")
        for status in (
            "[ACTIVATED]",
            "[ACTIVATED - RESET REQUIRED]",
            "[DEACTIVATED - RESET REQUIRED]",
            "[INACTIVE]",
        ):
            require(status_helper, status, f"patch status {status}")

        print("PASS: v2.45 PREENTRY Patch tab ownership + live-cheat isolation")
        return 0
    result = main()
    if result not in (None, 0):
        raise AssertionError("v245-preentry-patch-tab-golden.py returned non-zero: %r" % (result,))

# Preserved contract from v246-preentry-reset-retry-inline-golden.py
def check_v246_preentry_reset_retry_inline_golden() -> None:
    """v2.46 PREENTRY inline syntax + durable reset/retry regression guards."""

    import argparse
    import pathlib

    from v287_source_test_utils import extract_function, read_cheat_engine_implementation


    def require(haystack: str, needle: str, label: str) -> None:
        if needle not in haystack:
            raise AssertionError(f"missing {label}: {needle}")


    def forbid(haystack: str, needle: str, label: str) -> None:
        if needle in haystack:
            raise AssertionError(f"unexpected {label}: {needle}")


    def main() -> int:
        parser = argparse.ArgumentParser()
        parser.add_argument("--root", default=".")
        root = pathlib.Path(parser.parse_args().root).resolve()
        debug = root / "ui/xui/debug-tools"
        cc = read_cheat_engine_implementation(debug)
        hh = (debug / "cheat-engine.hh").read_text(encoding="utf-8")
        actions = (root / "ui/xui/actions.cc").read_text(encoding="utf-8")

        # Exact user-facing inline syntax: +:PREENTRY:Name{description}.
        parse = extract_function(cc, "void CheatEngineWindow::ParseSource(")
        require(cc, 'static const std::string kPreEntryPrefix = ":PREENTRY:";',
                "inline PREENTRY prefix")
        consume = extract_function(cc, "bool CheatEngineWindow::ConsumePreEntryPrefix(")
        require(consume, "Upper(spec).rfind(kPreEntryPrefix, 0) != 0",
                "case-insensitive inline prefix match")
        require(consume, "spec = Trim(spec.substr(kPreEntryPrefix.size()));",
                "inline marker stripping from display name")
        require(parse, "ConsumePreEntryPrefix(spec)", "shared inline block classification")
        require(parse, "next_block_preentry = true;", "inline block classification")
        require(parse, 'Upper(trimmed) == kPreEntryPrefix',
                "standalone PREENTRY backward compatibility")
        sample = "+:PREENTRY:480p Wide - 64MB Baseline{Halo 2 HD test}"
        sample_spec = sample[1:].strip()
        if not sample_spec.upper().startswith(":PREENTRY:"):
            raise AssertionError("exact Halo 2 inline PREENTRY sample was not recognized")
        sample_label = sample_spec[len(":PREENTRY:"):].strip()
        if not sample_label.startswith("480p Wide - 64MB Baseline"):
            raise AssertionError("inline PREENTRY marker was not stripped from display name")

        # actions.cc stays the same tiny bridge from v2.45: no PREENTRY execution or
        # state machine is allowed to escape Debug Tools.
        action_reset = extract_function(actions, "void ActionReset(")
        require(action_reset, "debug_tools_notify_game_reset();",
                "reset notification facade")
        require(action_reset, "qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);",
                "QEMU reset request")
        facade = (root / "ui/xui/debug-tools/debug-tools.cc").read_text(encoding="utf-8")
        require(facade, "cheat_engine_window.NotifyGameResetRequested();",
                "PREENTRY reset callback")
        forbid(action_reset, "ApplySelectedPreEntryPatches", "PREENTRY execution in actions.cc")

        # Same-title reset completion no longer depends on XBE becoming null.
        notify = extract_function(cc, "void CheatEngineWindow::NotifyGameResetRequested(")
        require(notify, "m_preentry_lifecycle = PreEntryLifecycle::ResetRequested;",
                "reset arm")
        observe = extract_function(cc, "void CheatEngineWindow::ObserveRequestedGameReset(")
        require(observe, "qemu_reset_requested_get() != SHUTDOWN_CAUSE_NONE",
                "durable QEMU reset pending gate")
        require(observe, "qemu_shutdown_requested_get() != SHUTDOWN_CAUSE_NONE",
                "reset-to-shutdown guard")
        forbid(observe, "xemu_get_xbe_info", "transient XBE-null dependency")
        forbid(observe, "m_preentry_reset_observed_unloaded", "old unloaded-edge latch")
        require(observe, "m_preentry_lifecycle = PreEntryLifecycle::ApplyPending;",
                "post-reset apply arm")
        require(observe, "block.preentry_applied = false;", "post-reset applied-state clear")
        require(observe, "m_seen_game_generation = UINT64_MAX;", "same-title forced reload")

        # Startup arms once, but actual execution retries until both the file and CPU
        # are ready. It is independent of the Cheats-tab Enabled/Disabled button.
        auto_load = extract_function(cc, "void CheatEngineWindow::MaybeAutoLoadCurrentGame(")
        require(auto_load, "game_identity_changed",
                "startup boundary including direct valid-to-valid identity changes")
        require(auto_load, "!was_game_valid || game_identity != m_seen_game_identity",
                "invalid-to-valid plus valid-to-valid identity detection")
        require(auto_load, "PreEntryLifecycle::ApplyPending", "startup apply arm")
        apply = extract_function(cc, "void CheatEngineWindow::ApplySelectedPreEntryPatches(")
        require(apply, "m_preentry_lifecycle != PreEntryLifecycle::ApplyPending",
                "one-shot pending gate")
        require(apply, "!current_game_manager.HasGame() || m_loaded_path.empty() ||",
                "XBE/file readiness gate")
        require(apply, "!xemu_cheat_cpu_available()", "CPU readiness gate")
        require(apply, "if (!m_engine_enabled)", "global engine master gate")
        forbid(apply, "m_live_cheats_enabled", "Cheats-tab Enabled dependency")
        require(apply, "ExecuteBlock(i, block);", "PREENTRY execution")
        require(apply, "if (m_last_runtime_message.empty())", "success-state validation")
        require(apply, "block.preentry_applied = true;", "Activated latch after success")
        require(apply, "m_preentry_lifecycle = PreEntryLifecycle::Idle;",
                "single ready-window completion")

        tick = extract_function(cc, "void CheatEngineWindow::Tick(")
        cpu_gate = tick.index("if (!cpu_available)")
        apply_pos = tick.index("ApplySelectedPreEntryPatches();")
        live_pos = tick.index("const bool run_live_blocks")
        if not (cpu_gate < apply_pos < live_pos):
            raise AssertionError("PREENTRY must run after CPU readiness and before live cheats")
        require(hh, "enum class PreEntryLifecycle", "lifecycle enum")
        require(hh, "PreEntryLifecycle m_preentry_lifecycle = PreEntryLifecycle::Idle;",
                "lifecycle state member")
        forbid(hh, "m_preentry_reset_observed_unloaded", "removed transient reset member")

        print("PASS: v2.46 PREENTRY inline syntax + durable reset retry + live-button isolation")
        return 0
    result = main()
    if result not in (None, 0):
        raise AssertionError("v246-preentry-reset-retry-inline-golden.py returned non-zero: %r" % (result,))

# Preserved contract from v247-preentry-lifecycle-hardening-golden.py
def check_v247_preentry_lifecycle_hardening_golden() -> None:
    """v2.47 PREENTRY lifecycle/engine/autoload hardening guards."""

    import argparse
    import pathlib

    from v287_source_test_utils import extract_function, read_cheat_engine_implementation


    def require(text: str, needle: str, label: str) -> None:
        if needle not in text:
            raise AssertionError(f"missing {label}: {needle}")


    def forbid(text: str, needle: str, label: str) -> None:
        if needle in text:
            raise AssertionError(f"unexpected {label}: {needle}")


    def main() -> int:
        parser = argparse.ArgumentParser()
        parser.add_argument("--root", default=".")
        root = pathlib.Path(parser.parse_args().root).resolve()
        debug = root / "ui/xui/debug-tools"
        cc = read_cheat_engine_implementation(debug)
        hh = (debug / "cheat-engine.hh").read_text(encoding="utf-8")

        # Contradictory reset/apply boolean combinations are replaced by one state.
        require(hh, "enum class PreEntryLifecycle", "PREENTRY lifecycle enum")
        for state in ("Idle", "ResetRequested", "ApplyPending"):
            require(hh, state, f"lifecycle state {state}")
        forbid(hh, "m_preentry_reset_requested", "legacy reset boolean")
        forbid(hh, "m_preentry_apply_pending", "legacy apply boolean")

        notify = extract_function(cc, "void CheatEngineWindow::NotifyGameResetRequested(")
        require(notify, "PreEntryLifecycle::ResetRequested", "explicit reset state")
        observe = extract_function(cc, "void CheatEngineWindow::ObserveRequestedGameReset(")
        require(observe, "m_preentry_lifecycle != PreEntryLifecycle::ResetRequested",
                "reset-state gate")
        require(observe, "PreEntryLifecycle::ApplyPending", "post-reset pending state")
        require(observe, "PreEntryLifecycle::Idle", "shutdown cancellation state")

        auto_load = extract_function(cc, "void CheatEngineWindow::MaybeAutoLoadCurrentGame(")
        # Identity observation/ownership invalidation must happen before the Auto-load gate.
        generation_pos = auto_load.index("const uint64_t generation")
        gate_pos = auto_load.index("if (!m_auto_load_current_game)")
        seen_pos = auto_load.index("m_seen_game_valid = game_valid;")
        if not (generation_pos < seen_pos < gate_pos):
            raise AssertionError("Auto-load OFF must still observe Current Game identity")
        require(auto_load, "m_preentry_lifecycle == PreEntryLifecycle::Idle",
                "ordinary startup cannot compete with explicit reset")

        menu = extract_function(cc, "void CheatEngineWindow::DrawMenuBar(")
        require(menu, "LoadMatchingCurrentGameFile(false);",
                "mid-game Auto-load enable may load file without forcing startup")
        forbid(menu, "m_seen_game_generation = UINT64_MAX;",
               "synthetic startup generation on Auto-load enable")

        # Engine Enabled is a startup gate, not an applied Patch teardown command.
        require(menu, 'ImGui::MenuItem("Engine Enabled", nullptr, &m_engine_enabled);',
                "Engine Enabled UI")
        forbid(menu, "DeactivateAllFHooks();", "global Patch F-hook teardown")
        apply = extract_function(cc, "void CheatEngineWindow::ApplySelectedPreEntryPatches(")
        require(apply, "if (!m_engine_enabled)", "startup master gate")
        require(apply, "PreEntryLifecycle::Idle", "disabled-startup cancellation")
        forbid(apply, "m_live_cheats_enabled", "live Cheats button dependency")

        tick = extract_function(cc, "void CheatEngineWindow::Tick(")
        require(tick, "owner_block.preentry", "Patch/live F-hook ownership split")
        require(tick, "? !owner_block.preentry_applied", "applied Patch hook retention")

        print("PASS: v2.47 PREENTRY lifecycle + engine/autoload hardening")
        return 0
    result = main()
    if result not in (None, 0):
        raise AssertionError("v247-preentry-lifecycle-hardening-golden.py returned non-zero: %r" % (result,))

# Preserved contract from v248-preentry-parser-identity-golden.py
def check_v248_preentry_parser_identity_golden() -> None:
    """v2.48 PREENTRY parser/identity cleanup guards."""

    import argparse
    import pathlib

    from v287_source_test_utils import extract_function, read_cheat_engine_implementation


    def require(text: str, needle: str, label: str) -> None:
        if needle not in text:
            raise AssertionError(f"missing {label}: {needle}")


    def forbid(text: str, needle: str, label: str) -> None:
        if needle in text:
            raise AssertionError(f"unexpected {label}: {needle}")


    def main() -> int:
        parser = argparse.ArgumentParser()
        parser.add_argument("--root", default=".")
        root = pathlib.Path(parser.parse_args().root).resolve()
        debug = root / "ui/xui/debug-tools"
        cc = read_cheat_engine_implementation(debug)
        hh = (debug / "cheat-engine.hh").read_text(encoding="utf-8")

        # Inline syntax owns one parser helper instead of duplicating prefix logic.
        require(cc, 'static const std::string kPreEntryPrefix = ":PREENTRY:";',
                "shared PREENTRY marker")
        consume = extract_function(cc, "bool CheatEngineWindow::ConsumePreEntryPrefix(")
        require(consume, "Upper(spec).rfind(kPreEntryPrefix, 0) != 0",
                "case-insensitive prefix test")
        require(consume, "spec = Trim(spec.substr(kPreEntryPrefix.size()));",
                "marker stripping")
        parse = extract_function(cc, "void CheatEngineWindow::ParseSource(")
        require(parse, "ConsumePreEntryPrefix(spec)", "inline parser helper use")
        if parse.count("Upper(spec).rfind") != 0:
            raise AssertionError("inline PREENTRY prefix parsing was duplicated")

        # Malformed standalone PREENTRY after RAW lines is diagnosed and ignored.
        require(parse, "else if (current)", "misplaced directive branch")
        require(parse, "appears after RAW lines and was ignored", "parse diagnostic")
        require(parse, "place it before ", "corrective parse guidance")
        require(parse, "+Cheat or use +:PREENTRY:Name.", "inline corrective guidance")

        # Selection identity remains group-aware and is superseded by v2.58's
        # duplicate-safe occurrence ordinal, shared by live and PREENTRY state.
        require(hh, "std::string group_path;", "block group path identity")
        require(hh, "uint32_t identity_ordinal = 0;", "duplicate-safe identity ordinal")
        require(parse, "block.group_path += m_groups[(size_t)group_stack[depth]].name;",
                "group path construction")
        identity = extract_function(cc, "std::string CheatEngineWindow::BlockIdentityKey(")
        for needle in ("m_loaded_path", "block.group_path", "block.name",
                       "block.identity_ordinal"):
            require(identity, needle, "stable block identity component")
        key = extract_function(cc, "std::string CheatEngineWindow::PreEntrySelectionKey(")
        require(key, "return BlockIdentityKey(block);",
                "PREENTRY selection delegates to stable block identity")

        # v2.58 intentionally unified live reload preservation onto the same stable
        # identity while PREENTRY's selected-key cache remains authoritative.
        require(parse, "selected_by_identity", "live selected-state identity map")
        require(parse, "enabled_by_identity", "live enabled-state identity map")
        require(parse, "BlockIdentityKey(block)", "live state stable identity lookup")
        require(parse, "block.selected = IsPreEntrySelected(block);",
                "inline PREENTRY selected-key restore")
        require(parse, "current->selected = IsPreEntrySelected(*current);",
                "standalone PREENTRY selected-key restore")

        print("PASS: v2.48 PREENTRY parser + group-aware selection identity cleanup")
        return 0
    result = main()
    if result not in (None, 0):
        raise AssertionError("v248-preentry-parser-identity-golden.py returned non-zero: %r" % (result,))

# Preserved contract from v249-patch-ui-tab-qol-golden.py
def check_v249_patch_ui_tab_qol_golden() -> None:
    """v2.49 Patch status/tooltip + Debug Tools tab-visibility QoL guards."""

    import argparse
    import pathlib

    from v287_source_test_utils import extract_function, read_cheat_engine_implementation


    def require(text: str, needle: str, label: str) -> None:
        if needle not in text:
            raise AssertionError(f"missing {label}: {needle}")


    def main() -> int:
        parser = argparse.ArgumentParser()
        parser.add_argument("--root", default=".")
        root = pathlib.Path(parser.parse_args().root).resolve()
        debug = root / "ui/xui/debug-tools"
        cc = read_cheat_engine_implementation(debug)
        hh = (debug / "cheat-engine.hh").read_text(encoding="utf-8")
        style = (debug / "tab-style.hh").read_text(encoding="utf-8")

        # Three-state Debug Tools palette: inactive grey, hover steel blue, active green.
        require(style, "ImGui::GetStyleColorVec4(ImGuiCol_TabActive)",
                "current active green inheritance")
        require(style, "ImGuiCol_Tab,", "inactive tab override")
        require(style, "const ImVec4 inactive(0.58f, 0.58f, 0.58f, 0.95f)",
                "light-grey inactive tab")
        require(style, "const ImVec4 hovered(0.36f, 0.56f, 0.73f, 1.00f)",
                "steel-blue hovered tab")
        require(style, "ImGuiCol_TabHovered, hovered", "hover state owner")
        require(style, "ImGuiCol_TabUnfocused, inactive",
                "unfocused inactive reuses grey")
        require(style, "ImGuiCol_TabActive, active", "selected tab green")
        require(style, "ImGuiCol_TabUnfocusedActive, active", "selected unfocused green")
        require(style, "ImGui::PopStyleColor(5);", "balanced tab style stack")

        tab_owners = {
            "cheat-engine-ui.cc": ("##RawCheatEngineTabs",),
            "current-game-ui.cc": ("##current_game_tabs",),
            "addons/memory-tools/memory-tools.cc": ("memory_tools_tabs",),
            "addons/memory-tools/memory-tools-debugger-ui.cc": ("current_register_tabs",),
            "addons/hdd/hdd-directory-ui.cc": ("##current_game_hdd_tabs", "##hdd_partitions"),
        }
        for rel, bars in tab_owners.items():
            text = (debug / rel).read_text(encoding="utf-8")
            require(text, '#include "tab-style.hh"', f"{rel} tab-style include")
            for bar in bars:
                begin = f'ImGui::BeginTabBar("{bar}")'
                pos = text.index(begin)
                scoped = text.rfind("XemuDebugUi::ScopedTabStyle tab_style;", 0, pos)
                if scoped < 0:
                    raise AssertionError(f"{rel}:{bar} is not wrapped in scoped Debug Tools tab style")
            if "XemuDebugUi::PushTabStyle();" in text or "XemuDebugUi::PopTabStyle();" in text:
                raise AssertionError(f"{rel} returned to manual Debug Tools tab style ownership")

        # Status strings have one owner and Patch description hover follows the name.
        status = extract_function(cc, "static const char *PreEntryStatusText(")
        for label in (
            "[ACTIVATED]", "[ACTIVATED - RESET REQUIRED]",
            "[DEACTIVATED - RESET REQUIRED]", "[INACTIVE]",
        ):
            require(status, label, f"status {label}")
        draw_patch = extract_function(cc, "void CheatEngineWindow::DrawPatch(")
        require(draw_patch, "const bool name_hovered = ImGui::IsItemHovered();",
                "Patch name hover capture")
        require(draw_patch, "if (name_hovered &&", "tooltip bound to Patch name")
        require(draw_patch, "PreEntryStatusText(", "centralized status rendering")
        require(draw_patch, "[ERROR - RESET REQUIRED]",
                "per-Patch error status superseding normal lifecycle text")

        # Patch execution errors no longer overwrite the live-cheat runtime channel.
        require(hh, "std::string m_last_preentry_message;", "Patch error channel")
        apply = extract_function(cc, "void CheatEngineWindow::ApplySelectedPreEntryPatches(")
        require(apply, "std::string saved_live_message = std::move(m_last_runtime_message);",
                "live runtime preservation")
        require(apply, "m_last_preentry_message = std::move(first_error);",
                "Patch error capture")
        require(apply, "m_last_runtime_message = std::move(saved_live_message);",
                "live runtime restore")
        draw = extract_function(cc, "void CheatEngineWindow::Draw(bool detached)")
        require(draw, 'ImGui::TextWrapped("Patch: %s", m_last_preentry_message.c_str());',
                "Patch error UI")

        print("PASS: v2.49 Patch UI/status + grey/blue/green Debug Tools tab states")
        return 0
    result = main()
    if result not in (None, 0):
        raise AssertionError("v249-patch-ui-tab-qol-golden.py returned non-zero: %r" % (result,))

# Preserved contract from v250-preentry-typef-ownership-golden.py
def check_v250_preentry_typef_ownership_golden() -> None:
    """v2.50 PREENTRY/live Type-F ownership/resource cleanup guards."""

    import argparse
    import pathlib

    from v287_source_test_utils import extract_function, read_cheat_engine_implementation


    def require(text: str, needle: str, label: str) -> None:
        if needle not in text:
            raise AssertionError(f"missing {label}: {needle}")


    def forbid(text: str, needle: str, label: str) -> None:
        if needle in text:
            raise AssertionError(f"unexpected {label}: {needle}")


    def main() -> int:
        parser = argparse.ArgumentParser()
        parser.add_argument("--root", default=".")
        root = pathlib.Path(parser.parse_args().root).resolve()
        cc = read_cheat_engine_implementation(root / "ui/xui/debug-tools")
        hh = (root / "ui/xui/debug-tools/cheat-engine.hh").read_text(encoding="utf-8")

        require(hh, "void DeactivateLiveFHooks();", "live-only hook helper declaration")
        live = extract_function(cc, "void CheatEngineWindow::DeactivateLiveFHooks(")
        require(live, "if (!has_preentry_blocks)", "ordinary-file fast path")
        require(live, "DeactivateAllFHooks();", "ordinary-file historical teardown")
        require(live, "owner < m_blocks.size() && !m_blocks[owner].preentry",
                "live block ownership filter")
        require(live, "m_f_deactivate_scratch.clear();", "allocation-free scratch reuse")
        require(live, "DeactivateFHook(pending.second);", "tracked live-hook teardown")

        disable = extract_function(cc, "void CheatEngineWindow::DisableAllCheats(")
        require(disable, "DeactivateLiveFHooks();", "single live teardown owner")
        forbid(disable, "m_f_hooks", "duplicated hook-table ownership scan")

        require(hh, "void ForgetFHookOwnershipForNewGuest();",
                "new-guest ownership reset declaration")
        forget = extract_function(cc, "void CheatEngineWindow::ForgetFHookOwnershipForNewGuest(")
        for needle in (
            "m_f_hooks.clear();", "m_retired_f_hooks.clear();",
            "m_f_deactivate_scratch.clear();", "m_active_f_hooks_scratch.clear();",
            "InvalidateFTempBankCache();", "xemu_cheat_external_code_reset_allocations();",
        ):
            require(forget, needle, f"new-guest cleanup {needle}")
        forbid(forget, "DeactivateFHook(", "old-guest byte restoration")
        forbid(forget, "xemu_cheat_patch_virtual", "new-guest memory mutation")

        auto_load = extract_function(cc, "void CheatEngineWindow::MaybeAutoLoadCurrentGame(")
        require(auto_load, "ForgetFHookOwnershipForNewGuest();",
                "identity transition uses ownership reset helper")
        forbid(auto_load, "m_f_hooks.clear();", "duplicated identity ownership reset")

        tick = extract_function(cc, "void CheatEngineWindow::Tick(")
        require(tick, "owner_block.preentry", "steady-state Patch/live ownership distinction")
        require(tick, "? !owner_block.preentry_applied", "applied Patch F-hook retention")

        print("PASS: v2.50 PREENTRY/live Type-F ownership + stale-guest resource cleanup")
        return 0
    result = main()
    if result not in (None, 0):
        raise AssertionError("v250-preentry-typef-ownership-golden.py returned non-zero: %r" % (result,))

# Preserved contract from v251-preentry-reset-refresh-optimization-golden.py
def check_v251_preentry_reset_refresh_optimization_golden() -> None:
    """v2.51 PREENTRY reset/XBE-only refresh optimization guards."""

    import argparse
    import pathlib

    from v287_source_test_utils import extract_function, read_cheat_engine_implementation


    def require(text: str, needle: str, label: str) -> None:
        if needle not in text:
            raise AssertionError(f"missing {label}: {needle}")


    def forbid(text: str, needle: str, label: str) -> None:
        if needle in text:
            raise AssertionError(f"unexpected {label}: {needle}")


    def main() -> int:
        parser = argparse.ArgumentParser()
        parser.add_argument("--root", default=".")
        root = pathlib.Path(parser.parse_args().root).resolve()
        debug = root / "ui/xui/debug-tools"
        current = (debug / "current-game.cc").read_text(encoding="utf-8")
        current_hh = (debug / "current-game.hh").read_text(encoding="utf-8")
        cheat = read_cheat_engine_implementation(debug)

        require(current_hh, "void RefreshRunningXbe(bool force = false);",
                "narrow loaded-XBE refresh API")
        require(current_hh, "void RefreshInternal(bool force, bool refresh_disc);",
                "shared refresh implementation")

        full = extract_function(current, "void CurrentGameManager::Refresh(bool force)")
        require(full, "RefreshInternal(force, true);", "normal full refresh semantics")
        narrow = extract_function(current, "void CurrentGameManager::RefreshRunningXbe(bool force)")
        require(narrow, "RefreshInternal(force, false);", "XBE-only refresh semantics")
        forbid(narrow, "RefreshDisc", "disc scan in PREENTRY narrow wrapper")

        internal = extract_function(current, "void CurrentGameManager::RefreshInternal(")
        require(internal, "struct xbe *xbe = xemu_get_xbe_info();", "fresh loaded-XBE read")
        require(internal, "if (refresh_disc)", "disc refresh gate")
        for call in ("RefreshDisc(force);", "RefreshDisc(true);",
                     "RefreshDisc(false);", "RefreshDisc(force || game_changed);"):
            require(internal, call, f"normal refresh path {call}")

        observe = extract_function(cheat, "void CheatEngineWindow::ObserveRequestedGameReset(")
        require(observe, "current_game_manager.RefreshRunningXbe(true);",
                "PREENTRY reset uses XBE-only refresh")
        forbid(observe, "current_game_manager.Refresh(true);", "forced disc/label rescan on reset")

        print("PASS: v2.51 PREENTRY reset refresh narrowed to running XBE only")
        return 0
    result = main()
    if result not in (None, 0):
        raise AssertionError("v251-preentry-reset-refresh-optimization-golden.py returned non-zero: %r" % (result,))

# Preserved contract from v252-preentry-cleanup-pruning-golden.py
def check_v252_preentry_cleanup_pruning_golden() -> None:
    """v2.52 cumulative PREENTRY cleanup/pruning/containment guards."""

    import argparse
    import hashlib
    import pathlib

    from v287_source_test_utils import extract_function, read_cheat_engine_implementation


    def require(text: str, needle: str, label: str) -> None:
        if needle not in text:
            raise AssertionError(f"missing {label}: {needle}")


    def forbid(text: str, needle: str, label: str) -> None:
        if needle in text:
            raise AssertionError(f"unexpected {label}: {needle}")


    def main() -> int:
        parser = argparse.ArgumentParser()
        parser.add_argument("--root", default=".")
        root = pathlib.Path(parser.parse_args().root).resolve()
        debug = root / "ui/xui/debug-tools"
        cc = read_cheat_engine_implementation(debug)
        hh = (debug / "cheat-engine.hh").read_text(encoding="utf-8")
        utils = (debug / "tests/v287_source_test_utils.py").read_text(encoding="utf-8")
        readme = (debug / "README.md").read_text(encoding="utf-8")
        changelog = (debug / "CHANGELOG.md").read_text(encoding="utf-8")
        actions_path = root / "ui/xui/actions.cc"
        actions = actions_path.read_text(encoding="utf-8")

        # v2.88.2 adds one debugger-only reset notification before the existing
        # PREENTRY/QEMU Reset bridge. It may discard stale Changes UI ownership,
        # but PREENTRY staging/execution remains entirely Cheat Engine-owned.
        reset = extract_function(actions, "void ActionReset(")
        require(reset, "debug_tools_notify_game_reset();", "Debug Tools Reset facade")
        require(reset, "qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);", "QEMU reset request")
        if reset.index("debug_tools_notify_game_reset();") >= reset.index(
                "qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);"):
            raise AssertionError("Reset facade ordering changed")
        facade = (root / "ui/xui/debug-tools/debug-tools.cc").read_text(encoding="utf-8")
        memory_addon = (root / "ui/xui/debug-tools/addons/memory-tools/debug-tools-memory-tools-addon.cc").read_text(encoding="utf-8")
        require(memory_addon, "debug_tools_register_reset(100, NotifyMemoryToolsReset);",
                "Debugger Changes Reset registration")
        require(memory_addon, "xemu_memory_tools_notify_game_reset();",
                "Debugger Changes Reset callback")
        require(facade, "debug_tools_register_reset(200, NotifyCheatEngineReset);",
                "PREENTRY Reset registration after debugger Reset")
        require(facade, "cheat_engine_window.NotifyGameResetRequested();",
                "Debug Tools PREENTRY Reset callback")
        forbid(reset, "ApplySelectedPreEntryPatches", "PREENTRY execution outside Debug Tools")

        # Transitional reset state and stale parser-test names are decommissioned.
        for stale in (
            "m_preentry_reset_requested", "m_preentry_apply_pending",
            "m_preentry_reset_observed_unloaded", "V245_CHEAT_HEADER_ADDITIONS",
            "strip_v245_cheat_header_additions",
        ):
            forbid(cc + hh + utils, stale, f"decommissioned token {stale}")
        require(utils, "PREENTRY_CHEAT_HEADER_ADDITIONS", "durable PREENTRY header scope name")
        require(utils, "strip_preentry_cheat_header_additions", "durable PREENTRY strip helper")

        # No dead XBE include remains in Cheat Engine; QEMU reset state still stays local.
        forbid(cc, '#include "xemu-xbe.h"', "unused Cheat Engine XBE include")
        require(cc, '#include "system/runstate.h"', "reset-request lifecycle dependency")

        # Cumulative ownership/lifecycle contracts remain present.
        for needle in (
            "PreEntryLifecycle::ResetRequested", "PreEntryLifecycle::ApplyPending",
            "ConsumePreEntryPrefix(spec)", "block.group_path",
            "DeactivateLiveFHooks();", "ForgetFHookOwnershipForNewGuest();",
            "current_game_manager.RefreshRunningXbe(true);",
            "PreEntryStatusText(", "[ERROR - RESET REQUIRED]",
        ):
            require(cc, needle, f"cumulative PREENTRY contract {needle}")

        style = (debug / "tab-style.hh").read_text(encoding="utf-8")
        require(style, "ImGui::GetStyleColorVec4(ImGuiCol_TabActive)", "selected current-green tab")
        require(style, "const ImVec4 inactive(0.58f, 0.58f, 0.58f, 0.95f)",
                "unselected light-grey tab")
        require(style, "const ImVec4 hovered(0.36f, 0.56f, 0.73f, 1.00f)",
                "hovered steel-blue tab")

        require(readme, "CHANGELOG.md", "current manual release-history link")
        for version in range(47, 53):
            require(changelog, f"## v2.{version}", f"v2.{version} cleanup release notes")

        print("PASS: v2.52 cumulative PREENTRY cleanup/pruning + Debug Tools containment")
        return 0
    result = main()
    if result not in (None, 0):
        raise AssertionError("v252-preentry-cleanup-pruning-golden.py returned non-zero: %r" % (result,))

# Preserved contract from v252-preentry-prefix-compile-hotfix-golden.py
def check_v252_preentry_prefix_compile_hotfix_golden() -> None:
    """Compile-scope guard for the v2.52 PREENTRY parser hotfix."""
    from pathlib import Path
    import sys
    from v287_source_test_utils import read_cheat_engine_implementation

    ROOT = Path(__file__).resolve().parents[4]
    CC = read_cheat_engine_implementation(ROOT / "ui/xui/debug-tools")
    HH = (ROOT / "ui/xui/debug-tools/cheat-engine.hh").read_text(encoding="utf-8")


    def require(text, needle, label):
        if needle not in text:
            raise AssertionError(f"missing {label}: {needle}")


    require(HH, "static bool ConsumePreEntryPrefix(std::string &spec);",
            "CheatEngineWindow member declaration")
    require(CC, "bool CheatEngineWindow::ConsumePreEntryPrefix(std::string &spec)",
            "CheatEngineWindow member definition")
    require(CC, "if (Upper(spec).rfind(kPreEntryPrefix, 0) != 0)",
            "member Upper use")
    require(CC, "spec = Trim(spec.substr(kPreEntryPrefix.size()));",
            "member Trim use")
    if "static bool ConsumePreEntryPrefix(std::string &spec)" in CC:
        raise AssertionError("obsolete free helper remains and would not have member scope")

    print("PASS: v2.52 PREENTRY prefix helper has valid CheatEngineWindow member scope")

# Preserved contract from v253-debug-tools-additions-audit-golden.py
def check_v253_debug_tools_additions_audit_golden() -> None:
    """v2.53 Debug Tools additions audit + three-state tab QoL guards."""

    import argparse
    import hashlib
    import pathlib

    from v287_source_test_utils import extract_function, read_cheat_engine_implementation


    def require(text: str, needle: str, label: str) -> None:
        if needle not in text:
            raise AssertionError(f"missing {label}: {needle}")


    def forbid(text: str, needle: str, label: str) -> None:
        if needle in text:
            raise AssertionError(f"unexpected {label}: {needle}")


    def main() -> int:
        parser = argparse.ArgumentParser()
        parser.add_argument("--root", default=".")
        root = pathlib.Path(parser.parse_args().root).resolve()
        debug = root / "ui/xui/debug-tools"
        cc = read_cheat_engine_implementation(debug)
        hh = (debug / "cheat-engine.hh").read_text(encoding="utf-8")
        style = (debug / "tab-style.hh").read_text(encoding="utf-8")

        # v2.88.2 extends the Reset bridge by exactly one debugger-only Changes
        # notification. PREENTRY still owns its own reset lifecycle and remains
        # the only Cheat Engine work performed by ActionReset.
        actions = root / "ui/xui/actions.cc"
        reset = extract_function(actions.read_text(encoding="utf-8"), "void ActionReset(")
        require(reset, "debug_tools_notify_game_reset();", "v2.91 Reset facade")
        require(reset, "qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);",
                "v2.91 QEMU Reset bridge")
        facade = (root / "ui/xui/debug-tools/debug-tools.cc").read_text(encoding="utf-8")
        memory_addon = (root / "ui/xui/debug-tools/addons/memory-tools/debug-tools-memory-tools-addon.cc").read_text(encoding="utf-8")
        require(memory_addon, "xemu_memory_tools_notify_game_reset();",
                "v2.88.2 debugger-only Changes reset callback")
        require(facade, "cheat_engine_window.NotifyGameResetRequested();",
                "PREENTRY Reset callback")
        if "ApplySelectedPreEntryPatches" in reset:
            raise AssertionError("PREENTRY execution leaked into ActionReset")

        # Exactly three visual backgrounds for Debug Tools tabs. Window focus must
        # not introduce a fourth grey/green shade.
        require(style, "const ImVec4 inactive(0.58f, 0.58f, 0.58f, 0.95f)",
                "inactive light-grey tab")
        require(style, "const ImVec4 hovered(0.36f, 0.56f, 0.73f, 1.00f)",
                "hover steel-blue tab")
        require(style, "ImGui::GetStyleColorVec4(ImGuiCol_TabActive)",
                "selected current-green inheritance")
        require(style, "ImGuiCol_Tab, inactive", "focused inactive grey")
        require(style, "ImGuiCol_TabHovered, hovered", "hover blue")
        require(style, "ImGuiCol_TabUnfocused, inactive", "unfocused inactive grey")
        require(style, "ImGuiCol_TabActive, active", "focused selected green")
        require(style, "ImGuiCol_TabUnfocusedActive, active",
                "unfocused selected green")
        require(style, "ImGui::PopStyleColor(5);", "balanced five-color stack")
        forbid(style, "ImVec4(0.68f, 0.68f, 0.68f", "old grey hover state")
        forbid(style, "ImVec4(0.52f, 0.52f, 0.52f", "old fourth unfocused shade")

        # PREENTRY persistence stores selected identities only. False entries no
        # longer accumulate, and name-only live state cannot override Patch state.
        require(hh, "#include <unordered_set>", "selected-key container include")
        require(hh, "std::unordered_set<std::string> m_selected_preentry_keys;",
                "selected-key container")
        forbid(hh + cc, "m_preentry_selections", "obsolete boolean selection map")
        selected = extract_function(cc, "bool CheatEngineWindow::IsPreEntrySelected(")
        require(selected, "m_selected_preentry_keys.find(PreEntrySelectionKey(block))",
                "central selected-key lookup")
        remember = extract_function(cc, "void CheatEngineWindow::RememberPreEntrySelection(")
        require(remember, "m_selected_preentry_keys.insert(key);", "selected-key insert")
        require(remember, "m_selected_preentry_keys.erase(key);", "unselected-key prune")
        parse = extract_function(cc, "void CheatEngineWindow::ParseSource(")
        require(parse, "if (block.preentry) {\n                continue;\n            }",
                "PREENTRY excluded from legacy live state map")
        if parse.count("IsPreEntrySelected(") != 2:
            raise AssertionError("PREENTRY selected-key restoration must have exactly two parser owners")

        # Preserve the compile-hotfix ownership and the v2.46+ lifecycle isolation.
        require(hh, "static bool ConsumePreEntryPrefix(std::string &spec);",
                "member PREENTRY prefix declaration")
        require(cc, "bool CheatEngineWindow::ConsumePreEntryPrefix(std::string &spec)",
                "member PREENTRY prefix definition")
        apply = extract_function(cc, "void CheatEngineWindow::ApplySelectedPreEntryPatches(")
        forbid(apply, "m_live_cheats_enabled", "Cheats Enabled tie into Patch apply")
        require(apply, "m_engine_enabled", "global engine safety gate")

        print("PASS: v2.53 Debug Tools additions audit + grey/blue/green tab QoL")
        return 0
    result = main()
    if result not in (None, 0):
        raise AssertionError("v253-debug-tools-additions-audit-golden.py returned non-zero: %r" % (result,))

# Preserved contract from v254-game-identity-live-cheat-safety-golden.py
def check_v254_game_identity_live_cheat_safety_golden() -> None:
    import argparse, pathlib
    from v287_source_test_utils import extract_function, read_cheat_engine_implementation
    ap=argparse.ArgumentParser(); ap.add_argument('--root',default='.'); root=pathlib.Path(ap.parse_args().root).resolve()
    cc=read_cheat_engine_implementation(root/'ui/xui/debug-tools')
    fn=extract_function(cc,'void CheatEngineWindow::MaybeAutoLoadCurrentGame()')
    forget=fn.index('ForgetFHookOwnershipForNewGuest();')
    clear=fn.index('for (auto &block : m_blocks)')
    auto=fn.index('if (!m_auto_load_current_game)')
    assert forget < clear < auto
    assert 'if (!block.preentry)' in fn and 'block.enabled = false;' in fn and 'block.selected = false;' in fn
    assert 'm_switches.clear();' in fn and 'm_last_runtime_message.clear();' in fn
    assert 'm_selected_preentry_keys.clear' not in fn
    print('PASS: v2.54 game identity clears stale live-cheat execution without touching Patch selections')

# Preserved contract from v255-preentry-valid-to-valid-startup-golden.py
def check_v255_preentry_valid_to_valid_startup_golden() -> None:
    import argparse,pathlib
    from v287_source_test_utils import extract_function, read_cheat_engine_implementation
    ap=argparse.ArgumentParser(); ap.add_argument('--root',default='.'); root=pathlib.Path(ap.parse_args().root).resolve()
    h=(root/'ui/xui/debug-tools/cheat-engine.hh').read_text(); cc=read_cheat_engine_implementation(root/'ui/xui/debug-tools')
    assert 'std::string m_seen_game_identity;' in h
    fn=extract_function(cc,'void CheatEngineWindow::MaybeAutoLoadCurrentGame()')
    assert 'const bool game_identity_changed =' in fn
    assert 'game_identity != m_seen_game_identity' in fn
    assert 'm_seen_game_identity = game_identity;' in fn
    assert 'if (loaded && game_identity_changed &&' in fn
    assert 'm_preentry_lifecycle == PreEntryLifecycle::Idle' in fn
    print('PASS: v2.55 direct valid-to-valid Current Game identity changes arm PREENTRY exactly at a new-game boundary')

# Preserved contract from v256-cheat-file-discovery-optimization-golden.py
def check_v256_cheat_file_discovery_optimization_golden() -> None:
    import argparse,pathlib
    from v287_source_test_utils import extract_function, read_cheat_engine_implementation
    ap=argparse.ArgumentParser(); ap.add_argument('--root',default='.'); root=pathlib.Path(ap.parse_args().root).resolve()
    cc=read_cheat_engine_implementation(root/'ui/xui/debug-tools')
    fn=extract_function(cc,'bool CheatEngineWindow::LoadMatchingCurrentGameFile(bool force_reload)')
    assert 'kMaxHeaderBytes = 64u * 1024u' in fn
    assert 'g_fopen(path.c_str(), "rb")' in fn
    assert 'std::fread(' in fn and 'std::fclose(input)' in fn
    assert 'std::ifstream' not in fn
    assert 'g_file_get_contents' not in fn
    assert 'if (force_reload && !m_loaded_path.empty()' in fn
    assert 'read_header(m_loaded_path, loaded_header)' in fn
    assert 'preferred_names' in fn and 'fallback_names' in fn
    assert fn.index('consider_names(preferred_names);') < fn.index('consider_names(fallback_names);')
    assert 'ParseHeader(prefix, header)' in fn and 'HashMatches(header.hash, game.header_sha256)' in fn
    print('PASS: v2.56 matching reads bounded authoritative headers, checks current file first, and prioritizes filename candidates')

# Preserved contract from v257-deterministic-cheat-file-selection-golden.py
def check_v257_deterministic_cheat_file_selection_golden() -> None:
    import argparse,pathlib
    from v287_source_test_utils import extract_function, read_cheat_engine_implementation
    ap=argparse.ArgumentParser(); ap.add_argument('--root',default='.'); root=pathlib.Path(ap.parse_args().root).resolve()
    cc=read_cheat_engine_implementation(root/'ui/xui/debug-tools'); fn=extract_function(cc,'bool CheatEngineWindow::LoadMatchingCurrentGameFile(bool force_reload)')
    for token in ('size_t matching_count = 0;','g_ascii_strcasecmp(a.c_str(), b.c_str())','score == best_score && filename_before(filename, best_filename)','Multiple matching code files'):
        assert token in fn, token
    assert fn.index('score > best_score') < fn.index('best_filename = filename;')
    print('PASS: v2.57 matching-file ties use a deterministic filename order and report ambiguity')

# Preserved contract from v258-stable-block-identity-golden.py
def check_v258_stable_block_identity_golden() -> None:
    import argparse,pathlib
    from v287_source_test_utils import extract_function, read_cheat_engine_implementation
    ap=argparse.ArgumentParser(); ap.add_argument('--root',default='.'); root=pathlib.Path(ap.parse_args().root).resolve()
    h=(root/'ui/xui/debug-tools/cheat-engine.hh').read_text(); cc=read_cheat_engine_implementation(root/'ui/xui/debug-tools')
    assert 'uint32_t identity_ordinal = 0;' in h
    key=extract_function(cc,'std::string CheatEngineWindow::BlockIdentityKey(')
    assert 'block.group_path' in key and 'block.name' in key and 'block.identity_ordinal' in key
    pre=extract_function(cc,'std::string CheatEngineWindow::PreEntrySelectionKey('); assert 'return BlockIdentityKey(block);' in pre
    parse=extract_function(cc,'void CheatEngineWindow::ParseSource(bool preserve_states)')
    for token in ('occurrence_by_base','block.identity_ordinal = occurrence_by_base[occurrence_base]++;','selected_by_identity[BlockIdentityKey(block)]','enabled_by_identity[BlockIdentityKey(block)]'):
        assert token in parse, token
    assert 'selected_by_name' not in parse and 'enabled_by_name' not in parse
    print('PASS: v2.58 live and PREENTRY state use duplicate-safe file/group/name/ordinal identity')

# Preserved contract from v259-preentry-selection-pruning-golden.py
def check_v259_preentry_selection_pruning_golden() -> None:
    import argparse,pathlib
    from v287_source_test_utils import extract_function, read_cheat_engine_implementation
    ap=argparse.ArgumentParser(); ap.add_argument('--root',default='.'); root=pathlib.Path(ap.parse_args().root).resolve()
    cc=read_cheat_engine_implementation(root/'ui/xui/debug-tools'); fn=extract_function(cc,'void CheatEngineWindow::ParseSource(bool preserve_states)')
    for token in ('valid_preentry_keys','const std::string file_prefix = m_loaded_path + "\\n"','belongs_to_loaded_file','m_selected_preentry_keys.erase(it)'):
        assert token in fn, token
    assert fn.index('valid_preentry_keys.insert(PreEntrySelectionKey(block));') < fn.index('m_selected_preentry_keys.erase(it)')
    print('PASS: v2.59 stale PREENTRY keys are pruned only for the currently parsed code file')

# Preserved contract from v260-preentry-per-patch-errors-golden.py
def check_v260_preentry_per_patch_errors_golden() -> None:
    import argparse,pathlib
    from v287_source_test_utils import extract_function, read_cheat_engine_implementation
    ap=argparse.ArgumentParser(); ap.add_argument('--root',default='.'); root=pathlib.Path(ap.parse_args().root).resolve()
    h=(root/'ui/xui/debug-tools/cheat-engine.hh').read_text(); cc=read_cheat_engine_implementation(root/'ui/xui/debug-tools')
    assert 'std::string preentry_error;' in h
    apply=extract_function(cc,'void CheatEngineWindow::ApplySelectedPreEntryPatches()')
    for t in ('block.preentry_error.clear();','block.preentry_error = m_last_runtime_message;','first_error = block.name + ": " + block.preentry_error;'): assert t in apply,t
    draw=extract_function(cc,'void CheatEngineWindow::DrawPatch(size_t block_index)')
    assert '"[ERROR - RESET REQUIRED]"' in draw and 'Last Patch error:' in draw
    remember=extract_function(cc,'void CheatEngineWindow::RememberPreEntrySelection('); assert 'block.preentry_error.clear();' in remember
    print('PASS: v2.60 each failed PREENTRY block owns its diagnostic and error status')

# Preserved contract from v261-patch-lifecycle-summary-golden.py
def check_v261_patch_lifecycle_summary_golden() -> None:
    import argparse,pathlib
    from v287_source_test_utils import extract_function, read_cheat_engine_implementation
    ap=argparse.ArgumentParser(); ap.add_argument('--root',default='.'); root=pathlib.Path(ap.parse_args().root).resolve()
    cc=read_cheat_engine_implementation(root/'ui/xui/debug-tools'); draw=extract_function(cc,'void CheatEngineWindow::Draw(bool detached)')
    for t in ('patch_applied','patch_reset_required','patch_failed','Selected: %zu/%zu | Applied: %zu | Reset Required: %zu | Failed: %zu','patch_selected != 0 && !m_engine_enabled','selected startup Patches will not apply'):
        assert t in draw,t
    assert 'm_live_cheats_enabled' not in draw[draw.index('if (ImGui::BeginTabItem("Patch")'):]
    print('PASS: v2.61 Patch tab reports lifecycle counts and master-engine startup warning without live-toggle coupling')

# Preserved contract from v262-mixed-group-checkbox-golden.py
def check_v262_mixed_group_checkbox_golden() -> None:
    import argparse,pathlib
    from v287_source_test_utils import extract_function, read_cheat_engine_implementation
    ap=argparse.ArgumentParser(); ap.add_argument('--root',default='.'); root=pathlib.Path(ap.parse_args().root).resolve()
    d=root/'ui/xui/debug-tools'; helper=(d/'mixed-checkbox.hh').read_text(); cc=read_cheat_engine_implementation(d)
    for t in ('ImGui::Checkbox(label, value)','GetItemRectMin','GetWindowDrawList()->AddLine','ImGuiCol_CheckMark'): assert t in helper,t
    for sig in ('void CheatEngineWindow::DrawGroup(int group_index)','void CheatEngineWindow::DrawPatchGroup(int group_index)'):
        fn=extract_function(cc,sig); assert 'XemuDebugUi::MixedCheckbox' in fn; assert 'TextDisabled("[-]")' not in fn
    print('PASS: v2.62 partially selected Cheat/Patch groups render an indeterminate mark inside the checkbox')

# Preserved contract from v263-scoped-tab-style-golden.py
def check_v263_scoped_tab_style_golden() -> None:
    import argparse,pathlib
    ap=argparse.ArgumentParser(); ap.add_argument('--root',default='.'); root=pathlib.Path(ap.parse_args().root).resolve(); d=root/'ui/xui/debug-tools'
    h=(d/'tab-style.hh').read_text()
    for t in ('class ScopedTabStyle','~ScopedTabStyle()','ImGui::PopStyleColor(5);','ScopedTabStyle(const ScopedTabStyle &) = delete;'): assert t in h,t
    assert 'inline void PushTabStyle' not in h and 'inline void PopTabStyle' not in h
    for name in ('cheat-engine-ui.cc','current-game-ui.cc','addons/hdd/hdd-directory-ui.cc','addons/memory-tools/memory-tools-debugger-ui.cc','addons/memory-tools/memory-tools.cc'):
        s=(d/name).read_text(); assert 'PushTabStyle' not in s and 'PopTabStyle' not in s; assert 'ScopedTabStyle tab_style;' in s,name
    print('PASS: v2.63 Debug Tools tab colors use scoped RAII ownership with no manual Push/Pop pairs')

# Preserved contract from v271-preentry-selection-const-hotfix-golden.py
def check_v271_preentry_selection_const_hotfix_golden() -> None:
    """Guard the v2.71 PREENTRY selection const-correctness compile hotfix."""

    import argparse
    import pathlib

    from v287_source_test_utils import extract_function, read_cheat_engine_implementation


    def main() -> int:
        parser = argparse.ArgumentParser()
        parser.add_argument("--root", default=".")
        root = pathlib.Path(parser.parse_args().root).resolve()
        debug = root / "ui/xui/debug-tools"
        hh = (debug / "cheat-engine.hh").read_text(encoding="utf-8")
        cc = read_cheat_engine_implementation(debug)

        declaration = "void RememberPreEntrySelection(CheatBlock &block);"
        definition = "void CheatEngineWindow::RememberPreEntrySelection(CheatBlock &block)"
        stale_decl = "void RememberPreEntrySelection(const CheatBlock &block);"
        stale_def = "void CheatEngineWindow::RememberPreEntrySelection(const CheatBlock &block)"

        assert declaration in hh, "mutable PREENTRY selection declaration missing"
        assert definition in cc, "mutable PREENTRY selection definition missing"
        assert stale_decl not in hh and stale_def not in cc, "const compile regression returned"

        remember = extract_function(cc, definition)
        assert "block.preentry_error.clear();" in remember, "deselection must clear the block error"
        assert "m_selected_preentry_keys.erase(key);" in remember, "deselection must prune selection identity"

        set_group = extract_function(cc, "void CheatEngineWindow::SetPatchGroupSelected(")
        draw = extract_function(cc, "void CheatEngineWindow::DrawPatch(size_t block_index)")
        assert "RememberPreEntrySelection(m_blocks[cheat]);" in set_group
        assert "CheatBlock &block = m_blocks[block_index];" in draw
        assert "RememberPreEntrySelection(block);" in draw

        print("PASS: v2.71 PREENTRY selection uses mutable CheatBlock ownership for error clearing")
        return 0
    result = main()
    if result not in (None, 0):
        raise AssertionError("v271-preentry-selection-const-hotfix-golden.py returned non-zero: %r" % (result,))

CONTRACTS = (
    ('v245-preentry-patch-tab-golden.py', check_v245_preentry_patch_tab_golden),
    ('v246-preentry-reset-retry-inline-golden.py', check_v246_preentry_reset_retry_inline_golden),
    ('v247-preentry-lifecycle-hardening-golden.py', check_v247_preentry_lifecycle_hardening_golden),
    ('v248-preentry-parser-identity-golden.py', check_v248_preentry_parser_identity_golden),
    ('v249-patch-ui-tab-qol-golden.py', check_v249_patch_ui_tab_qol_golden),
    ('v250-preentry-typef-ownership-golden.py', check_v250_preentry_typef_ownership_golden),
    ('v251-preentry-reset-refresh-optimization-golden.py', check_v251_preentry_reset_refresh_optimization_golden),
    ('v252-preentry-cleanup-pruning-golden.py', check_v252_preentry_cleanup_pruning_golden),
    ('v252-preentry-prefix-compile-hotfix-golden.py', check_v252_preentry_prefix_compile_hotfix_golden),
    ('v253-debug-tools-additions-audit-golden.py', check_v253_debug_tools_additions_audit_golden),
    ('v254-game-identity-live-cheat-safety-golden.py', check_v254_game_identity_live_cheat_safety_golden),
    ('v255-preentry-valid-to-valid-startup-golden.py', check_v255_preentry_valid_to_valid_startup_golden),
    ('v256-cheat-file-discovery-optimization-golden.py', check_v256_cheat_file_discovery_optimization_golden),
    ('v257-deterministic-cheat-file-selection-golden.py', check_v257_deterministic_cheat_file_selection_golden),
    ('v258-stable-block-identity-golden.py', check_v258_stable_block_identity_golden),
    ('v259-preentry-selection-pruning-golden.py', check_v259_preentry_selection_pruning_golden),
    ('v260-preentry-per-patch-errors-golden.py', check_v260_preentry_per_patch_errors_golden),
    ('v261-patch-lifecycle-summary-golden.py', check_v261_patch_lifecycle_summary_golden),
    ('v262-mixed-group-checkbox-golden.py', check_v262_mixed_group_checkbox_golden),
    ('v263-scoped-tab-style-golden.py', check_v263_scoped_tab_style_golden),
    ('v271-preentry-selection-const-hotfix-golden.py', check_v271_preentry_selection_const_hotfix_golden),
)

def main() -> int:
    for legacy_name, check in CONTRACTS:
        try:
            check()
        except Exception as exc:
            raise AssertionError(f"v2.87 retained contract failed ({legacy_name}): {exc}") from exc
    print("PASS: v2.87 PREENTRY/Patch/Cheat regression contracts")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
