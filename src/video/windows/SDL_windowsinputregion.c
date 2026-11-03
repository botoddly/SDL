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
#include "../../events/SDL_mouse_c.h"

/* A low-level mouse hook sees the mouse input that goes to every window. Windows holds each mouse event, for the whole system, until the hook
   returns. So the hook runs on its own thread with its own message loop. It decides, as the input happens, whether an SDL window receives the
   input itself, and queues the rest; it holds the lock only to queue. The event pump reports the queued input on the main thread. */

typedef struct
{
    UINT message;
    POINT pt;
    WORD xbutton;
} PassthroughInput;

// Plenty for the input between two pumps, as motion is merged. Beyond it, input is dropped rather than growing without bound.
#define MAX_QUEUED_INPUT 4096

static struct
{
    // Guards queue, num_queued and queue_capacity, which the hook thread reads and writes.
    SDL_Mutex *lock;
    PassthroughInput *queue;
    int num_queued;
    int queue_capacity;

    int num_windows;
    // The last window is being disabled. Waiting for the thread handles sent messages, which can run SDL and application code.
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
} passthrough;

static HWND GetThreadCapture(DWORD thread_id)
{
    GUITHREADINFO info;

    SDL_zero(info);
    info.cbSize = sizeof(info);
    return GetGUIThreadInfo(thread_id, &info) ? info.hwndCapture : NULL;
}

static bool IsOwnWindow(HWND hwnd)
{
    DWORD process_id = 0;
    GetWindowThreadProcessId(hwnd, &process_id);
    return process_id == GetCurrentProcessId();
}

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

// Whether an SDL window receives the input at pt itself, decided as the input happens. pt is in physical screen coordinates.
static bool IsNativeInput(POINT pt)
{
    HWND foreground;
    HWND hwnd;
    LONG_PTR style;

    // A capture routes the input to the capturing window wherever the pointer is. SDL captures for any of its windows, also ones that are not
    // in the foreground; another application's capture is on the foreground thread.
    if (GetThreadCapture(passthrough.main_thread_id)) {
        return true;
    }
    foreground = GetForegroundWindow();
    if (foreground && GetThreadCapture(GetWindowThreadProcessId(foreground, NULL))) {
        return false;
    }

    hwnd = WindowFromPoint(pt);
    if (!hwnd || !IsOwnWindow(hwnd)) {
        return false;
    }
    // A window that lets all input through never receives it, whether or not hit testing skips it.
    style = GetWindowLongPtr(GetAncestor(hwnd, GA_ROOT), GWL_EXSTYLE);
    return (style & (WS_EX_LAYERED | WS_EX_TRANSPARENT)) != (WS_EX_LAYERED | WS_EX_TRANSPARENT);
}

// Call it with the lock held.
static void QueueInput(UINT message, POINT pt, WORD xbutton)
{
    PassthroughInput *input;

    // Only the latest position of consecutive motion matters.
    if (message == WM_MOUSEMOVE && passthrough.num_queued > 0 && passthrough.queue[passthrough.num_queued - 1].message == WM_MOUSEMOVE) {
        passthrough.queue[passthrough.num_queued - 1].pt = pt;
        return;
    }

    if (passthrough.num_queued == passthrough.queue_capacity) {
        int capacity = passthrough.queue_capacity ? passthrough.queue_capacity * 2 : 64;
        PassthroughInput *queue;
        if (capacity > MAX_QUEUED_INPUT) {
            return;
        }
        queue = (PassthroughInput *)SDL_realloc(passthrough.queue, capacity * sizeof(*queue));
        if (!queue) {
            return;
        }
        passthrough.queue = queue;
        passthrough.queue_capacity = capacity;
    }

    input = &passthrough.queue[passthrough.num_queued++];
    input->message = message;
    input->pt = pt;
    input->xbutton = xbutton;

    // Wake the event loop once per batch, so SDL_WaitEvent() returns. PostThreadMessage() does not wait.
    if (passthrough.num_queued == 1) {
        PostThreadMessage(passthrough.main_thread_id, passthrough.wake_message, 0, 0);
    }
}

static LRESULT CALLBACK WIN_MousePassthroughHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION) {
        const MSLLHOOKSTRUCT *info = (const MSLLHOOKSTRUCT *)lParam;
        const UINT message = (UINT)wParam;
        bool release = false;
        bool handled = true;

        switch (message) {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
            break;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
        case WM_XBUTTONUP:
            release = true;
            break;
        default:
            handled = false;
            break;
        }

        // Touch and pens generate mouse input too. A release is queued even when it is native: it is reported only if its press was, and the
        // native one is then ignored.
        if (handled && WIN_GetMouseMessageSource((ULONG)info->dwExtraInfo) == SDL_MOUSE_EVENT_SOURCE_MOUSE) {
            const POINT pt = ClampToCursorArea(info->pt);
            if (release || !IsNativeInput(pt)) {
                SDL_LockMutex(passthrough.lock);
                QueueInput(message, pt, HIWORD(info->mouseData));
                SDL_UnlockMutex(passthrough.lock);
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

static DWORD WINAPI WIN_MousePassthroughThread(LPVOID param)
{
    HMODULE module;
    MSG msg;

    // Windows silently removes a low-level hook that does not return in time.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    if (passthrough.SetThreadDpiAwarenessContext) {
        passthrough.SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }

    // SetWindowsHookEx() needs the module with the hook, and SDL can be linked statically or dynamically.
    if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPTSTR)WIN_MousePassthroughHookProc, &module)) {
        passthrough.hook = SetWindowsHookEx(WH_MOUSE_LL, WIN_MousePassthroughHookProc, module, 0);
    }
    passthrough.hook_error = passthrough.hook ? 0 : GetLastError();
    SetEvent(passthrough.ready_event);
    if (!passthrough.hook) {
        return 0;
    }

    // Windows calls the hook while this thread retrieves messages.
    for (;;) {
        const DWORD result = MsgWaitForMultipleObjects(1, &passthrough.stop_event, FALSE, INFINITE, QS_ALLINPUT);
        if (result != WAIT_OBJECT_0 + 1) {
            break;
        }
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        }
    }

    UnhookWindowsHookEx(passthrough.hook);
    passthrough.hook = NULL;
    return 0;
}

static void StopThread(void)
{
    if (passthrough.thread) {
        HANDLE thread = passthrough.thread;
        SetEvent(passthrough.stop_event);
        // Handle messages sent to this thread while waiting, so a hook call that sent one can finish.
        while (MsgWaitForMultipleObjects(1, &thread, FALSE, INFINITE, QS_SENDMESSAGE) == WAIT_OBJECT_0 + 1) {
            MSG msg;
            PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
        }
        CloseHandle(thread);
        passthrough.thread = NULL;
    }
    if (passthrough.ready_event) {
        CloseHandle(passthrough.ready_event);
        passthrough.ready_event = NULL;
    }
    if (passthrough.stop_event) {
        CloseHandle(passthrough.stop_event);
        passthrough.stop_event = NULL;
    }
    passthrough.hook = NULL;
}

static bool StartThread(SDL_VideoDevice *_this)
{
    passthrough.SetThreadDpiAwarenessContext = _this->internal->SetThreadDpiAwarenessContext;
    passthrough.ready_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    passthrough.stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!passthrough.ready_event || !passthrough.stop_event) {
        WIN_SetError("CreateEvent()");
        StopThread();
        return false;
    }

    passthrough.thread = CreateThread(NULL, 0, WIN_MousePassthroughThread, NULL, 0, NULL);
    if (!passthrough.thread) {
        WIN_SetError("CreateThread()");
        StopThread();
        return false;
    }

    WaitForSingleObject(passthrough.ready_event, INFINITE);
    if (!passthrough.hook) {
        // The thread exits on its own when the hook fails.
        const DWORD error = passthrough.hook_error;
        StopThread();
        SetLastError(error);
        return WIN_SetError("SetWindowsHookEx()");
    }
    return true;
}

// Frees everything once no window has mouse passthrough events and the thread is stopped.
static void Cleanup(void)
{
    SDL_free(passthrough.queue);
    SDL_DestroyMutex(passthrough.lock);
    SDL_zero(passthrough);
}

// Whether the pointer is over the window's own input, so it has the mouse focus natively.
static bool IsPointerOverOwnInput(SDL_Window *window)
{
    const SDL_WindowData *data = window->internal;
    POINT pt;
    HWND hwnd;

    if (data->input_transparent || !GetCursorPos(&pt)) {
        return false;
    }
    hwnd = WindowFromPoint(pt);
    return hwnd && GetAncestor(hwnd, GA_ROOT) == data->hwnd;
}

bool WIN_SetWindowMousePassthroughEvents(SDL_VideoDevice *_this, SDL_Window *window, bool enabled)
{
    // Only the window being disabled can disable during the stop, as it was the last one.
    if (passthrough.stopping) {
        return enabled ? SDL_SetError("Mouse passthrough events are stopping") : true;
    }

    if (enabled) {
        if (passthrough.num_windows == 0) {
            passthrough.lock = SDL_CreateMutex();
            if (!passthrough.lock) {
                return false;
            }
            passthrough.wake_message = RegisterWindowMessage(TEXT("SDL_MousePassthroughWake"));
            passthrough.main_thread_id = GetCurrentThreadId();
            if (!StartThread(_this)) {
                Cleanup();
                return false;
            }
        }
        ++passthrough.num_windows;
        return true;
    }

    --passthrough.num_windows;
    if (passthrough.num_windows == 0) {
        // The hook takes the lock, so nothing here holds it while waiting for the thread.
        passthrough.stopping = true;
        StopThread();
        Cleanup();
    }

    // The window keeps a focus it received natively.
    if (SDL_GetMouseFocus() == window && !IsPointerOverOwnInput(window)) {
        SDL_SetMouseFocus(NULL);
    }
    return true;
}

static bool WIN_LocatePassthroughPointer(SDL_Window *window, void *userdata, float *x, float *y)
{
    const SDL_WindowData *data = window->internal;
    POINT pt = *(const POINT *)userdata;

    // The hook reports physical coordinates. A window that is not per-monitor DPI aware uses logical ones; for one that is, this changes nothing.
    if (data->videodata->PhysicalToLogicalPointForPerMonitorDPI && !data->videodata->PhysicalToLogicalPointForPerMonitorDPI(data->hwnd, &pt)) {
        return false;
    }
    // Client coordinates, as WM_MOUSEMOVE reports.
    if (!ScreenToClient(data->hwnd, &pt)) {
        return false;
    }
    *x = (float)pt.x;
    *y = (float)pt.y;
    return true;
}

static Uint8 GetButton(const PassthroughInput *input, bool *down)
{
    switch (input->message) {
    case WM_LBUTTONDOWN:
        *down = true;
        return SDL_BUTTON_LEFT;
    case WM_LBUTTONUP:
        *down = false;
        return SDL_BUTTON_LEFT;
    case WM_RBUTTONDOWN:
        *down = true;
        return SDL_BUTTON_RIGHT;
    case WM_RBUTTONUP:
        *down = false;
        return SDL_BUTTON_RIGHT;
    case WM_MBUTTONDOWN:
        *down = true;
        return SDL_BUTTON_MIDDLE;
    case WM_MBUTTONUP:
        *down = false;
        return SDL_BUTTON_MIDDLE;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        *down = (input->message == WM_XBUTTONDOWN);
        return (input->xbutton == XBUTTON1) ? SDL_BUTTON_X1 : (input->xbutton == XBUTTON2) ? SDL_BUTTON_X2 : 0;
    default:
        return 0;
    }
}

void WIN_PumpMousePassthroughEvents(SDL_VideoDevice *_this)
{
    PassthroughInput *queue;
    int num_queued;
    int i;

    if (passthrough.num_windows == 0) {
        return;
    }

    // Report outside the lock, so the hook never waits for SDL.
    SDL_LockMutex(passthrough.lock);
    queue = passthrough.queue;
    num_queued = passthrough.num_queued;
    passthrough.queue = NULL;
    passthrough.num_queued = 0;
    passthrough.queue_capacity = 0;
    SDL_UnlockMutex(passthrough.lock);

    for (i = 0; i < num_queued; ++i) {
        PassthroughInput *input = &queue[i];
        if (input->message == WM_MOUSEMOVE) {
            SDL_SendPassthroughMouseMotion(0, WIN_LocatePassthroughPointer, &input->pt);
        } else {
            bool down = false;
            const Uint8 button = GetButton(input, &down);
            SDL_SendPassthroughMouseButton(0, WIN_LocatePassthroughPointer, &input->pt, button, down);
        }
    }
    SDL_free(queue);
}

#endif // defined(SDL_VIDEO_DRIVER_WINDOWS) && !defined(SDL_PLATFORM_XBOXONE) && !defined(SDL_PLATFORM_XBOXSERIES)
