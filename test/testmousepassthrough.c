/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* An always-on-top transparent window that lets all mouse input through to the windows beneath it, and reports that input with
   SDL_SetWindowMousePassthroughEvents(). It logs each mouse event and draws a dot where the pointer is. It waits for events and redraws only
   when something changed, so it uses almost no CPU. It uses the software renderer, which draws into a window whose shape lets all input
   through on every platform.

   --opaque  makes a square in the middle of the window opaque, so the window receives the input there itself.
   --fullscreen  covers the display instead, like a desktop overlay. It cannot be combined with --opaque.
   --toggle N  turns the passthrough events off and on again every N seconds.
   --seconds N  quits after N seconds. Otherwise quit with Ctrl+C, or with Escape while the window has keyboard focus. */

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#define WINDOW_WIDTH 640
#define WINDOW_HEIGHT 480
#define OPAQUE_SIZE 160

static SDL_FRect OpaqueRect(void)
{
    SDL_FRect rect;
    rect.x = (WINDOW_WIDTH - OPAQUE_SIZE) / 2.0f;
    rect.y = (WINDOW_HEIGHT - OPAQUE_SIZE) / 2.0f;
    rect.w = OPAQUE_SIZE;
    rect.h = OPAQUE_SIZE;
    return rect;
}

static bool SetShape(SDL_Window *window, bool opaque)
{
    bool result;
    SDL_Surface *shape = SDL_CreateSurface(WINDOW_WIDTH, WINDOW_HEIGHT, SDL_PIXELFORMAT_ARGB8888);
    if (!shape) {
        return false;
    }

    // SDL_CreateSurface clears the pixels, so the shape starts fully transparent.
    if (opaque) {
        SDL_FRect area = OpaqueRect();
        SDL_Rect rect = { (int)area.x, (int)area.y, (int)area.w, (int)area.h };
        SDL_FillSurfaceRect(shape, &rect, SDL_MapSurfaceRGBA(shape, 255, 255, 255, 255));
    }
    result = SDL_SetWindowShape(window, shape);
    SDL_DestroySurface(shape);
    return result;
}

static void LogEvent(const SDL_Event *event)
{
    switch (event->type) {
    case SDL_EVENT_MOUSE_MOTION:
        SDL_Log("motion window=%" SDL_PRIu32 " (%.1f, %.1f)", event->motion.windowID, event->motion.x, event->motion.y);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        SDL_Log("button %d %s window=%" SDL_PRIu32 " (%.1f, %.1f) clicks=%d", event->button.button, event->button.down ? "down" : "up",
                event->button.windowID, event->button.x, event->button.y, event->button.clicks);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        SDL_Log("wheel window=%" SDL_PRIu32 " (%.1f, %.1f)", event->wheel.windowID, event->wheel.x, event->wheel.y);
        break;
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
        SDL_Log("enter window=%" SDL_PRIu32, event->window.windowID);
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        SDL_Log("leave window=%" SDL_PRIu32, event->window.windowID);
        break;
    default:
        break;
    }
}

int main(int argc, char *argv[])
{
    SDL_Window *window;
    SDL_Renderer *renderer;
    bool opaque = false;
    SDL_WindowFlags flags = SDL_WINDOW_TRANSPARENT | SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP;
    Uint64 deadline = 0;
    Uint64 toggle_interval = 0;
    Uint64 next_toggle = 0;
    bool enabled = true;
    bool done = false;
    bool redraw = true;
    bool has_position = false;
    SDL_FPoint position = { 0.0f, 0.0f };
    int i;

    for (i = 1; i < argc; ++i) {
        if (SDL_strcmp(argv[i], "--opaque") == 0) {
            opaque = true;
        } else if (SDL_strcmp(argv[i], "--fullscreen") == 0) {
            flags |= SDL_WINDOW_FULLSCREEN;
        } else if (SDL_strcmp(argv[i], "--toggle") == 0 && i + 1 < argc) {
            toggle_interval = (Uint64)SDL_atoi(argv[++i]) * 1000;
        } else if (SDL_strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            deadline = SDL_GetTicks() + (Uint64)SDL_atoi(argv[++i]) * 1000;
        } else {
            SDL_Log("Usage: %s [--opaque | --fullscreen] [--toggle N] [--seconds N]", argv[0]);
            return 1;
        }
    }
    // The opaque square is placed for the windowed size.
    if (opaque && (flags & SDL_WINDOW_FULLSCREEN)) {
        SDL_Log("--opaque cannot be combined with --fullscreen");
        return 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    window = SDL_CreateWindow("testmousepassthrough", WINDOW_WIDTH, WINDOW_HEIGHT, flags);
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    renderer = SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);
    if (!renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    if (!SetShape(window, opaque)) {
        SDL_Log("SDL_SetWindowShape failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    if (!SDL_SetWindowMousePassthroughEvents(window, true)) {
        SDL_Log("SDL_SetWindowMousePassthroughEvents failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_Log("Window %" SDL_PRIu32 " receives mouse passthrough events", SDL_GetWindowID(window));
    if (toggle_interval) {
        next_toggle = SDL_GetTicks() + toggle_interval;
    }

    while (!done) {
        SDL_Event event;
        Uint64 now = SDL_GetTicks();
        Uint64 wake = 0;
        Sint32 timeout = -1;
        bool has_event;

        // Sleep until an event arrives or the next toggle or the deadline is due.
        if (toggle_interval) {
            wake = next_toggle;
        }
        if (deadline && (!wake || deadline < wake)) {
            wake = deadline;
        }
        if (wake) {
            timeout = (wake > now) ? (Sint32)SDL_min(wake - now, (Uint64)SDL_MAX_SINT32) : 0;
        }

        has_event = SDL_WaitEventTimeout(&event, timeout);
        while (has_event) {
            LogEvent(&event);
            switch (event.type) {
            case SDL_EVENT_QUIT:
                done = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.key == SDLK_ESCAPE) {
                    done = true;
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                position.x = event.motion.x;
                position.y = event.motion.y;
                has_position = true;
                redraw = true;
                break;
            case SDL_EVENT_WINDOW_MOUSE_LEAVE:
                has_position = false;
                redraw = true;
                break;
            case SDL_EVENT_WINDOW_EXPOSED:
            case SDL_EVENT_WINDOW_RESIZED:
                redraw = true;
                break;
            default:
                break;
            }
            has_event = SDL_PollEvent(&event);
        }
        if (toggle_interval && SDL_GetTicks() >= next_toggle) {
            enabled = !enabled;
            if (SDL_SetWindowMousePassthroughEvents(window, enabled)) {
                SDL_Log("Passthrough events %s", enabled ? "enabled" : "disabled");
            } else {
                SDL_Log("SDL_SetWindowMousePassthroughEvents failed: %s", SDL_GetError());
            }
            next_toggle += toggle_interval;
        }
        if (deadline && SDL_GetTicks() >= deadline) {
            done = true;
        }

        if (!redraw) {
            continue;
        }
        redraw = false;

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_TRANSPARENT);
        SDL_RenderClear(renderer);
        if (opaque) {
            SDL_FRect rect = OpaqueRect();
            SDL_SetRenderDrawColor(renderer, 64, 64, 160, SDL_ALPHA_OPAQUE);
            SDL_RenderFillRect(renderer, &rect);
        }
        if (has_position) {
            SDL_FRect dot = { position.x - 6.0f, position.y - 6.0f, 12.0f, 12.0f };
            SDL_SetRenderDrawColor(renderer, 255, 160, 0, SDL_ALPHA_OPAQUE);
            SDL_RenderFillRect(renderer, &dot);
        }
        SDL_RenderPresent(renderer);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
