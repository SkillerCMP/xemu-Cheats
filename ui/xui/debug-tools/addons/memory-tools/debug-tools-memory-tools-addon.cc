// xemu Debug Tools - Memory Viewer / Search optional addition registration.

#include "../../debug-tools-module.hh"
#include "../../detached-tools.hh"
#include "memory-tools.hh"

void xemu_memory_tools_notify_game_reset();

namespace {

void DrawMemoryToolsDetached()
{
    memory_tools_window.Draw(true);
}

void NotifyMemoryToolsReset()
{
    xemu_memory_tools_notify_game_reset();
}

} // namespace

void debug_tools_register_memory_tools_addition()
{
    debug_tools_register_menu_item(400, "Memory Viewer / Search",
                                   &memory_tools_window.is_open);
    debug_tools_register_reset(100, NotifyMemoryToolsReset);

    detached_tools_register({
        "xemu - Memory Viewer / Search / x86 Debugger",
        1320, 860,
        900, 620,
        &memory_tools_window.is_open,
        DrawMemoryToolsDetached,
        400,
    });
}
