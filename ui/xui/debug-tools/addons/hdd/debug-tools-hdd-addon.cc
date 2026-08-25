// xemu Debug Tools - HDD Directory optional addition registration.

#include "../../debug-tools-module.hh"
#include "../../detached-tools.hh"
#include "guest-kernel-rpc.hh"
#include "hdd-directory.hh"

#include "../../../common.hh"

namespace {

bool g_show_kernel_rpc_diagnostics = false;

void TickHddAddition()
{
    guest_kernel_rpc_manager.Tick();
}

void DrawHddDirectoryDetached()
{
    hdd_directory_window.Draw(true);
}

void DrawCurrentGameHddTabs(uint32_t title_id)
{
    if (ImGui::BeginTabItem("HDD")) {
        hdd_directory_window.DrawCurrentGameHdd(title_id);
        ImGui::EndTabItem();
    }

    if (g_show_kernel_rpc_diagnostics &&
        ImGui::BeginTabItem("Kernel RPC Diagnostics")) {
        guest_kernel_rpc_manager.DrawTestPanel();
        ImGui::EndTabItem();
    }
}

void DrawCurrentGameHddFooter()
{
    ImGui::Separator();
    ImGui::Checkbox("Show Kernel RPC diagnostics",
                    &g_show_kernel_rpc_diagnostics);
    if (!g_show_kernel_rpc_diagnostics) {
        ImGui::SameLine();
        ImGui::TextDisabled("Advanced/developer filesystem RPC test surface");
    }
}

} // namespace

void debug_tools_register_hdd_addition()
{
    debug_tools_register_menu_item(200, "HDD Directory",
                                   &hdd_directory_window.is_open);
    debug_tools_register_tick(100, TickHddAddition);
    debug_tools_register_current_game_extension(
        100, DrawCurrentGameHddTabs, DrawCurrentGameHddFooter);

    detached_tools_register({
        "xemu - Xbox HDD Directory",
        1100, 760,
        760, 520,
        &hdd_directory_window.is_open,
        DrawHddDirectoryDetached,
        200,
    });
}
