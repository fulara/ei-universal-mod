// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// um.dll: BORDERLESS - the windowed game behaves like a borderless full screen window (RE: App_WndProc 0x457D40).
// The game's window procedure treats every WM_ACTIVATE with WA_INACTIVE (an alt-tab, a click on another window) as a loss of the
// screen: it marks itself inactive (the main loop then stops ticking: a multiplayer game desyncs), releases the DirectInput mouse,
// pauses all sound and minimizes its window (ShowWindow SW_MINIMIZE), even in windowed mode. It also never confines the cursor
// to its window, so the camera cannot be scrolled with the mouse at the screen edge. Here the game's window is subclassed:
//   - the deactivation never reaches the game (it stays active, does not minimize and keeps running in the background);
//   - while focused the cursor is confined unless released by Ctrl+left Alt; focus changes, window moves and the watchdog respect that release until a client click.
// Needs the game's own setting fullscreen=0 (the window is a plain WS_POPUP at 0,0 of the game's resolution).
// SCALE (um.cfg BORDERLESS_SCALE / um-window.ini Scale=1): a game resolution lower than the monitor's fills the screen. The window gets the size of the game
// picture scaled by the largest factor that fits the monitor (aspect ratio kept), centred, with a black window behind it for the bars. D7VK stretches its frame to
// the window by itself; with any other DirectDraw the primary surface's Blt / BltFast are turned into stretching Blt calls. The game takes its mouse position
// from GetCursorPos / SetCursorPos and from the position in the mouse messages, all as if the window sat at 0,0 at the game's size: those are mapped.
// BORDERLESS_FRAME adds a fixed-size native caption at the game resolution. Native DirectDraw presents a client-sized front buffer to the HWND, independent of monitor coordinates.
// Self-contained: only LogLine and the import patch of CreateWindowExA are shared with the rest of um.dll. um-launcher builds this file
// on its own (UM_WINDOW_STANDALONE, tools/um-launcher/window/) as um-window.asi for mods that do not use um.dll.
#ifdef UM_WINDOW_STANDALONE
#include "um_window_standalone.hpp"
#else
#include "um_internal.hpp"
#endif
#include <algorithm>
#include <atomic>
#include <ddraw.h>
#include "um_cursor_map.hpp"

bool g_enableBorderless = false;
bool g_enableScale = false; // SCALE
bool g_enableFrame = false;
static bool g_frameActive = false;
static std::atomic<bool> g_mouseReleased{false}, g_windowMoving{false};
static POINT g_virtualCursor = {0, 0}; // frozen input while the real cursor belongs to the desktop
static int g_cursorShows = 0;
static bool g_eatCaptureUp = false;
static DWORD g_gameMouseButtons = 0, g_pendingButtonUps = 0, g_resetDoubleClicks = 0;
static LPARAM g_lastClientMouse = 0;
static const WPARAM kReleaseButtonUp = 0x80000000u; // internal, never exposed to the game's input handler
static int g_gameW = 0, g_gameH = 0;     // the game's own window size (its resolution)
static int g_winX = 0, g_winY = 0;       // the mapped client area's origin on the screen
static double g_scale = 1.0;             // the scaled window's size / the game's
static bool g_scaleActive = false;
static CursorMap g_cursor;               // GetCursorPos / SetCursorPos mapping
static CRITICAL_SECTION g_cursorLock;
static bool g_cursorLockReady = false;
static RECT g_monitorRect = {0, 0, 0, 0}, g_gameRect = {0, 0, 0, 0};
static HWND g_bars[4] = {NULL, NULL, NULL, NULL}; // the black bars around the scaled window (they never overlap it)
static bool g_scaleByBlt = false;        // native DirectDraw needs presentation hooks (scaled desktop or framed client), unlike D7VK

static HWND g_gameWindow = NULL;
static WNDPROC g_originalProc = NULL;
static bool g_clipped = false;

static bool WindowClientOnScreen(RECT& screen) {
    RECT client;
    if (!g_gameWindow || !GetClientRect(g_gameWindow, &client)) return false;
    POINT a = {client.left, client.top}, b = {client.right, client.bottom};
    ClientToScreen(g_gameWindow, &a); ClientToScreen(g_gameWindow, &b);
    screen.left = a.x; screen.top = a.y; screen.right = b.x; screen.bottom = b.y;
    return true;
}

static void WindowClip(bool on) {
    if (!g_gameWindow) return;
    if (on && (g_mouseReleased.load() || g_windowMoving.load() || IsIconic(g_gameWindow) ||
               GetForegroundWindow() != g_gameWindow)) on = false;
    if (!on) {
        if (g_clipped) { ClipCursor(NULL); g_clipped = false; }
        return;
    }
    RECT screen;
    if (!WindowClientOnScreen(screen)) return;
    if (ClipCursor(&screen)) g_clipped = true;
}

static bool ReleaseKeysDown();
static void ReleaseMouse(const char* reason = "Ctrl+left Alt");
static bool SetMouseDeviceAcquired(bool on);
static bool PaintClientPrimary(HWND window);
static void ReleaseMouseDevice();

static bool WindowMapsCoordinates() { return g_scaleActive || g_frameActive; }

static void UpdateWindowMapping(bool reset = false) {
    RECT screen;
    if (!WindowClientOnScreen(screen) || IsIconic(g_gameWindow) ||
        screen.right <= screen.left || screen.bottom <= screen.top) return;
    if (!reset && screen.left == g_winX && screen.top == g_winY) return;
    EnterCriticalSection(&g_cursorLock);
    g_winX = screen.left; g_winY = screen.top;
    g_cursor.SetGeometry(g_winX, g_winY, g_gameW, g_gameH, g_scale);
    LeaveCriticalSection(&g_cursorLock);
}

// The confinement is also checked on a timer (four times a second): the system drops it by itself (an overlay, another program, a desktop switch, a focus change that sends no
// message to the game) and a ClipCursor made before the window had its final place or its X focus (Wine) can fail or be wrong; none of that sends the game window a message.
// While the game window is the foreground window and the cursor is not confined to it, it is confined again; when the game is not in front, a confinement of ours is released.
static const UINT_PTR kClipWatchTimer = 0x5CA20;
static void WindowClipWatch() {
    if (!g_gameWindow) return;
    if (GetForegroundWindow() == g_gameWindow && ReleaseKeysDown()) ReleaseMouse();
    if (GetForegroundWindow() != g_gameWindow || IsIconic(g_gameWindow) ||
        g_mouseReleased.load() || g_windowMoving.load()) {
        if (g_clipped) WindowClip(false);
        return;
    }
    RECT want, now;
    if (!WindowClientOnScreen(want)) return;
    if (GetClipCursor(&now) && EqualRect(&now, &want)) { g_clipped = true; return; }
    g_clipped = false;
    const bool ok = ClipCursor(&want) != 0;
    g_clipped = ok;
    static DWORD lastLog = 0;
    const DWORD tick = GetTickCount();
    if (tick - lastLog > 30000) {
        lastLog = tick;
        LogLine(ok ? "INFO" : "WARN", "BORDERLESS: the cursor was free while the game window has the focus: %s", ok ? "confined to it again" : "ClipCursor failed");
    }
}

static void CreateBars(const RECT& monitor, const RECT& game);
static bool g_topmost = false;
// The scaled picture covers the whole monitor height or width; the taskbar is topmost and would cover its edge: the game window and the bars are topmost while the game is active only.
static void SetScaleTopmost(bool on) {
    if (!g_scaleActive || g_topmost == on) return;
    g_topmost = on;
    LogLine("INFO", "SCALE: the window is %s", on ? "topmost (the game is active)" : "no longer topmost (the game lost the focus)");
    HWND after = on ? HWND_TOPMOST : HWND_NOTOPMOST;
    for (HWND bar : g_bars) if (bar) SetWindowPos(bar, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (g_gameWindow) SetWindowPos(g_gameWindow, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void DestroyBars() {
    for (HWND& bar : g_bars) if (bar) { DestroyWindow(bar); bar = NULL; }
}

// Screen -> game coordinates (the game's window is taken to sit at 0,0 at its own size), for positions that are not the pointer's (the wheel message's).
static void ScaleToGame(LONG& x, LONG& y) {
    x = static_cast<LONG>((x - g_winX) / g_scale); y = static_cast<LONG>((y - g_winY) / g_scale);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > g_gameW - 1) x = g_gameW - 1;
    if (y > g_gameH - 1) y = g_gameH - 1;
}

static BOOL (WINAPI *g_originalGetCursorPos)(LPPOINT) = NULL;
static BOOL (WINAPI *g_originalSetCursorPos)(int, int) = NULL;
static bool WindowAcceptsMouse() {
    return g_gameWindow && GetForegroundWindow() == g_gameWindow && !IsIconic(g_gameWindow) &&
           !g_mouseReleased.load() && !g_windowMoving.load();
}

static BOOL WINAPI HookedGetCursorPos(LPPOINT p) {
    const BOOL ok = g_originalGetCursorPos(p);
    if (ok && p && g_gameWindow && g_cursorLockReady) {
        const bool active = WindowAcceptsMouse();
        EnterCriticalSection(&g_cursorLock);
        if (!active) *p = g_virtualCursor;
        else {
            if (WindowMapsCoordinates()) {
                long gx, gy;
                g_cursor.ToGame(p->x, p->y, gx, gy);
                p->x = gx; p->y = gy;
            }
            g_virtualCursor = *p;
        }
        LeaveCriticalSection(&g_cursorLock);
    }
    return ok;
}

static BOOL WINAPI HookedSetCursorPos(int x, int y) {
    if (g_gameWindow && g_cursorLockReady) {
        const bool active = WindowAcceptsMouse();
        EnterCriticalSection(&g_cursorLock);
        g_virtualCursor.x = x; g_virtualCursor.y = y;
        if (active && WindowMapsCoordinates()) {
            long sx, sy;
            g_cursor.ToScreen(x, y, sx, sy);
            x = static_cast<int>(sx); y = static_cast<int>(sy);
        }
        LeaveCriticalSection(&g_cursorLock);
        if (!active) return TRUE; // a background/released game must never warp the desktop pointer
    }
    return g_originalSetCursorPos(x, y);
}

static bool ReleaseKeysDown() {
    // Right Alt is AltGr on Polish and other layouts, not the release shortcut.
    return (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_LMENU) & 0x8000);
}

static DWORD MouseDownButton(UINT message) {
    switch (message) {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: return MK_LBUTTON;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: return MK_RBUTTON;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: return MK_MBUTTON;
    default: return 0;
    }
}

static DWORD MouseUpButton(UINT message) {
    switch (message) {
    case WM_LBUTTONUP: return MK_LBUTTON;
    case WM_RBUTTONUP: return MK_RBUTTON;
    case WM_MBUTTONUP: return MK_MBUTTON;
    default: return 0;
    }
}

static void RestoreCursorVisibility() {
    while (g_cursorShows > 0) { ShowCursor(FALSE); --g_cursorShows; }
}

static void ReleaseMouse(const char* reason) {
    if (!g_gameWindow || g_mouseReleased.exchange(true)) return;
    // Finish only presses the game actually received. The physical up can go
    // to another application after ReleaseCapture; dropping it leaves a drag stuck.
    static const struct { DWORD button; UINT message; } releases[] = {
        {MK_LBUTTON, WM_LBUTTONUP}, {MK_RBUTTON, WM_RBUTTONUP}, {MK_MBUTTON, WM_MBUTTONUP}
    };
    g_pendingButtonUps |= g_gameMouseButtons;
    for (const auto& release : releases) {
        if ((g_gameMouseButtons & release.button) &&
            !PostMessageA(g_gameWindow, release.message, kReleaseButtonUp, g_lastClientMouse))
            LogLine("ERROR", "WINDOW: could not enqueue a held mouse button release");
    }
    g_eatCaptureUp = false;
    g_resetDoubleClicks = 0;
    EnterCriticalSection(&g_cursorLock);
    g_virtualCursor.x = g_gameW / 2; g_virtualCursor.y = g_gameH / 2;
    LeaveCriticalSection(&g_cursorLock);
    SetMouseDeviceAcquired(false);
    ClipCursor(NULL); g_clipped = false;
    ReleaseCapture();
    int visible;
    do { visible = ShowCursor(TRUE); ++g_cursorShows; } while (visible < 0);
    SetCursor(LoadCursorA(NULL, IDC_ARROW));
    LogLine("INFO", "WINDOW: mouse released (%s); click inside the game to capture it again", reason);
}

static bool CaptureMouse() {
    UpdateWindowMapping(true);
    g_mouseReleased.store(false);
    if (!SetMouseDeviceAcquired(true)) { g_mouseReleased.store(true); return false; }
    RestoreCursorVisibility();
    WindowClip(true);
    LogLine("INFO", "WINDOW: mouse captured by a client click (click consumed)");
    return true;
}

static void CaptureOnActivation() {
    if (GetForegroundWindow() != g_gameWindow) { WindowClip(false); return; }
    POINT point;
    RECT client;
    const bool inside = WindowClientOnScreen(client) &&
        (g_originalGetCursorPos ? g_originalGetCursorPos(&point) : GetCursorPos(&point)) &&
        PtInRect(&client, point);
    if (g_frameActive && !inside) ReleaseMouse("activation outside the game client");
    else {
        if (WindowAcceptsMouse()) SetMouseDeviceAcquired(true);
        WindowClip(true);
    }
}

// Maps the position of a mouse message (client coordinates of the scaled window, or screen coordinates for the wheel) to the game's coordinates.
static void MapMouseMessage(UINT message, LPARAM& lParam) {
    if ((message >= WM_MOUSEMOVE && message <= WM_MBUTTONDBLCLK && message != WM_MOUSEWHEEL) || (message >= WM_XBUTTONDOWN && message <= WM_XBUTTONDBLCLK)) {
        LONG gx = static_cast<LONG>(static_cast<short>(LOWORD(lParam)) / g_scale), gy = static_cast<LONG>(static_cast<short>(HIWORD(lParam)) / g_scale);
        gx = (std::max)(0L, (std::min)(gx, static_cast<LONG>(g_gameW - 1)));
        gy = (std::max)(0L, (std::min)(gy, static_cast<LONG>(g_gameH - 1)));
        lParam = MAKELPARAM(static_cast<WORD>(gx), static_cast<WORD>(gy));
    } else if (message == WM_MOUSEWHEEL) {
        LONG gx = static_cast<short>(LOWORD(lParam)), gy = static_cast<short>(HIWORD(lParam));
        ScaleToGame(gx, gy);
        lParam = MAKELPARAM(static_cast<WORD>(gx), static_cast<WORD>(gy));
    }
}

static void ProcessWindowMessage(MSG& msg) {
    if (!g_gameWindow || msg.hwnd != g_gameWindow) return;
    // App_HandleInputMessage calls the game's original App_WndProc directly instead of
    // DispatchMessage. Native caption actions and our timers must reach the subclass.
    const UINT command = static_cast<UINT>(msg.wParam & 0xFFF0);
    const bool frameCommand = g_frameActive &&
        (msg.message == WM_PAINT || (msg.message >= WM_NCMOUSEMOVE && msg.message <= WM_NCXBUTTONDBLCLK) ||
         (msg.message == WM_SYSCOMMAND && (command == SC_MOVE || command == SC_MINIMIZE ||
                                          command == SC_RESTORE || command == SC_CLOSE)));
    const bool ownTimer = msg.message == WM_TIMER && (msg.wParam == kClipWatchTimer || msg.wParam == 0x5CA1E);
    if (frameCommand || ownTimer) {
        DispatchMessageA(&msg);
        msg.message = WM_NULL;
        return;
    }
    if ((msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) && ReleaseKeysDown() &&
        GetForegroundWindow() == g_gameWindow) {
        ReleaseMouse();
        msg.message = WM_NULL;
        return;
    }
    if (msg.message >= WM_MOUSEMOVE && msg.message <= WM_MOUSEHWHEEL) {
        const DWORD up = MouseUpButton(msg.message);
        const bool internalUp = up && (msg.wParam & kReleaseButtonUp);
        if (internalUp) msg.wParam &= ~kReleaseButtonUp;
        if (up && (g_pendingButtonUps & up)) {
            g_pendingButtonUps &= ~up;
            g_gameMouseButtons &= ~up;
            if (!internalUp && msg.message == WM_LBUTTONUP) g_eatCaptureUp = false;
            if (WindowMapsCoordinates()) MapMouseMessage(msg.message, msg.lParam);
            return;
        }
        if (internalUp) { msg.message = WM_NULL; return; } // a physical up already ended this press
        if (g_mouseReleased.load()) {
            if (msg.message == WM_LBUTTONDOWN && GetForegroundWindow() == g_gameWindow && !ReleaseKeysDown()) {
                const POINT point = {static_cast<short>(LOWORD(msg.lParam)), static_cast<short>(HIWORD(msg.lParam))};
                RECT client;
                if (GetClientRect(g_gameWindow, &client) && PtInRect(&client, point)) {
                    g_eatCaptureUp = CaptureMouse();
                    if (g_eatCaptureUp) g_resetDoubleClicks = MK_LBUTTON | MK_RBUTTON | MK_MBUTTON;
                }
            }
            msg.message = WM_NULL;
            return;
        }
        if (g_eatCaptureUp) {
            if (msg.message == WM_LBUTTONUP) g_eatCaptureUp = false;
            msg.message = WM_NULL;
            return;
        }
        const DWORD down = MouseDownButton(msg.message);
        if (down && (g_resetDoubleClicks & down)) {
            // The capture click was swallowed, so a subsequent OS double-click
            // is the game's first click, not a double-click on its interface.
            if (msg.message == WM_LBUTTONDBLCLK) msg.message = WM_LBUTTONDOWN;
            else if (msg.message == WM_RBUTTONDBLCLK) msg.message = WM_RBUTTONDOWN;
            else if (msg.message == WM_MBUTTONDBLCLK) msg.message = WM_MBUTTONDOWN;
            g_resetDoubleClicks &= ~down;
        }
        g_gameMouseButtons = (g_gameMouseButtons | down) & ~up;
        if (msg.message <= WM_MBUTTONDBLCLK) g_lastClientMouse = msg.lParam; // before scaling
        if (WindowMapsCoordinates()) MapMouseMessage(msg.message, msg.lParam);
    }
}

// The game's loop takes its messages from GetMessageA / PeekMessageA and passes them to its own input handler without dispatching them to the
// window procedure: the positions are mapped here.
static BOOL (WINAPI *g_originalGetMessageA)(LPMSG, HWND, UINT, UINT) = NULL;
static BOOL (WINAPI *g_originalPeekMessageA)(LPMSG, HWND, UINT, UINT, UINT) = NULL;
static BOOL WINAPI HookedGetMessageA(LPMSG msg, HWND window, UINT min, UINT max) {
    BOOL r = g_originalGetMessageA(msg, window, min, max);
    if (r > 0 && msg) ProcessWindowMessage(*msg);
    return r;
}
static BOOL WINAPI HookedPeekMessageA(LPMSG msg, HWND window, UINT min, UINT max, UINT remove) {
    BOOL r = g_originalPeekMessageA(msg, window, min, max, remove);
    if (r && (remove & PM_REMOVE) && msg) ProcessWindowMessage(*msg); // peeking alone must not capture/release or map twice
    return r;
}

static LRESULT CALLBACK BorderlessProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    static const bool control = getenv("UM_WINTEST_RAW") != NULL; // research: the hook installed but passing everything (the game's own behaviour)
    if (control) return CallWindowProcA(g_originalProc, window, message, wParam, lParam);
    if (g_scaleActive) { // (the mouse messages are mapped where the game takes them from its queue, GetMessageA / PeekMessageA: mapping them again here would scale them twice)
        if (message == WM_ERASEBKGND) { // black, not whatever the class brush is
            RECT r; GetClientRect(window, &r); FillRect(reinterpret_cast<HDC>(wParam), &r, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            return 1;
        }
    }
    if (message == WM_TIMER && wParam == kClipWatchTimer) { WindowClipWatch(); return 0; }
    if (message == WM_TIMER && wParam == 0x5CA1E) { KillTimer(window, 0x5CA1E); if (g_scaleActive) CreateBars(g_monitorRect, g_gameRect); return 0; }
    switch (message) {
    case WM_PAINT:
        if (PaintClientPrimary(window)) return 0;
        break;
    case WM_NCHITTEST:
        if (g_frameActive) return DefWindowProcA(window, message, wParam, lParam);
        break;
    case WM_NCLBUTTONDOWN:
        if (g_frameActive) {
            WindowClip(false);
            return DefWindowProcA(window, message, wParam, lParam);
        }
        break;
    case WM_SYSCOMMAND:
        if (g_frameActive && ((wParam & 0xFFF0) == SC_MOVE || (wParam & 0xFFF0) == SC_MINIMIZE ||
                             (wParam & 0xFFF0) == SC_RESTORE || (wParam & 0xFFF0) == SC_CLOSE))
            return DefWindowProcA(window, message, wParam, lParam);
        break;
    case WM_ENTERSIZEMOVE:
        g_windowMoving.store(true);
        SetMouseDeviceAcquired(false);
        WindowClip(false);
        break;
    case WM_EXITSIZEMOVE:
        UpdateWindowMapping(true);
        g_windowMoving.store(false);
        if (WindowAcceptsMouse()) SetMouseDeviceAcquired(true);
        WindowClip(true);
        break;
    case WM_SETCURSOR:
        if (g_frameActive && LOWORD(lParam) != HTCLIENT)
            return DefWindowProcA(window, message, wParam, lParam);
        if (g_mouseReleased.load() && LOWORD(lParam) == HTCLIENT) {
            SetCursor(LoadCursorA(NULL, IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_ACTIVATE:
        SetScaleTopmost(LOWORD(wParam) != WA_INACTIVE);
        if (LOWORD(wParam) == WA_INACTIVE) { // the game must not see it
            WindowClip(false);
            SetMouseDeviceAcquired(false);
            return 0;
        }
        UpdateWindowMapping(true);
        CaptureOnActivation();
        break;
    case WM_SETFOCUS:
        CaptureOnActivation();
        break;
    case WM_KILLFOCUS:
        WindowClip(false);
        SetMouseDeviceAcquired(false);
        break;
    case WM_SIZE:
    case WM_MOVE:
    case WM_DISPLAYCHANGE:
        if (GetForegroundWindow() == window) WindowClip(true);
        UpdateWindowMapping();
        break;
    case WM_DESTROY:
        KillTimer(window, kClipWatchTimer);
        WindowClip(false);
        ReleaseMouseDevice();
        DestroyBars();
        RestoreCursorVisibility();
        g_mouseReleased.store(false);
        g_windowMoving.store(false);
        g_eatCaptureUp = false;
        g_gameMouseButtons = g_pendingButtonUps = g_resetDoubleClicks = 0;
        g_lastClientMouse = 0;
        g_frameActive = false;
        g_gameWindow = NULL;
        g_scaleActive = false;
        g_topmost = false;
        break;
    }
    return CallWindowProcA(g_originalProc, window, message, wParam, lParam);
}

typedef HWND (WINAPI *CreateWindowExAFunction)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
static CreateWindowExAFunction g_originalCreateWindowExA = NULL;

// Research: UM_WINTEST=<seconds> sends the game's window a deactivation after that long and an activation 4 s later (what an alt-tab does).
static DWORD WINAPI WindowTestThread(LPVOID) {
    const char* seconds = getenv("UM_WINTEST");
    if (!seconds) return 0;
    Sleep(static_cast<DWORD>(atof(seconds) * 1000));
    LogLine("INFO", "BORDERLESS test: sending WM_ACTIVATE inactive to the game window");
    SendMessageA(g_gameWindow, WM_ACTIVATE, WA_INACTIVE, 0);
    Sleep(4000);
    LogLine("INFO", "BORDERLESS test: sending WM_ACTIVATE active");
    SendMessageA(g_gameWindow, WM_ACTIVATE, WA_ACTIVE, 0);
    return 0;
}

// The black bars: up to four plain black windows around the scaled game window.
static void CreateBars(const RECT& monitor, const RECT& game) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSA wc; memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "UmScaleBar";
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        RegisterClassA(&wc); registered = true;
    }
    const RECT rects[4] = {{monitor.left, monitor.top, game.left, monitor.bottom}, {game.right, monitor.top, monitor.right, monitor.bottom},
                           {game.left, monitor.top, game.right, game.top}, {game.left, game.bottom, game.right, monitor.bottom}};
    for (int i = 0; i < 4; ++i) {
        const RECT& r = rects[i];
        if (r.right - r.left <= 0 || r.bottom - r.top <= 0) continue;
        g_bars[i] = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, "UmScaleBar", "", WS_POPUP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                                    NULL, NULL, GetModuleHandleA(NULL), NULL);
        if (g_bars[i]) { ShowWindow(g_bars[i], SW_SHOWNOACTIVATE); if (g_topmost) SetWindowPos(g_bars[i], HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); }
    }
}

static HWND WINAPI HookedCreateWindowExA(DWORD exStyle, LPCSTR className, LPCSTR title, DWORD style, int x, int y, int w, int h,
                                         HWND parent, HMENU menu, HINSTANCE instance, LPVOID param) {
    const bool gameWindow = !g_gameWindow && !parent && (style & WS_POPUP) && w >= 640 && h >= 480; // the game's main window
    if (gameWindow) {
        g_gameW = w; g_gameH = h; g_scale = 1.0;
        g_frameActive = g_enableFrame;
    }
    if (gameWindow && g_frameActive) {
        style = (style & ~(WS_POPUP | WS_THICKFRAME | WS_MAXIMIZEBOX)) | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        exStyle &= ~WS_EX_TOPMOST;
        RECT outer = {0, 0, w, h};
        AdjustWindowRectEx(&outer, style, menu != NULL, exStyle);
        w = outer.right - outer.left; h = outer.bottom - outer.top;
        MONITORINFO mi = {}; mi.cbSize = sizeof mi;
        const POINT origin = {x, y};
        if (GetMonitorInfoA(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            x = mi.rcWork.left + (std::max)(0L, (mi.rcWork.right - mi.rcWork.left - w) / 2);
            y = mi.rcWork.top + (std::max)(0L, (mi.rcWork.bottom - mi.rcWork.top - h) / 2);
        }
        if (!title || !*title) title = "Evil Islands";
    }
    if (gameWindow && g_enableScale && !g_frameActive) {
        POINT origin = {x, y};
        MONITORINFO mi; memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
        RECT monitor = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        if (GetMonitorInfoA(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi)) monitor = mi.rcMonitor;
        const int mw = monitor.right - monitor.left, mh = monitor.bottom - monitor.top;
        const double scale = (std::min)(static_cast<double>(mw) / w, static_cast<double>(mh) / h);
        if (scale > 1.001) {
            g_gameW = w; g_gameH = h; g_scale = scale;
            int nw = static_cast<int>(w * scale + 0.5), nh = static_cast<int>(h * scale + 0.5);
            if (nw > mw) nw = mw;
            if (nh > mh) nh = mh;
            g_winX = monitor.left + (mw - nw) / 2; g_winY = monitor.top + (mh - nh) / 2;
            LogLine("INFO", "SCALE: the game window %dx%d becomes %dx%d (x%.3f) at %d,%d on the %dx%d monitor", w, h, nw, nh, scale, g_winX, g_winY, mw, mh);
            g_monitorRect = monitor; g_gameRect.left = g_winX; g_gameRect.top = g_winY; g_gameRect.right = g_winX + nw; g_gameRect.bottom = g_winY + nh;
            x = g_winX; y = g_winY; w = nw; h = nh;
            g_cursor.SetGeometry(g_winX, g_winY, g_gameW, g_gameH, scale);
            g_scaleActive = true;
        } else LogLine("INFO", "SCALE: the game window %dx%d is not smaller than the %dx%d monitor: nothing to scale", w, h, mw, mh);
    }
    HWND window = g_originalCreateWindowExA(exStyle, className, title, style, x, y, w, h, parent, menu, instance, param);
    if (window && gameWindow) {
        g_gameWindow = window;
        UpdateWindowMapping(true);
        if (g_frameActive) LogLine("INFO", "WINDOW: framed client %dx%d at %d,%d; movable native caption, no live resizing", g_gameW, g_gameH, g_winX, g_winY);
        g_originalProc = reinterpret_cast<WNDPROC>(SetWindowLongA(window, GWL_WNDPROC, reinterpret_cast<LONG>(BorderlessProc)));
        LogLine("INFO", "BORDERLESS: the game window %dx%d at %d,%d is kept active when it loses the focus, and the cursor is confined to it while it has it", w, h, x, y);
        SetTimer(window, kClipWatchTimer, 250, NULL);
        if (g_scaleActive) SetTimer(window, 0x5CA1E, 1500, NULL); // the bars after the game window has been shown
        if (getenv("UM_WINTEST")) CreateThread(NULL, 0, WindowTestThread, NULL, 0, NULL);
    }
    return window;
}

// ---- DirectDraw, for the DirectDraw that does not stretch by itself ----
// IDirectDraw7 slots: CreateSurface 6, SetCooperativeLevel 20. IDirectDrawSurface7 slots: Blt 5, BltFast 7, GetSurfaceDesc 22.
typedef HRESULT (WINAPI *CreateSurfaceFn)(void*, void*, void**, IUnknown*);
typedef HRESULT (WINAPI *SetCoopFn)(void*, HWND, DWORD);
typedef HRESULT (WINAPI *BltFn)(void*, RECT*, void*, RECT*, DWORD, void*);
typedef HRESULT (WINAPI *BltFastFn)(void*, DWORD, DWORD, void*, RECT*, DWORD);
typedef HRESULT (WINAPI *GetSurfaceDescFn)(void*, void*);
static CreateSurfaceFn g_originalCreateSurface = NULL;
static SetCoopFn g_originalSetCoop = NULL;
static BltFn g_originalBlt = NULL;
static BltFastFn g_originalBltFast = NULL;
static void* g_primary = NULL;

// A windowed DirectDraw primary is still only as large as the primary monitor.
// Keep the framed game's front buffer in client coordinates and present to its
// HWND instead; native Blt retains colour keys, fills and pixel-format conversion.
static IDirectDrawSurface7* g_clientPrimary = NULL;
typedef ULONG (WINAPI *SurfaceReleaseFn)(void*);
typedef HRESULT (WINAPI *SurfaceLockFn)(void*, RECT*, void*, DWORD, HANDLE);
typedef HRESULT (WINAPI *SurfaceUnlockFn)(void*, RECT*);
typedef HRESULT (WINAPI *SurfaceGetDcFn)(void*, HDC*);
typedef HRESULT (WINAPI *SurfaceReleaseDcFn)(void*, HDC);
static SurfaceReleaseFn g_originalSurfaceRelease = NULL;
static SurfaceLockFn g_originalSurfaceLock = NULL;
static SurfaceUnlockFn g_originalSurfaceUnlock = NULL;
static SurfaceGetDcFn g_originalSurfaceGetDC = NULL;
static SurfaceReleaseDcFn g_originalSurfaceReleaseDC = NULL;
static RECT g_clientLockedRect = {};
static bool g_clientLockWrites = false;

// Writes `replacement` into a vtable slot of the object's class and returns what was there (the previous hook's, if any).
static void* WindowPatchSlot(void* object, int slot, void* replacement) {
    if (!object || IsBadReadPtr(object, sizeof(void*))) return NULL;
    void** vtable = *reinterpret_cast<void***>(object);
    if (!vtable || IsBadReadPtr(vtable + slot, sizeof(void*))) return NULL;
    DWORD old = 0;
    if (!VirtualProtect(vtable + slot, sizeof(void*), PAGE_READWRITE, &old)) return NULL;
    void* original = vtable[slot];
    vtable[slot] = replacement;
    VirtualProtect(vtable + slot, sizeof(void*), old, &old);
    return original;
}

// DirectInput's acquired mouse can keep the native Wine cursor grabbed after ClipCursor(NULL).
// Retain the real mouse device so release/capture controls both layers, without pausing the game.
static void* g_mouseDevice = NULL;
typedef HRESULT (WINAPI *MouseAcquireFn)(void*);
static MouseAcquireFn g_originalMouseAcquire = NULL;

static HRESULT WINAPI HookedMouseAcquire(void* self) {
    if (self == g_mouseDevice && !WindowAcceptsMouse())
        return S_FALSE; // acquisition intentionally deferred until the user captures the client
    return g_originalMouseAcquire(self);
}

static bool SetMouseDeviceAcquired(bool on) {
    if (!g_mouseDevice) return true;
    void** vtable = *reinterpret_cast<void***>(g_mouseDevice);
    const HRESULT result = on ? reinterpret_cast<MouseAcquireFn>(vtable[7])(g_mouseDevice)
                              : reinterpret_cast<MouseAcquireFn>(vtable[8])(g_mouseDevice);
    if (FAILED(result)) {
        LogLine("WARN", "WINDOW: DirectInput mouse %s failed: 0x%08lX", on ? "acquire" : "unacquire", result);
        return false;
    }
    return true;
}

static void ReleaseMouseDevice() {
    if (!g_mouseDevice) return;
    SetMouseDeviceAcquired(false);
    void* device = g_mouseDevice;
    g_mouseDevice = NULL;
    void** vtable = *reinterpret_cast<void***>(device);
    reinterpret_cast<ULONG (WINAPI*)(void*)>(vtable[2])(device);
}

static void TrackMouseDevice(const GUID& guid, void* device) {
    static const GUID mouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
    if (!device || memcmp(&guid, &mouse, sizeof mouse) != 0 || device == g_mouseDevice) return;
    ReleaseMouseDevice();
    void** vtable = *reinterpret_cast<void***>(device);
    reinterpret_cast<ULONG (WINAPI*)(void*)>(vtable[1])(device); // own one reference until window teardown
    g_mouseDevice = device;
    void* previous = WindowPatchSlot(device, 7, reinterpret_cast<void*>(HookedMouseAcquire));
    if (previous && previous != reinterpret_cast<void*>(HookedMouseAcquire))
        g_originalMouseAcquire = reinterpret_cast<MouseAcquireFn>(previous);
    LogLine("INFO", "WINDOW: DirectInput mouse release/capture installed");
}

typedef HRESULT (WINAPI *InputCreateDeviceFn)(void*, const GUID&, void**, IUnknown*);
typedef HRESULT (WINAPI *InputCreateDeviceExFn)(void*, const GUID&, const GUID&, void**, IUnknown*);
typedef HRESULT (WINAPI *InputCreateExFn)(HINSTANCE, DWORD, const GUID&, void**, IUnknown*);
static InputCreateDeviceFn g_originalInputCreateDevice = NULL;
static InputCreateDeviceExFn g_originalInputCreateDeviceEx = NULL;
static InputCreateExFn g_originalInputCreateEx = NULL;

static HRESULT WINAPI HookedInputCreateDevice(void* self, const GUID& guid, void** device, IUnknown* outer) {
    const HRESULT result = g_originalInputCreateDevice(self, guid, device, outer);
    if (SUCCEEDED(result) && device) TrackMouseDevice(guid, *device);
    return result;
}

static HRESULT WINAPI HookedInputCreateDeviceEx(void* self, const GUID& guid, const GUID& iid, void** device, IUnknown* outer) {
    const HRESULT result = g_originalInputCreateDeviceEx(self, guid, iid, device, outer);
    if (SUCCEEDED(result) && device) TrackMouseDevice(guid, *device);
    return result;
}

static HRESULT WINAPI HookedInputCreateEx(HINSTANCE instance, DWORD version, const GUID& iid, void** input, IUnknown* outer) {
    const HRESULT result = g_originalInputCreateEx(instance, version, iid, input, outer);
    if (SUCCEEDED(result) && input && *input) {
        void* old = WindowPatchSlot(*input, 3, reinterpret_cast<void*>(HookedInputCreateDevice));
        if (old && old != reinterpret_cast<void*>(HookedInputCreateDevice))
            g_originalInputCreateDevice = reinterpret_cast<InputCreateDeviceFn>(old);
        if (version >= 0x0700) {
            old = WindowPatchSlot(*input, 9, reinterpret_cast<void*>(HookedInputCreateDeviceEx));
            if (old && old != reinterpret_cast<void*>(HookedInputCreateDeviceEx))
                g_originalInputCreateDeviceEx = reinterpret_cast<InputCreateDeviceExFn>(old);
        }
    }
    return result;
}

static HRESULT PresentClientPrimary(const RECT* dirty, HDC paintDC = NULL) {
    if (!g_gameWindow || !g_clientPrimary) return DDERR_SURFACELOST;
    if (IsIconic(g_gameWindow)) return DD_OK; // retain the frame, without painting a minimized window
    RECT full = {0, 0, g_gameW, g_gameH}, area;
    if (!IntersectRect(&area, dirty ? dirty : &full, &full)) return DD_OK;
    HDC source = NULL;
    HRESULT result = g_originalSurfaceGetDC(g_clientPrimary, &source);
    if (FAILED(result)) return result;
    HDC window = paintDC ? paintDC : GetDC(g_gameWindow);
    const BOOL copied = window && BitBlt(window, area.left, area.top, area.right - area.left,
        area.bottom - area.top, source, area.left, area.top, SRCCOPY);
    GdiFlush(); // complete reads from the surface DC before releasing it
    if (window && !paintDC) ReleaseDC(g_gameWindow, window);
    result = g_originalSurfaceReleaseDC(g_clientPrimary, source);
    return FAILED(result) ? result : (copied ? DD_OK : E_FAIL);
}

static bool PaintClientPrimary(HWND window) {
    if (!g_frameActive || !g_clientPrimary) return false;
    PAINTSTRUCT paint = {};
    HDC dc = BeginPaint(window, &paint);
    if (!dc) return false;
    const HRESULT result = PresentClientPrimary(&paint.rcPaint, dc);
    EndPaint(window, &paint);
    if (FAILED(result))
        LogLine("ERROR", "WINDOW: could not repaint the client surface (0x%lX)", static_cast<unsigned long>(result));
    return true;
}

static ULONG WINAPI HookedSurfaceRelease(void* self) {
    const ULONG references = g_originalSurfaceRelease(self);
    if (!references && self == g_primary) {
        g_primary = NULL;
        IDirectDrawSurface7* client = g_clientPrimary;
        g_clientPrimary = NULL;
        if (client) client->Release();
    }
    return references;
}

static HRESULT WINAPI HookedSurfaceLock(void* self, RECT* rect, void* desc, DWORD flags, HANDLE event) {
    if (self != g_primary || !g_frameActive || !g_clientPrimary)
        return g_originalSurfaceLock(self, rect, desc, flags, event);
    const HRESULT result = g_originalSurfaceLock(g_clientPrimary, rect, desc, flags, event);
    if (SUCCEEDED(result)) {
        const RECT full = {0, 0, g_gameW, g_gameH};
        g_clientLockedRect = rect ? *rect : full;
        g_clientLockWrites = !(flags & DDLOCK_READONLY);
    }
    return result;
}

static HRESULT WINAPI HookedSurfaceUnlock(void* self, RECT* rect) {
    if (self != g_primary || !g_frameActive || !g_clientPrimary)
        return g_originalSurfaceUnlock(self, rect);
    const HRESULT result = g_originalSurfaceUnlock(g_clientPrimary, rect);
    return SUCCEEDED(result) && g_clientLockWrites ? PresentClientPrimary(&g_clientLockedRect) : result;
}

static HRESULT WINAPI HookedSurfaceGetDC(void* self, HDC* dc) {
    return g_originalSurfaceGetDC(self == g_primary && g_frameActive && g_clientPrimary ? g_clientPrimary : self, dc);
}

static HRESULT WINAPI HookedSurfaceReleaseDC(void* self, HDC dc) {
    if (self != g_primary || !g_frameActive || !g_clientPrimary)
        return g_originalSurfaceReleaseDC(self, dc);
    const HRESULT result = g_originalSurfaceReleaseDC(g_clientPrimary, dc);
    return SUCCEEDED(result) ? PresentClientPrimary(NULL) : result;
}

static HRESULT CreateClientPrimary(void* draw, void* primary) {
    DDSURFACEDESC2 original = {};
    original.dwSize = sizeof original;
    HRESULT result = static_cast<IDirectDrawSurface7*>(primary)->GetSurfaceDesc(&original);
    if (FAILED(result)) return result;
    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof desc;
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.dwWidth = g_gameW; desc.dwHeight = g_gameH;
    desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    desc.ddpfPixelFormat = original.ddpfPixelFormat;
    IDirectDrawSurface7* client = NULL;
    result = g_originalCreateSurface(draw, &desc, reinterpret_cast<void**>(&client), NULL);
    if (FAILED(result)) return result;
    IDirectDrawClipper* clipper = NULL;
    result = static_cast<IDirectDraw7*>(draw)->CreateClipper(0, &clipper, NULL);
    if (SUCCEEDED(result)) {
        struct { RGNDATAHEADER header; RECT rect; } region = {};
        region.rect = {0, 0, g_gameW, g_gameH};
        region.header.dwSize = sizeof(RGNDATAHEADER);
        region.header.iType = RDH_RECTANGLES;
        region.header.nCount = 1;
        region.header.nRgnSize = sizeof(RECT);
        region.header.rcBound = region.rect;
        result = clipper->SetClipList(reinterpret_cast<RGNDATA*>(&region), 0);
        if (SUCCEEDED(result)) result = client->SetClipper(clipper);
        clipper->Release();
    }
    if (FAILED(result)) { client->Release(); return result; }
    if (g_clientPrimary) g_clientPrimary->Release();
    g_clientPrimary = client;
    LogLine("INFO", "WINDOW: native DirectDraw uses a %dx%d client presentation surface (all monitors)", g_gameW, g_gameH);
    return DD_OK;
}

static void ScaleRectToScreen(const RECT& in, RECT& out) {
    out.left = g_winX + static_cast<LONG>(in.left * g_scale + 0.5); out.top = g_winY + static_cast<LONG>(in.top * g_scale + 0.5);
    out.right = g_winX + static_cast<LONG>(in.right * g_scale + 0.5); out.bottom = g_winY + static_cast<LONG>(in.bottom * g_scale + 0.5);
}

static HRESULT WINAPI HookedPrimaryBlt(void* self, RECT* dest, void* src, RECT* srcRect, DWORD flags, void* fx) {
    if (g_frameActive && g_clientPrimary && (self == g_primary || src == g_primary)) {
        const HRESULT result = g_originalBlt(self == g_primary ? g_clientPrimary : self, dest,
            src == g_primary ? g_clientPrimary : src, srcRect, flags, fx);
        return SUCCEEDED(result) && self == g_primary ? PresentClientPrimary(dest) : result;
    }
    if (self != g_primary || !WindowMapsCoordinates() || !g_scaleByBlt) return g_originalBlt(self, dest, src, srcRect, flags, fx);
    RECT full = {0, 0, g_gameW, g_gameH}, scaled;
    ScaleRectToScreen(dest ? *dest : full, scaled);
    return g_originalBlt(self, &scaled, src, srcRect, flags, fx);
}

static HRESULT WINAPI HookedPrimaryBltFast(void* self, DWORD x, DWORD y, void* src, RECT* srcRect, DWORD trans) {
    const bool clientCopy = g_frameActive && g_clientPrimary && (self == g_primary || src == g_primary);
    if (!src || (!clientCopy && (self != g_primary || !WindowMapsCoordinates() || !g_scaleByBlt)))
        return g_originalBltFast(self, x, y, src, srcRect, trans);
    RECT whole = {0, 0, 0, 0};
    if (!srcRect) { // the whole source: its size from the surface description (DDSURFACEDESC2: height at +8, width at +12)
        BYTE desc[124]; memset(desc, 0, sizeof desc); *reinterpret_cast<DWORD*>(desc) = sizeof desc;
        void* source = clientCopy && src == g_primary ? g_clientPrimary : src;
        void** vtable = *reinterpret_cast<void***>(source);
        const HRESULT result = reinterpret_cast<GetSurfaceDescFn>(vtable[22])(source, desc);
        if (FAILED(result)) return clientCopy ? result : g_originalBltFast(self, x, y, src, srcRect, trans);
        whole.right = static_cast<LONG>(*reinterpret_cast<DWORD*>(desc + 12)); whole.bottom = static_cast<LONG>(*reinterpret_cast<DWORD*>(desc + 8));
        srcRect = &whole;
    }
    RECT dest = {static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x) + (srcRect->right - srcRect->left), static_cast<LONG>(y) + (srcRect->bottom - srcRect->top)};
    DWORD flags = 0x01000000 /* DDBLT_WAIT */;
    if (trans & 1) flags |= 0x8000;  // DDBLTFAST_SRCCOLORKEY -> DDBLT_KEYSRC
    if (trans & 2) flags |= 0x2000;  // DDBLTFAST_DESTCOLORKEY -> DDBLT_KEYDEST
    return HookedPrimaryBlt(self, &dest, src, srcRect, flags, NULL);
}

static HRESULT WINAPI HookedCreateSurfaceScale(void* self, void* desc, void** surface, IUnknown* outer) {
    HRESULT result = g_originalCreateSurface(self, desc, surface, outer);
    if (SUCCEEDED(result) && surface && *surface && desc && g_scaleByBlt) {
        const DWORD flags = *reinterpret_cast<DWORD*>(reinterpret_cast<BYTE*>(desc) + 4), caps = *reinterpret_cast<DWORD*>(reinterpret_cast<BYTE*>(desc) + 0x68); // DDSURFACEDESC2: ddsCaps at 0x68
        if ((flags & 0x1 /* DDSD_CAPS */) && (caps & 0x200 /* DDSCAPS_PRIMARYSURFACE */)) {
            g_primary = *surface; // a primary surface the game creates again (a lost surface, a mode change) replaces the old one
            if (g_frameActive) {
                const HRESULT created = CreateClientPrimary(self, *surface);
                if (FAILED(created)) {
                    LogLine("ERROR", "WINDOW: could not create the client presentation surface (0x%lX)", static_cast<unsigned long>(created));
                    static_cast<IDirectDrawSurface7*>(*surface)->Release();
                    *surface = NULL; g_primary = NULL;
                    return created;
                }
                if (!g_originalSurfaceRelease) {
                    g_originalSurfaceRelease = reinterpret_cast<SurfaceReleaseFn>(WindowPatchSlot(g_primary, 2, reinterpret_cast<void*>(HookedSurfaceRelease)));
                    g_originalSurfaceLock = reinterpret_cast<SurfaceLockFn>(WindowPatchSlot(g_primary, 25, reinterpret_cast<void*>(HookedSurfaceLock)));
                    g_originalSurfaceUnlock = reinterpret_cast<SurfaceUnlockFn>(WindowPatchSlot(g_primary, 32, reinterpret_cast<void*>(HookedSurfaceUnlock)));
                    g_originalSurfaceGetDC = reinterpret_cast<SurfaceGetDcFn>(WindowPatchSlot(g_primary, 17, reinterpret_cast<void*>(HookedSurfaceGetDC)));
                    g_originalSurfaceReleaseDC = reinterpret_cast<SurfaceReleaseDcFn>(WindowPatchSlot(g_primary, 26, reinterpret_cast<void*>(HookedSurfaceReleaseDC)));
                }
            }
            if (!g_originalBlt) { // the vtable (shared by the class's surfaces) is patched once
                g_originalBlt = reinterpret_cast<BltFn>(WindowPatchSlot(g_primary, 5, reinterpret_cast<void*>(HookedPrimaryBlt)));
                g_originalBltFast = reinterpret_cast<BltFastFn>(WindowPatchSlot(g_primary, 7, reinterpret_cast<void*>(HookedPrimaryBltFast)));
                if (!g_frameActive) LogLine("INFO", "SCALE: the primary surface's Blt / BltFast stretch the frame to the scaled window");
            }
        }
    }
    return result;
}

// Exclusive full screen has no window to scale: leave the game at its own size.
static HRESULT WINAPI HookedSetCoopScale(void* self, HWND window, DWORD flags) {
    if ((flags & 0x10 /* DDSCL_EXCLUSIVE */) && WindowMapsCoordinates()) {
        g_scaleActive = false;
        if (g_frameActive && g_gameWindow) {
            const LONG style = GetWindowLongA(g_gameWindow, GWL_STYLE);
            SetWindowLongA(g_gameWindow, GWL_STYLE, (style & ~(WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX)) | WS_POPUP);
        }
        g_frameActive = false;
        LogLine("WARN", "WINDOW: exclusive fullscreen requested: window framing and coordinate mapping disabled");
        if (g_gameWindow) SetWindowPos(g_gameWindow, NULL, 0, 0, g_gameW, g_gameH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        DestroyBars();
    }
    return g_originalSetCoop(self, window, flags);
}

// Called with every new DirectDraw object the game creates (by um.dll's hook, or by the asi's own below).
void WindowScaleDirectDrawCreated(void* directDraw, bool stretchesByItself) {
    if (!WindowMapsCoordinates() || !directDraw || g_originalCreateSurface) return;
    g_scaleByBlt = !stretchesByItself;
    g_originalSetCoop = reinterpret_cast<SetCoopFn>(WindowPatchSlot(directDraw, 20, reinterpret_cast<void*>(HookedSetCoopScale)));
    if (g_scaleByBlt) g_originalCreateSurface = reinterpret_cast<CreateSurfaceFn>(WindowPatchSlot(directDraw, 6, reinterpret_cast<void*>(HookedCreateSurfaceScale)));
    else g_originalCreateSurface = reinterpret_cast<CreateSurfaceFn>(-1); // "installed": nothing else to hook
    LogLine("INFO", g_frameActive ? "WINDOW: %s" : "SCALE: %s", g_scaleByBlt ? (g_frameActive ? "native DirectDraw will present through the client surface" : "this DirectDraw does not stretch by itself: the present is stretched by the plugin") : "D7VK stretches the frame to the window by itself");
}

#ifdef UM_WINDOW_STANDALONE
// Without um.dll the plugin hooks the game's DirectDrawCreateEx itself, and finds out whether the DirectDraw is D7VK by its strings.
typedef HRESULT (WINAPI *DirectDrawCreateExFn)(const GUID*, void**, const GUID&, IUnknown*);
static DirectDrawCreateExFn g_originalDirectDrawCreateEx = NULL;

static bool ModuleHasText(HMODULE module, const char* text) {
    const BYTE* p = reinterpret_cast<const BYTE*>(module);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(p);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(p + dos->e_lfanew);
    const size_t n = nt->OptionalHeader.SizeOfImage, k = strlen(text);
    for (size_t i = 0; i + k < n; ++i) {
        if ((i & 0xFFF) == 0 && IsBadReadPtr(p + i, 4096 + k)) { i |= 0xFFF; continue; } // an unreadable page (a discardable section): skip it
        if (p[i] == static_cast<BYTE>(text[0]) && memcmp(p + i, text, k) == 0) return true;
    }
    return false;
}

static HRESULT WINAPI HookedDirectDrawCreateExScale(const GUID* guid, void** directDraw, const GUID& iid, IUnknown* outer) {
    HRESULT result = g_originalDirectDrawCreateEx(guid, directDraw, iid, outer);
    if (SUCCEEDED(result) && directDraw && *directDraw && WindowMapsCoordinates()) {
        HMODULE ddraw = GetModuleHandleA("ddraw.dll");
        WindowScaleDirectDrawCreated(*directDraw, ddraw && (ModuleHasText(ddraw, "D7VK") || ModuleHasText(ddraw, "DXVK")));
    }
    return result;
}
#endif

// A DPI-unaware process gets scaled-down monitor sizes and a blurry bitmap-stretched window on a scaled display: the scaling needs the real pixels.
static void EnableDpiAwareness() {
    HMODULE user32 = GetModuleHandleA("user32.dll");
    FARPROC aware = user32 ? GetProcAddress(user32, "SetProcessDPIAware") : NULL; // Vista and newer
    if (aware) reinterpret_cast<BOOL (WINAPI*)()>(aware)();
}

void InstallWindowHooks() {
    if (!g_enableBorderless) return;
    if (!g_cursorLockReady) { InitializeCriticalSection(&g_cursorLock); g_cursorLockReady = true; }
    if (g_enableScale || g_enableFrame) EnableDpiAwareness();
    HMODULE process = GetModuleHandleA(NULL);
    if (!process || !PatchImportedFunction(process, "CreateWindowExA", reinterpret_cast<ULONG_PTR>(HookedCreateWindowExA), reinterpret_cast<ULONG_PTR*>(&g_originalCreateWindowExA)))
        LogLine("WARN", "BORDERLESS: could not hook CreateWindowExA");
    if (process) {
        PatchImportedFunction(process, "GetCursorPos", reinterpret_cast<ULONG_PTR>(HookedGetCursorPos), reinterpret_cast<ULONG_PTR*>(&g_originalGetCursorPos));
        PatchImportedFunction(process, "GetMessageA", reinterpret_cast<ULONG_PTR>(HookedGetMessageA), reinterpret_cast<ULONG_PTR*>(&g_originalGetMessageA));
        PatchImportedFunction(process, "PeekMessageA", reinterpret_cast<ULONG_PTR>(HookedPeekMessageA), reinterpret_cast<ULONG_PTR*>(&g_originalPeekMessageA));
        PatchImportedFunction(process, "SetCursorPos", reinterpret_cast<ULONG_PTR>(HookedSetCursorPos), reinterpret_cast<ULONG_PTR*>(&g_originalSetCursorPos));
        PatchImportedFunction(process, "DirectInputCreateEx", reinterpret_cast<ULONG_PTR>(HookedInputCreateEx), reinterpret_cast<ULONG_PTR*>(&g_originalInputCreateEx));
#ifdef UM_WINDOW_STANDALONE
        PatchImportedFunction(process, "DirectDrawCreateEx", reinterpret_cast<ULONG_PTR>(HookedDirectDrawCreateExScale), reinterpret_cast<ULONG_PTR*>(&g_originalDirectDrawCreateEx));
#endif
    }
}
