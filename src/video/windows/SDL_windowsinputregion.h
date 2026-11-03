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

#ifndef SDL_windowsinputregion_h_
#define SDL_windowsinputregion_h_

extern bool WIN_SetWindowInputRegion(SDL_VideoDevice *_this, SDL_Window *window, const SDL_Rect *rects, int count);
// Updates where the hook finds the window's region, when the window moves, resizes or changes its DPI.
extern void WIN_UpdateWindowInputRegionPlacement(SDL_Window *window);
// Takes the window off the hook, when it is destroyed.
extern void WIN_RemoveWindowInputRegion(SDL_Window *window);
// Reports the presses in input regions, and their releases, that the hook took since the last call.
extern void WIN_PumpInputRegionEvents(SDL_VideoDevice *_this);

#endif // SDL_windowsinputregion_h_
