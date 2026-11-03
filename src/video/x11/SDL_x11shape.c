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

#ifdef SDL_VIDEO_DRIVER_X11

#include "SDL_x11video.h"
#include "SDL_x11shape.h"


#ifdef SDL_VIDEO_DRIVER_X11_XSHAPE
static Uint8 *GenerateShapeMask(SDL_Surface *shape)
{
    int x, y;
    const size_t ppb = 8;
    const size_t bytes_per_scanline = (shape->w + (ppb - 1)) / ppb;
    const Uint8 *a;
    Uint8 *mask;
    Uint8 *mask_scanline;
    Uint8 mask_value;

    mask = (Uint8 *)SDL_calloc(1, shape->h * bytes_per_scanline);
    if (mask) {
        for (y = 0; y < shape->h; y++) {
            a = (const Uint8 *)shape->pixels + y * shape->pitch;
            mask_scanline = mask + y * bytes_per_scanline;
            for (x = 0; x < shape->w; x++) {
                mask_value = (*a == SDL_ALPHA_TRANSPARENT) ? 0 : 1;
                mask_scanline[x / ppb] |= mask_value << (x % ppb);
                a += 4;
            }
        }
    }
    return mask;
}
#endif // SDL_VIDEO_DRIVER_X11_XSHAPE

bool X11_UpdateWindowShape(SDL_VideoDevice *_this, SDL_Window *window, SDL_Surface *shape)
{
    bool result = false;

#ifdef SDL_VIDEO_DRIVER_X11_XSHAPE
    SDL_WindowData *windowdata = window->internal;

    // Generate a set of spans for the region
    if (shape) {
        SDL_Surface *stretched = NULL;
        Uint8 *mask;
        Pixmap pixmap;

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

        mask = GenerateShapeMask(shape);
        if (mask) {
            pixmap = X11_XCreateBitmapFromData(windowdata->videodata->display, windowdata->xwindow, (const char *)mask, shape->w, shape->h);
            X11_XShapeCombineMask(windowdata->videodata->display, windowdata->xwindow, ShapeInput, 0, 0, pixmap, ShapeSet);
            SDL_free(mask);

            result = true;
        }

        if (stretched) {
            SDL_DestroySurface(stretched);
        }
    } else {
        Region region = X11_XCreateRegion();
        XRectangle rect;

        rect.x = 0;
        rect.y = 0;
        rect.width = window->w;
        rect.height = window->h;
        X11_XUnionRectWithRegion(&rect, region, region);
        X11_XShapeCombineRegion(windowdata->videodata->display, windowdata->xwindow, ShapeInput, 0, 0, region, ShapeSet);
        X11_XDestroyRegion(region);
        result = true;
    }
#endif // SDL_VIDEO_DRIVER_X11_XSHAPE

    return result;
}

bool X11_SetWindowInputRegion(SDL_VideoDevice *_this, SDL_Window *window, const SDL_Rect *rects, int count)
{
#ifdef SDL_VIDEO_DRIVER_X11_XSHAPE
    SDL_WindowData *windowdata = window->internal;
    Display *display = windowdata->videodata->display;
    XRectangle *xrects = NULL;
    bool isstack = false;
    int num_xrects = 0;
    int event_base, error_base;
    int i;

    if (!SDL_X11_HAVE_XSHAPE || !X11_XShapeQueryExtension(display, &event_base, &error_base)) {
        return SDL_SetError("The X server doesn't support the SHAPE extension");
    }

    if (count > 0) {
        xrects = SDL_small_alloc(XRectangle, count, &isstack);
        if (!xrects) {
            return false;
        }
    }
    // X rectangles have 16-bit coordinates, and one with no area would wrap around to a huge one.
    for (i = 0; i < count; ++i) {
        const SDL_Rect *rect = &rects[i];
        if (rect->w > 0 && rect->h > 0) {
            XRectangle *xrect = &xrects[num_xrects++];
            xrect->x = (short)SDL_clamp(rect->x, SDL_MIN_SINT16, SDL_MAX_SINT16);
            xrect->y = (short)SDL_clamp(rect->y, SDL_MIN_SINT16, SDL_MAX_SINT16);
            xrect->width = (unsigned short)SDL_min(rect->w, SDL_MAX_UINT16);
            xrect->height = (unsigned short)SDL_min(rect->h, SDL_MAX_UINT16);
        }
    }

    // The input shape leaves what the window shows alone. An empty one lets all input through.
    X11_XShapeCombineRectangles(display, windowdata->xwindow, ShapeInput, 0, 0, xrects, num_xrects, ShapeSet, Unsorted);
    X11_XFlush(display);

    if (xrects) {
        SDL_small_free(xrects, isstack);
    }
    return true;
#else
    return SDL_Unsupported();
#endif // SDL_VIDEO_DRIVER_X11_XSHAPE
}

#endif // SDL_VIDEO_DRIVER_X11
