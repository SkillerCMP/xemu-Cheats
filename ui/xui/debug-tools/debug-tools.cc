//
// xemu Debug Tools integration facade / module registry
//
// Copyright (C) 2026 xemu contributors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//

#include "debug-tools.hh"
#include "debug-tools-module.hh"

#include "../common.hh"
#include "cheat-engine.hh"
#include "current-game.hh"
#include "detached-tools.hh"

#include <algorithm>
#include <vector>

namespace {

template <typename T>
void StableSortByOrder(std::vector<T> &items)
{
    std::stable_sort(items.begin(), items.end(),
                     [](const T &a, const T &b) { return a.order < b.order; });
}

struct OrderedCallback {
    int order = 0;
    DebugToolsCallback callback = nullptr;
};

struct MenuItemRegistration {
    int order = 0;
    const char *label = nullptr;
    bool *open = nullptr;
};

struct CurrentGameExtensionRegistration {
    int order = 0;
    DebugToolsCurrentGameTabsCallback draw_tabs = nullptr;
    DebugToolsCallback draw_footer = nullptr;
};

std::vector<OrderedCallback> g_tick_callbacks;
std::vector<OrderedCallback> g_reset_callbacks;
std::vector<MenuItemRegistration> g_menu_items;
std::vector<CurrentGameExtensionRegistration> g_current_game_extensions;
bool g_initialized = false;

void DrawCheatEngineDetached()
{
    cheat_engine_window.Draw(true);
}

void DrawCurrentGameDetached()
{
    current_game_manager.Draw(true);
}

void RefreshCurrentGame()
{
    current_game_manager.Refresh();
}

void TickCheatEngine()
{
    cheat_engine_window.Tick();
}

void NotifyCheatEngineReset()
{
    cheat_engine_window.NotifyGameResetRequested();
}

void RegisterMainAddition()
{
    // Preserve the historical menu order when optional additions are present:
    // Current Game, HDD Directory, Cheat Engine, Memory Viewer / Search.
    debug_tools_register_menu_item(100, "Current Game",
                                   &current_game_manager.is_open);
    debug_tools_register_menu_item(300, "Cheat Engine",
                                   &cheat_engine_window.is_open);

    // Preserve historical per-frame order around the optional HDD tick:
    // Guest Kernel RPC (100), Current Game (200), Cheat Engine (300).
    debug_tools_register_tick(200, RefreshCurrentGame);
    debug_tools_register_tick(300, TickCheatEngine);

    // Memory Tools registers at 100 when present, preserving the historical
    // reset order before the Cheat Engine PREENTRY/F-hook reset notification.
    debug_tools_register_reset(200, NotifyCheatEngineReset);

    detached_tools_register({
        "xemu - Current Game",
        900, 620,
        700, 480,
        &current_game_manager.is_open,
        DrawCurrentGameDetached,
        100,
    });
    detached_tools_register({
        "xemu - RAW Cheat Engine",
        980, 760,
        720, 520,
        &cheat_engine_window.is_open,
        DrawCheatEngineDetached,
        300,
    });
}

void FinalizeRegistrations()
{
    StableSortByOrder(g_tick_callbacks);
    StableSortByOrder(g_reset_callbacks);
    StableSortByOrder(g_menu_items);
    StableSortByOrder(g_current_game_extensions);
    detached_tools_finalize_registry();
}

void ClearRegistrations()
{
    g_tick_callbacks.clear();
    g_reset_callbacks.clear();
    g_menu_items.clear();
    g_current_game_extensions.clear();
    detached_tools_clear_registry();
}

} // namespace

void debug_tools_register_tick(int order, DebugToolsCallback callback)
{
    if (callback) {
        g_tick_callbacks.push_back({order, callback});
    }
}

void debug_tools_register_reset(int order, DebugToolsCallback callback)
{
    if (callback) {
        g_reset_callbacks.push_back({order, callback});
    }
}

void debug_tools_register_menu_item(int order, const char *label, bool *open)
{
    if (label && open) {
        g_menu_items.push_back({order, label, open});
    }
}

void debug_tools_register_current_game_extension(
    int order,
    DebugToolsCurrentGameTabsCallback draw_tabs,
    DebugToolsCallback draw_footer)
{
    if (draw_tabs || draw_footer) {
        g_current_game_extensions.push_back({order, draw_tabs, draw_footer});
    }
}

void debug_tools_init(SDL_Window *main_window, void *main_gl_context)
{
    if (g_initialized) {
        return;
    }

    ClearRegistrations();
    RegisterMainAddition();

    // Optional additions hook only the Debug Tools module API. Meson supplies
    // real registration units for enabled additions and tiny no-op stubs for
    // disabled additions, so upstream xemu never needs feature-specific code.
    debug_tools_register_hdd_addition();
    debug_tools_register_memory_tools_addition();

    FinalizeRegistrations();
    detached_tools_init(main_window, main_gl_context);
    g_initialized = true;
}

void debug_tools_cleanup()
{
    if (!g_initialized) {
        return;
    }
    detached_tools_cleanup();
    ClearRegistrations();
    g_initialized = false;
}

bool debug_tools_process_sdl_event(SDL_Event *event)
{
    return detached_tools_process_sdl_event(event);
}

bool debug_tools_owns_window_id(SDL_WindowID window_id)
{
    return detached_tools_owns_window_id(window_id);
}

void debug_tools_tick()
{
    for (const OrderedCallback &entry : g_tick_callbacks) {
        entry.callback();
    }
}

void debug_tools_draw_menu_items()
{
    for (const MenuItemRegistration &entry : g_menu_items) {
        ImGui::MenuItem(entry.label, nullptr, entry.open);
    }
}

void debug_tools_build_detached_frames()
{
    detached_tools_build_frames();
}

void debug_tools_render_detached_frames()
{
    detached_tools_render_frames();
}

void debug_tools_notify_game_reset()
{
    for (const OrderedCallback &entry : g_reset_callbacks) {
        entry.callback();
    }
}

void debug_tools_draw_current_game_extension_tabs(uint32_t title_id)
{
    for (const CurrentGameExtensionRegistration &entry :
         g_current_game_extensions) {
        if (entry.draw_tabs) {
            entry.draw_tabs(title_id);
        }
    }
}

void debug_tools_draw_current_game_extension_footer()
{
    for (const CurrentGameExtensionRegistration &entry :
         g_current_game_extensions) {
        if (entry.draw_footer) {
            entry.draw_footer();
        }
    }
}
