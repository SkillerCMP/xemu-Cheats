//
// xemu Detached Debug Tool Windows
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

struct DetachedToolRegistration {
    const char *title = nullptr;
    int default_width = 900;
    int default_height = 700;
    int min_width = 640;
    int min_height = 480;
    bool *open = nullptr;
    void (*draw)() = nullptr;
    int order = 0;
};

// Core/additions register detached windows here so this host-window layer has
// no direct dependency on feature headers such as HDD Directory or Memory Tools.
void detached_tools_register(const DetachedToolRegistration &registration);
void detached_tools_finalize_registry();
void detached_tools_clear_registry();

void detached_tools_init(SDL_Window *main_window, void *main_gl_context);
void detached_tools_cleanup();
bool detached_tools_process_sdl_event(SDL_Event *event);
bool detached_tools_owns_window_id(SDL_WindowID window_id);
void detached_tools_build_frames();
void detached_tools_render_frames();
