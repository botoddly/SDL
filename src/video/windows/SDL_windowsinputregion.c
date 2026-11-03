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

#if defined(SDL_VIDEO_DRIVER_WINDOWS) && !defined(SDL_PLATFORM_XBOXONE) && !defined(SDL_PLATFORM_XBOXSERIES)

#include "SDL_windowsvideo.h"
#include "SDL_windowsinputregion.h"
#include "SDL_windowsshape.h"
#include "../../events/SDL_mouse_c.h"

/* Windows has no input region that leaves what a window shows alone. So a window with an input region lets all mouse input through, and a
   low-level mouse hook keeps the left button presses in the region, and their releases, from the windows beneath. Windows holds each mouse
   event, for the whole system, until the hook returns. So the hook runs on its own thread with its own message loop. It decides, as the input
   happens, whether a press is in a window's region, and queues the presses it blocks and their releases; it holds the lock only to read the
   regions and to queue. The event pump reports the queued input on the main thread. */

typedef struct
{
    SDL_WindowID window_id;
    bool down;
    // Window coordinates, as the window's own mouse events have them.
    float x;
    float y;
} QueuedButton;

typedef struct
{
    SDL_WindowID window_id;
    HWND hwnd;
    // The client area's origin in physical screen coordinates, as the hook reports them, and the size of a physical pixel in window coordinates.
    POINT origin;
    float scale_x;
    float scale_y;
    SDL_Rect *rects;
    int num_rects;
    int rects_capacity;
    // The hook blocked a press in the region, so it blocks the release too, wherever it happens.
    bool pressed;
} RegionWindow;

// Plenty for the presses between two pumps. Beyond it, input is dropped rather than growing without bound.
#define MAX_QUEUED_BUTTONS 4096

// The z-order can change during the walk up from a window, so the walk is bounded.
#define MAX_WINDOWS_ABOVE 4096

static struct
{
    // Guards windows, num_windows, queue, num_queued and queue_capacity, which the hook thread reads and writes. Only the main thread changes
    // windows and num_windows, so it reads them without the lock.
    SDL_Mutex *lock;
    RegionWindow *windows;
    int num_windows;
    QueuedButton *queue;
    int num_queued;
    int queue_capacity;

    // The last window is leaving the hook. Waiting for the thread handles sent messages, which can run SDL and application code.
    bool stopping;
    // The thread with the SDL windows, which reports the input. It is woken with a thread message, because a window can be recreated.
    DWORD main_thread_id;
    HANDLE thread;
    HANDLE ready_event;
    HANDLE stop_event;
    HHOOK hook;
    DWORD hook_error;
    UINT wake_message;
    // The hook reports physical coordinates, so its thread is per-monitor DPI aware for hit testing, whatever the process is.
    DPI_AWARENESS_CONTEXT (WINAPI *SetThreadDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);
} input_region;

// The hook reports the requested position, which can lie past a screen edge or the cursor clip rectangle; the cursor itself stays inside both.
static POINT ClampToCursorArea(POINT pt)
{
    HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info;
    RECT clip;

    SDL_zero(info);
    info.cbSize = sizeof(info);
    if (monitor && GetMonitorInfo(monitor, &info)) {
        pt.x = SDL_clamp(pt.x, info.rcMonitor.left, info.rcMonitor.right - 1);
        pt.y = SDL_clamp(pt.y, info.rcMonitor.top, info.rcMonitor.bottom - 1);
    }
    if (GetClipCursor(&clip)) {
        pt.x = SDL_clamp(pt.x, clip.left, clip.right - 1);
        pt.y = SDL_clamp(pt.y, clip.top, clip.bottom - 1);
    }
    return pt;
}

// Call it with the lock held.
static RegionWindow *FindWindowByID(SDL_WindowID window_id)
{
    int i;

    for (i = 0; i < input_region.num_windows; ++i) {
        if (input_region.windows[i].window_id == window_id) {
            return &input_region.windows[i];
        }
    }
    return NULL;
}

// Call it with the lock held.
static RegionWindow *FindWindowByHWND(HWND hwnd)
{
    int i;

    for (i = 0; i < input_region.num_windows; ++i) {
        if (input_region.windows[i].hwnd == hwnd) {
            return &input_region.windows[i];
        }
    }
    return NULL;
}

// Call it with the lock held. pt is in physical screen coordinates.
static void GetWindowPoint(const RegionWindow *window, POINT pt, float *x, float *y)
{
    // Whole window coordinates, as the window's own mouse events have them.
    *x = SDL_floorf((float)(pt.x - window->origin.x) * window->scale_x);
    *y = SDL_floorf((float)(pt.y - window->origin.y) * window->scale_y);
}

// Call it with the lock held. The window with pt, in physical screen coordinates, in its region, and pt in its window coordinates.
static RegionWindow *FindWindowAt(POINT pt, float *x, float *y)
{
    int i, j;

    for (i = 0; i < input_region.num_windows; ++i) {
        RegionWindow *window = &input_region.windows[i];

        // A hidden or minimized window takes no input. Neither call sends the window a message.
        if (window->num_rects == 0 || !IsWindowVisible(window->hwnd) || IsIconic(window->hwnd)) {
            continue;
        }

        GetWindowPoint(window, pt, x, y);
        for (j = 0; j < window->num_rects; ++j) {
            const SDL_Rect *rect = &window->rects[j];
            if (*x >= (float)rect->x && *y >= (float)rect->y && *x < (float)rect->x + (float)rect->w && *y < (float)rect->y + (float)rect->h) {
                return window;
            }
        }
    }
    return NULL;
}

/* Whether the window that would get input at pt, in physical screen coordinates, is above hwnd, such as another window that stays on top or a
   taskbar flyout. Hit testing skips windows that let all input through, so a window that only shows something over the screen doesn't count. */
static bool IsCoveredAt(HWND hwnd, POINT pt)
{
    HWND hit = WindowFromPoint(pt);
    HWND above;
    int i;

    if (!hit) {
        return false;
    }
    hit = GetAncestor(hit, GA_ROOT);

    above = GetWindow(hwnd, GW_HWNDPREV);
    for (i = 0; above && i < MAX_WINDOWS_ABOVE; ++i) {
        if (above == hit) {
            return true;
        }
        above = GetWindow(above, GW_HWNDPREV);
    }
    return false;
}

// Call it with the lock held.
static bool QueueButton(const RegionWindow *window, bool down, float x, float y)
{
    QueuedButton *button;

    if (input_region.num_queued == input_region.queue_capacity) {
        int capacity = input_region.queue_capacity ? input_region.queue_capacity * 2 : 16;
        QueuedButton *queue;
        if (capacity > MAX_QUEUED_BUTTONS) {
            return false;
        }
        queue = (QueuedButton *)SDL_realloc(input_region.queue, capacity * sizeof(*queue));
        if (!queue) {
            return false;
        }
        input_region.queue = queue;
        input_region.queue_capacity = capacity;
    }

    button = &input_region.queue[input_region.num_queued++];
    button->window_id = window->window_id;
    button->down = down;
    button->x = x;
    button->y = y;

    // Wake the event loop once per batch, so SDL_WaitEvent() returns. PostThreadMessage() does not wait.
    if (input_region.num_queued == 1) {
        PostThreadMessage(input_region.main_thread_id, input_region.wake_message, 0, 0);
    }
    return true;
}

// Whether the hook blocks a left button press at pt, in physical screen coordinates.
static bool BlockPress(POINT pt)
{
    RegionWindow *window;
    HWND hwnd = NULL;
    bool blocked = false;
    float x = 0.0f, y = 0.0f;

    SDL_LockMutex(input_region.lock);
    window = FindWindowAt(pt, &x, &y);
    if (window) {
        hwnd = window->hwnd;
    }
    SDL_UnlockMutex(input_region.lock);

    // Hit testing runs outside the lock, so the main thread never waits for it.
    if (!hwnd || IsCoveredAt(hwnd, pt)) {
        return false;
    }

    // The window may have left the hook meanwhile.
    SDL_LockMutex(input_region.lock);
    window = FindWindowByHWND(hwnd);
    if (window && QueueButton(window, true, x, y)) {
        window->pressed = true;
        blocked = true;
    }
    SDL_UnlockMutex(input_region.lock);
    return blocked;
}

// Whether the hook blocks a left button release at pt, in physical screen coordinates: it does if it blocked the press.
static bool BlockRelease(POINT pt)
{
    bool blocked = false;
    int i;

    SDL_LockMutex(input_region.lock);
    for (i = 0; i < input_region.num_windows; ++i) {
        RegionWindow *window = &input_region.windows[i];
        if (window->pressed) {
            float x, y;
            GetWindowPoint(window, pt, &x, &y);
            // The window beneath never got the press, so the release is blocked even if it can't be reported.
            QueueButton(window, false, x, y);
            window->pressed = false;
            blocked = true;
            break;
        }
    }
    SDL_UnlockMutex(input_region.lock);
    return blocked;
}

static LRESULT CALLBACK WIN_InputRegionHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && (wParam == WM_LBUTTONDOWN || wParam == WM_LBUTTONUP)) {
        const MSLLHOOKSTRUCT *info = (const MSLLHOOKSTRUCT *)lParam;

        // Touch and pens generate mouse input too, and the hook leaves it alone.
        if (WIN_GetMouseMessageSource((ULONG)info->dwExtraInfo) == SDL_MOUSE_EVENT_SOURCE_MOUSE) {
            const POINT pt = ClampToCursorArea(info->pt);
            const bool blocked = (wParam == WM_LBUTTONDOWN) ? BlockPress(pt) : BlockRelease(pt);
            if (blocked) {
                return 1;
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

static DWORD WINAPI WIN_InputRegionThread(LPVOID param)
{
    HMODULE module;
    MSG msg;

    // Windows silently removes a low-level hook that does not return in time.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    if (input_region.SetThreadDpiAwarenessContext) {
        input_region.SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }

    // SetWindowsHookEx() needs the module with the hook, and SDL can be linked statically or dynamically.
    if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPTSTR)WIN_InputRegionHookProc, &module)) {
        input_region.hook = SetWindowsHookEx(WH_MOUSE_LL, WIN_InputRegionHookProc, module, 0);
    }
    input_region.hook_error = input_region.hook ? 0 : GetLastError();
    SetEvent(input_region.ready_event);
    if (!input_region.hook) {
        return 0;
    }

    // Windows calls the hook while this thread retrieves messages.
    for (;;) {
        const DWORD result = MsgWaitForMultipleObjects(1, &input_region.stop_event, FALSE, INFINITE, QS_ALLINPUT);
        if (result != WAIT_OBJECT_0 + 1) {
            break;
        }
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        }
    }

    UnhookWindowsHookEx(input_region.hook);
    input_region.hook = NULL;
    return 0;
}

static void StopThread(void)
{
    if (input_region.thread) {
        HANDLE thread = input_region.thread;
        SetEvent(input_region.stop_event);
        // Handle messages sent to this thread while waiting, so a hook call that sent one can finish.
        while (MsgWaitForMultipleObjects(1, &thread, FALSE, INFINITE, QS_SENDMESSAGE) == WAIT_OBJECT_0 + 1) {
            MSG msg;
            PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
        }
        CloseHandle(thread);
        input_region.thread = NULL;
    }
    if (input_region.ready_event) {
        CloseHandle(input_region.ready_event);
        input_region.ready_event = NULL;
    }
    if (input_region.stop_event) {
        CloseHandle(input_region.stop_event);
        input_region.stop_event = NULL;
    }
    input_region.hook = NULL;
}

static bool StartThread(SDL_VideoDevice *_this)
{
    input_region.SetThreadDpiAwarenessContext = _this->internal->SetThreadDpiAwarenessContext;
    input_region.ready_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    input_region.stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!input_region.ready_event || !input_region.stop_event) {
        WIN_SetError("CreateEvent()");
        StopThread();
        return false;
    }

    input_region.thread = CreateThread(NULL, 0, WIN_InputRegionThread, NULL, 0, NULL);
    if (!input_region.thread) {
        WIN_SetError("CreateThread()");
        StopThread();
        return false;
    }

    WaitForSingleObject(input_region.ready_event, INFINITE);
    if (!input_region.hook) {
        // The thread exits on its own when the hook fails.
        const DWORD error = input_region.hook_error;
        StopThread();
        SetLastError(error);
        return WIN_SetError("SetWindowsHookEx()");
    }
    return true;
}

// Frees everything once no window is on the hook and the thread is stopped.
static void Cleanup(void)
{
    SDL_free(input_region.windows);
    SDL_free(input_region.queue);
    SDL_DestroyMutex(input_region.lock);
    SDL_zero(input_region);
}

static bool StartHook(SDL_VideoDevice *_this)
{
    input_region.lock = SDL_CreateMutex();
    if (!input_region.lock) {
        return false;
    }
    input_region.wake_message = RegisterWindowMessage(TEXT("SDL_InputRegionWake"));
    input_region.main_thread_id = GetCurrentThreadId();
    if (!StartThread(_this)) {
        Cleanup();
        return false;
    }
    return true;
}

static void StopHook(void)
{
    // The hook takes the lock, so nothing here holds it while waiting for the thread.
    input_region.stopping = true;
    StopThread();
    Cleanup();
}

/* Where the window's client area is in the physical screen coordinates the hook reports, and how they map to window coordinates. The window's
   own mouse events are in client coordinates, which are logical for a window that is not per-monitor DPI aware; for one that is, they are
   physical. */
static void GetPlacement(SDL_Window *window, POINT *origin, float *scale_x, float *scale_y)
{
    const SDL_WindowData *data = window->internal;
    RECT client;
    POINT corner, physical_origin, physical_corner;

    origin->x = 0;
    origin->y = 0;
    *scale_x = 1.0f;
    *scale_y = 1.0f;
    if (!GetClientRect(data->hwnd, &client) || !ClientToScreen(data->hwnd, origin)) {
        return;
    }

    // The conversion needs points inside the window, so the scale comes from the last pixel rather than the far edge.
    corner.x = SDL_max(client.right - 1, 0);
    corner.y = SDL_max(client.bottom - 1, 0);
    if (!data->videodata->LogicalToPhysicalPointForPerMonitorDPI || !ClientToScreen(data->hwnd, &corner)) {
        return;
    }
    physical_origin = *origin;
    physical_corner = corner;
    if (!data->videodata->LogicalToPhysicalPointForPerMonitorDPI(data->hwnd, &physical_origin) ||
        !data->videodata->LogicalToPhysicalPointForPerMonitorDPI(data->hwnd, &physical_corner)) {
        return;
    }
    if (physical_corner.x > physical_origin.x) {
        *scale_x = (float)(corner.x - origin->x) / (float)(physical_corner.x - physical_origin.x);
    }
    if (physical_corner.y > physical_origin.y) {
        *scale_y = (float)(corner.y - origin->y) / (float)(physical_corner.y - physical_origin.y);
    }
    *origin = physical_origin;
}

// Call it with the lock held.
static RegionWindow *AddWindow(SDL_Window *window)
{
    RegionWindow *windows = (RegionWindow *)SDL_realloc(input_region.windows, (input_region.num_windows + 1) * sizeof(*windows));
    RegionWindow *added;

    if (!windows) {
        return NULL;
    }
    input_region.windows = windows;
    added = &input_region.windows[input_region.num_windows++];
    SDL_zerop(added);
    added->window_id = window->id;
    return added;
}

bool WIN_SetWindowInputRegion(SDL_VideoDevice *_this, SDL_Window *window, const SDL_Rect *rects, int count)
{
    SDL_WindowData *data = window->internal;
    RegionWindow *entry;
    RECT box;
    POINT origin;
    float scale_x, scale_y;
    bool result = true;

    /* The window lets all mouse input through, and the hook takes the presses in the region. If the hook can't be installed, the window still
       lets all input through, so a failure never leaves it taking input it should let through. */
    if (!data->input_transparent && !WIN_SetWindowInputTransparent(window, true)) {
        return false;
    }
    // A region from SDL_SetWindowShape() clips what the window shows. Removing a region redraws the frame, so it is removed only if there is one.
    if (GetWindowRgnBox(data->hwnd, &box) != ERROR && !SetWindowRgn(data->hwnd, NULL, TRUE)) {
        return WIN_SetError("SetWindowRgn()");
    }

    // Only the window leaving the hook runs code during the stop, and it doesn't come back.
    if (input_region.stopping) {
        return SDL_SetError("The input region hook is stopping");
    }
    if (input_region.num_windows == 0 && !StartHook(_this)) {
        return false;
    }

    GetPlacement(window, &origin, &scale_x, &scale_y);

    SDL_LockMutex(input_region.lock);
    entry = FindWindowByID(window->id);
    if (!entry) {
        entry = AddWindow(window);
    }
    if (entry && count > entry->rects_capacity) {
        SDL_Rect *grown = (SDL_Rect *)SDL_realloc(entry->rects, count * sizeof(*grown));
        if (grown) {
            entry->rects = grown;
            entry->rects_capacity = count;
        } else {
            entry = NULL;
        }
    }
    if (entry) {
        // A count of 0 only empties the region, so a press the hook blocked still has its release blocked and reported.
        if (count > 0) {
            SDL_memcpy(entry->rects, rects, count * sizeof(*rects));
        }
        entry->num_rects = count;
        entry->hwnd = data->hwnd;
        entry->origin = origin;
        entry->scale_x = scale_x;
        entry->scale_y = scale_y;
    } else {
        result = false;
    }
    SDL_UnlockMutex(input_region.lock);

    // The hook started for a window that couldn't be added.
    if (input_region.num_windows == 0) {
        StopHook();
    }
    return result;
}

void WIN_UpdateWindowInputRegionPlacement(SDL_Window *window)
{
    RegionWindow *entry;
    POINT origin;
    float scale_x, scale_y;

    if (input_region.num_windows == 0) {
        return;
    }

    GetPlacement(window, &origin, &scale_x, &scale_y);

    SDL_LockMutex(input_region.lock);
    entry = FindWindowByID(window->id);
    if (entry) {
        entry->origin = origin;
        entry->scale_x = scale_x;
        entry->scale_y = scale_y;
    }
    SDL_UnlockMutex(input_region.lock);
}

void WIN_RemoveWindowInputRegion(SDL_Window *window)
{
    int i;

    if (input_region.num_windows == 0) {
        return;
    }

    SDL_LockMutex(input_region.lock);
    for (i = 0; i < input_region.num_windows; ++i) {
        if (input_region.windows[i].window_id == window->id) {
            SDL_free(input_region.windows[i].rects);
            --input_region.num_windows;
            SDL_memmove(&input_region.windows[i], &input_region.windows[i + 1], (input_region.num_windows - i) * sizeof(*input_region.windows));
            break;
        }
    }
    SDL_UnlockMutex(input_region.lock);

    if (input_region.num_windows == 0) {
        StopHook();
    }
}

void WIN_PumpInputRegionEvents(SDL_VideoDevice *_this)
{
    QueuedButton *queue;
    int num_queued;
    int i;

    if (input_region.num_windows == 0) {
        return;
    }

    // Report outside the lock, so the hook never waits for SDL.
    SDL_LockMutex(input_region.lock);
    queue = input_region.queue;
    num_queued = input_region.num_queued;
    input_region.queue = NULL;
    input_region.num_queued = 0;
    input_region.queue_capacity = 0;
    SDL_UnlockMutex(input_region.lock);

    for (i = 0; i < num_queued; ++i) {
        const QueuedButton *button = &queue[i];
        SDL_Window *window;

        for (window = _this->windows; window; window = window->next) {
            if (window->id == button->window_id) {
                break;
            }
        }
        if (!window) {
            continue;
        }

        SDL_SendInputRegionMouseButton(0, window, button->x, button->y, SDL_BUTTON_LEFT, button->down);
        // The window lets all mouse input through, so it never gets a native leave. The input it took ends with the release.
        if (!button->down && SDL_GetMouseFocus() == window) {
            SDL_SetMouseFocus(NULL);
        }
    }
    SDL_free(queue);
}

#endif // defined(SDL_VIDEO_DRIVER_WINDOWS) && !defined(SDL_PLATFORM_XBOXONE) && !defined(SDL_PLATFORM_XBOXSERIES)
