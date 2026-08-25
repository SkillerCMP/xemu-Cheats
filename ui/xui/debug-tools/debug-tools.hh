//
// xemu Debug Tools integration facade
//
// Copyright (C) 2026 xemu contributors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once

#include <SDL3/SDL.h>

// These are the only normal xui-facing Debug Tools entry points. Optional
// additions register with this facade instead of adding their own hooks to
// upstream xemu UI files.
void debug_tools_init(SDL_Window *main_window, void *main_gl_context);
void debug_tools_cleanup();
bool debug_tools_process_sdl_event(SDL_Event *event);
bool debug_tools_owns_window_id(SDL_WindowID window_id);

void debug_tools_tick();
void debug_tools_draw_menu_items();
void debug_tools_build_detached_frames();
void debug_tools_render_detached_frames();
void debug_tools_notify_game_reset();
