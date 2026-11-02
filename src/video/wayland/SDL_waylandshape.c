/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_WAYLAND

#include "SDL_waylandvideo.h"
#include "SDL_waylandwindow.h"
#include "SDL_waylandshape.h"

// Wayland has no window shapes, so the shape becomes the surface's input region, as X11 sets only the input shape.
// The opaque spans of each row receive input; the rest passes through to whatever is beneath the window.
bool Wayland_UpdateWindowShape(SDL_VideoDevice *_this, SDL_Window *window, SDL_Surface *shape)
{
    SDL_VideoData *video = _this->internal;
    SDL_WindowData *data = window->internal;
    SDL_Surface *stretched = NULL;
    struct wl_region *region;
    int x, y;

    if (!data->surface) {
        return SDL_SetError("Window has no surface");
    }

    if (!shape) {
        // A null region restores the default, the whole surface.
        wl_surface_set_input_region(data->surface, NULL);
        return true;
    }

    if (shape->w != window->w || shape->h != window->h) {
        stretched = SDL_CreateSurfaceUninitialized(window->w, window->h, SDL_PIXELFORMAT_ARGB32);
        if (!stretched) {
            return false;
        }
        if (!SDL_StretchSurface(shape, NULL, stretched, NULL, SDL_SCALEMODE_LINEAR)) {
            SDL_DestroySurface(stretched);
            return false;
        }
        shape = stretched;
    }

    region = wl_compositor_create_region(video->compositor);
    for (y = 0; y < shape->h; y++) {
        // SDL_PIXELFORMAT_ARGB32 stores alpha in the first byte of each pixel.
        const Uint8 *a = (const Uint8 *)shape->pixels + y * shape->pitch;
        int span_start = -1;
        for (x = 0; x < shape->w; x++) {
            if (*a != SDL_ALPHA_TRANSPARENT) {
                if (span_start == -1) {
                    span_start = x;
                }
            } else if (span_start != -1) {
                wl_region_add(region, span_start, y, x - span_start, 1);
                span_start = -1;
            }
            a += 4;
        }
        if (span_start != -1) {
            wl_region_add(region, span_start, y, x - span_start, 1);
        }
    }
    wl_surface_set_input_region(data->surface, region);
    wl_region_destroy(region);

    if (stretched) {
        SDL_DestroySurface(stretched);
    }
    return true;
}

#endif // SDL_VIDEO_DRIVER_WAYLAND
