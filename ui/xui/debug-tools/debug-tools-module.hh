//
// xemu Debug Tools module/addition registration API
//
// Optional additions depend on this API; upstream xemu does not depend on the
// additions themselves.
//
#pragma once

#include <cstdint>

using DebugToolsCallback = void (*)();
using DebugToolsCurrentGameTabsCallback = void (*)(uint32_t title_id);

void debug_tools_register_tick(int order, DebugToolsCallback callback);
void debug_tools_register_reset(int order, DebugToolsCallback callback);
void debug_tools_register_menu_item(int order, const char *label, bool *open);
void debug_tools_register_current_game_extension(
    int order,
    DebugToolsCurrentGameTabsCallback draw_tabs,
    DebugToolsCallback draw_footer);

// Called by Current Game while its tab bar/footer are active. Additions can
// extend Current Game without Current Game including their headers.
void debug_tools_draw_current_game_extension_tabs(uint32_t title_id);
void debug_tools_draw_current_game_extension_footer();

// Addition entry points. Meson selects the real implementation or a no-op stub
// from the local Debug Tools build profile.
void debug_tools_register_hdd_addition();
void debug_tools_register_memory_tools_addition();
