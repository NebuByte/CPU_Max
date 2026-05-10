/*
 * CPUMax - System tray utility to toggle Windows Maximum Processor State
 *
 * Right-click menu:
 *   - Enable Max Performance (checked when active)
 *   - Start with Windows     (checked when active)
 *   - Exit                   (resets to 99% before quitting)
 *
 * Hover tooltip: status (Enabled/Disabled), CPU usage %, RAM usage %.
 * Tray icon is drawn at runtime: green dot = enabled, red dot = disabled.
 */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <powrprof.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")

#define WM_TRAYICON         (WM_USER + 1)
#define ID_TRAY_TOGGLE      1001
#define ID_TRAY_STARTUP     1003
#define ID_TRAY_EXIT        1005
#define TIMER_UPDATE        1
#define UPDATE_INTERVAL_MS  2000
#define APP_NAME            "CPUMax"
#define WND_CLASS           "CPUMaxTrayClass"
#define RUN_KEY             "Software\\Microsoft\\Windows\\CurrentVersion\\Run"

/* GUID_PROCESSOR_THROTTLE_MAXIMUM - {BC5038F7-23E0-4960-96DA-33ABAF5935EC} */
static const GUID GUID_PROC_MAX =
    { 0xBC5038F7, 0x23E0, 0x4960, { 0x96, 0xDA, 0x33, 0xAB, 0xAF, 0x59, 0x35, 0xEC } };
/* GUID_PROCESSOR_SETTINGS_SUBGROUP - {54533251-82BE-4824-96C1-47B60B740D00} */
static const GUID GUID_PROC_SUBGROUP =
    { 0x54533251, 0x82BE, 0x4824, { 0x96, 0xC1, 0x47, 0xB6, 0x0B, 0x74, 0x0D, 0x00 } };

static NOTIFYICONDATA g_nid;
static HWND  g_hwnd       = NULL;
static HICON g_iconOn     = NULL;
static HICON g_iconOff    = NULL;
static BOOL  g_enabled    = FALSE;

/* CPU usage state */
static ULARGE_INTEGER g_lastIdle  = {0};
static ULARGE_INTEGER g_lastKern  = {0};
static ULARGE_INTEGER g_lastUser  = {0};
static double         g_cpuPct    = 0.0;
static double         g_ramPct    = 0.0;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static ULONGLONG ftToU64(const FILETIME *ft) {
    ULARGE_INTEGER u;
    u.LowPart  = ft->dwLowDateTime;
    u.HighPart = ft->dwHighDateTime;
    return u.QuadPart;
}

static void sampleCpu(void) {
    FILETIME idle, kern, user;
    if (!GetSystemTimes(&idle, &kern, &user)) return;

    ULONGLONG i = ftToU64(&idle);
    ULONGLONG k = ftToU64(&kern);
    ULONGLONG u = ftToU64(&user);

    if (g_lastKern.QuadPart != 0) {
        ULONGLONG sys  = (k - g_lastKern.QuadPart) + (u - g_lastUser.QuadPart);
        ULONGLONG idl  = i - g_lastIdle.QuadPart;
        if (sys > 0) {
            g_cpuPct = 100.0 * (double)(sys - idl) / (double)sys;
            if (g_cpuPct < 0)   g_cpuPct = 0;
            if (g_cpuPct > 100) g_cpuPct = 100;
        }
    }
    g_lastIdle.QuadPart = i;
    g_lastKern.QuadPart = k;
    g_lastUser.QuadPart = u;
}

static void sampleRam(void) {
    MEMORYSTATUSEX m = { sizeof(m) };
    if (GlobalMemoryStatusEx(&m)) {
        g_ramPct = (double)m.dwMemoryLoad;
    }
}

/* ------------------------------------------------------------------ */
/* Power management                                                    */
/* ------------------------------------------------------------------ */

static BOOL readMaxState(DWORD *outAc, DWORD *outDc) {
    GUID *active = NULL;
    if (PowerGetActiveScheme(NULL, &active) != ERROR_SUCCESS) return FALSE;

    DWORD ac = 0, dc = 0;
    BOOL ok = TRUE;
    if (PowerReadACValueIndex(NULL, active, &GUID_PROC_SUBGROUP, &GUID_PROC_MAX, &ac) != ERROR_SUCCESS) ok = FALSE;
    if (PowerReadDCValueIndex(NULL, active, &GUID_PROC_SUBGROUP, &GUID_PROC_MAX, &dc) != ERROR_SUCCESS) ok = FALSE;

    if (ok) { *outAc = ac; *outDc = dc; }
    if (active) LocalFree(active);
    return ok;
}

static BOOL writeMaxState(DWORD value) {
    GUID *active = NULL;
    if (PowerGetActiveScheme(NULL, &active) != ERROR_SUCCESS) return FALSE;

    DWORD r1 = PowerWriteACValueIndex(NULL, active, &GUID_PROC_SUBGROUP, &GUID_PROC_MAX, value);
    DWORD r2 = PowerWriteDCValueIndex(NULL, active, &GUID_PROC_SUBGROUP, &GUID_PROC_MAX, value);
    /* Re-apply scheme so changes take effect immediately */
    PowerSetActiveScheme(NULL, active);
    if (active) LocalFree(active);
    return (r1 == ERROR_SUCCESS && r2 == ERROR_SUCCESS);
}

static void refreshEnabledState(void) {
    DWORD ac = 0, dc = 0;
    if (readMaxState(&ac, &dc)) {
        /* "Enabled" means max performance allowed (100%) */
        g_enabled = (ac >= 100);
    }
}

/* ------------------------------------------------------------------ */
/* Icons (drawn at runtime)                                            */
/* ------------------------------------------------------------------ */

/* Draws a CPU-chip icon: square body with pins on all 4 sides + inner die.
   `accent` tints the die (green = enabled, red = disabled). */
static HICON makeCpuIcon(COLORREF accent) {
    const int W = 32, H = 32;
    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem    = CreateCompatibleDC(hdcScreen);
    HDC hdcMask   = CreateCompatibleDC(hdcScreen);

    BITMAPV5HEADER bi = {0};
    bi.bV5Size        = sizeof(bi);
    bi.bV5Width       = W;
    bi.bV5Height      = H;
    bi.bV5Planes      = 1;
    bi.bV5BitCount    = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask     = 0x00FF0000;
    bi.bV5GreenMask   = 0x0000FF00;
    bi.bV5BlueMask    = 0x000000FF;
    bi.bV5AlphaMask   = 0xFF000000;

    void *bits = NULL;
    HBITMAP hColor = CreateDIBSection(hdcScreen, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HBITMAP hMask  = CreateBitmap(W, H, 1, 1, NULL);

    HBITMAP oldColor = (HBITMAP)SelectObject(hdcMem,  hColor);
    HBITMAP oldMask  = (HBITMAP)SelectObject(hdcMask, hMask);

    if (bits) memset(bits, 0, W * H * 4);

    /* Palette */
    COLORREF body    = RGB( 50,  55,  65);   /* dark chip body  */
    COLORREF outline = RGB( 18,  20,  25);
    COLORREF pinCol  = RGB(190, 190, 190);   /* metal pins      */
    COLORREF dieEdge = RGB( 28,  30,  35);

    HBRUSH bBody = CreateSolidBrush(body);
    HBRUSH bDie  = CreateSolidBrush(accent);
    HBRUSH bPin  = CreateSolidBrush(pinCol);
    HPEN   pOut  = CreatePen(PS_SOLID, 1, outline);
    HPEN   pDie  = CreatePen(PS_SOLID, 1, dieEdge);
    HPEN   pNull = CreatePen(PS_NULL,  0, 0);

    /* --- Pins (drawn first so the body covers their inner ends) --- */
    SelectObject(hdcMem, bPin);
    SelectObject(hdcMem, pNull);
    /* top + bottom */
    int pinXs[3] = { 9, 15, 21 };
    for (int i = 0; i < 3; ++i) {
        Rectangle(hdcMem, pinXs[i],     2, pinXs[i] + 3,  9);   /* top    */
        Rectangle(hdcMem, pinXs[i],    23, pinXs[i] + 3, 30);   /* bottom */
        Rectangle(hdcMem,  2, pinXs[i],   9, pinXs[i] + 3);     /* left   */
        Rectangle(hdcMem, 23, pinXs[i],  30, pinXs[i] + 3);     /* right  */
    }

    /* --- Chip body (rounded rect) --- */
    SelectObject(hdcMem, bBody);
    SelectObject(hdcMem, pOut);
    RoundRect(hdcMem, 6, 6, 26, 26, 4, 4);

    /* --- Inner die (the colored heart of the chip) --- */
    SelectObject(hdcMem, bDie);
    SelectObject(hdcMem, pDie);
    RoundRect(hdcMem, 11, 11, 21, 21, 2, 2);

    /* --- Notch / pin-1 marker (small dot in top-left of body) --- */
    HBRUSH bDot = CreateSolidBrush(RGB(220, 220, 220));
    SelectObject(hdcMem, bDot);
    SelectObject(hdcMem, pNull);
    Ellipse(hdcMem, 8, 8, 11, 11);

    /* Force alpha=255 on every drawn pixel */
    if (bits) {
        unsigned char *p = (unsigned char*)bits;
        for (int i = 0; i < W * H; ++i) {
            int idx = i * 4;
            if (p[idx] || p[idx+1] || p[idx+2]) p[idx + 3] = 255;
        }
    }

    /* Mask: black = opaque, white = transparent. Cover the union of
       chip body and pins. */
    PatBlt(hdcMask, 0, 0, W, H, WHITENESS);
    HBRUSH mb = CreateSolidBrush(RGB(0,0,0));
    HBRUSH om = (HBRUSH)SelectObject(hdcMask, mb);
    HPEN   mp = (HPEN)  SelectObject(hdcMask, pNull);
    RoundRect(hdcMask, 6, 6, 26, 26, 4, 4);
    for (int i = 0; i < 3; ++i) {
        Rectangle(hdcMask, pinXs[i],  2, pinXs[i] + 3,  9);
        Rectangle(hdcMask, pinXs[i], 23, pinXs[i] + 3, 30);
        Rectangle(hdcMask,  2, pinXs[i],   9, pinXs[i] + 3);
        Rectangle(hdcMask, 23, pinXs[i],  30, pinXs[i] + 3);
    }
    SelectObject(hdcMask, om);
    SelectObject(hdcMask, mp);
    DeleteObject(mb);

    /* Cleanup GDI */
    SelectObject(hdcMem,  oldColor);
    SelectObject(hdcMask, oldMask);
    DeleteObject(bBody);
    DeleteObject(bDie);
    DeleteObject(bPin);
    DeleteObject(bDot);
    DeleteObject(pOut);
    DeleteObject(pDie);
    DeleteObject(pNull);
    DeleteDC(hdcMem);
    DeleteDC(hdcMask);
    ReleaseDC(NULL, hdcScreen);

    ICONINFO ii = {0};
    ii.fIcon    = TRUE;
    ii.hbmColor = hColor;
    ii.hbmMask  = hMask;
    HICON icon  = CreateIconIndirect(&ii);

    DeleteObject(hColor);
    DeleteObject(hMask);
    return icon;
}

/* ------------------------------------------------------------------ */
/* Startup registry                                                    */
/* ------------------------------------------------------------------ */

static BOOL getExePath(char *out, DWORD cap) {
    return GetModuleFileNameA(NULL, out, cap) > 0;
}

static BOOL isStartupEnabled(void) {
    HKEY hk;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return FALSE;
    char buf[MAX_PATH] = {0};
    DWORD type = 0, sz = sizeof(buf);
    LONG r = RegQueryValueExA(hk, APP_NAME, NULL, &type, (LPBYTE)buf, &sz);
    RegCloseKey(hk);
    return (r == ERROR_SUCCESS && type == REG_SZ);
}

static void setStartupEnabled(BOOL enable) {
    HKEY hk;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &hk) != ERROR_SUCCESS)
        return;
    if (enable) {
        char path[MAX_PATH];
        if (getExePath(path, sizeof(path))) {
            char quoted[MAX_PATH + 4];
            snprintf(quoted, sizeof(quoted), "\"%s\"", path);
            RegSetValueExA(hk, APP_NAME, 0, REG_SZ, (const BYTE*)quoted, (DWORD)strlen(quoted) + 1);
        }
    } else {
        RegDeleteValueA(hk, APP_NAME);
    }
    RegCloseKey(hk);
}

/* ------------------------------------------------------------------ */
/* Tray                                                                */
/* ------------------------------------------------------------------ */

static void updateTrayIconAndTip(void) {
    g_nid.hIcon = g_enabled ? g_iconOn : g_iconOff;
    snprintf(g_nid.szTip, sizeof(g_nid.szTip),
             "%s: %s\nCPU: %.0f%%   RAM: %.0f%%",
             APP_NAME,
             g_enabled ? "Max Performance ENABLED" : "Max Performance disabled",
             g_cpuPct, g_ramPct);
    Shell_NotifyIcon(NIM_MODIFY, &g_nid);
}

static void addTray(HWND hwnd) {
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = g_enabled ? g_iconOn : g_iconOff;
    strcpy(g_nid.szTip, APP_NAME);
    Shell_NotifyIcon(NIM_ADD, &g_nid);
}

static void removeTray(void) {
    Shell_NotifyIcon(NIM_DELETE, &g_nid);
}

static void showMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    AppendMenuA(menu, MF_STRING | (g_enabled ? MF_CHECKED : 0), ID_TRAY_TOGGLE,  "Enable Max Performance");
    AppendMenuA(menu, MF_STRING | (isStartupEnabled() ? MF_CHECKED : 0), ID_TRAY_STARTUP, "Start with Windows");
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING, ID_TRAY_EXIT, "Exit");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);
}

/* ------------------------------------------------------------------ */
/* Window proc                                                         */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            sampleCpu();    /* prime */
            sampleRam();
            refreshEnabledState();
            addTray(hwnd);
            SetTimer(hwnd, TIMER_UPDATE, UPDATE_INTERVAL_MS, NULL);
            return 0;

        case WM_TIMER:
            if (wp == TIMER_UPDATE) {
                sampleCpu();
                sampleRam();
                refreshEnabledState();
                updateTrayIconAndTip();
            }
            return 0;

        case WM_TRAYICON:
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
                showMenu(hwnd);
            } else if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
                /* double-click toggles */
                if (writeMaxState(g_enabled ? 99 : 100)) {
                    refreshEnabledState();
                    updateTrayIconAndTip();
                }
            }
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_TRAY_TOGGLE:
                    if (writeMaxState(g_enabled ? 99 : 100)) {
                        refreshEnabledState();
                        updateTrayIconAndTip();
                    }
                    break;
                case ID_TRAY_STARTUP:
                    setStartupEnabled(!isStartupEnabled());
                    break;
                case ID_TRAY_EXIT:
                    DestroyWindow(hwnd);
                    break;
            }
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, TIMER_UPDATE);
            /* On exit always reset to disabled (99%) */
            writeMaxState(99);
            removeTray();
            if (g_iconOn)  DestroyIcon(g_iconOn);
            if (g_iconOff) DestroyIcon(g_iconOff);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* Entry                                                               */
/* ------------------------------------------------------------------ */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    (void)hPrev; (void)cmd; (void)show;

    /* single instance */
    HANDLE mtx = CreateMutexA(NULL, TRUE, "Global\\CPUMax_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mtx) CloseHandle(mtx);
        return 0;
    }

    g_iconOn  = makeCpuIcon(RGB( 80, 220, 120));   /* enabled  - vivid green */
    g_iconOff = makeCpuIcon(RGB(220,  70,  70));   /* disabled - vivid red   */

    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = WND_CLASS;
    RegisterClassA(&wc);

    g_hwnd = CreateWindowExA(0, WND_CLASS, APP_NAME,
                             0, 0, 0, 0, 0,
                             HWND_MESSAGE, NULL, hInst, NULL);
    if (!g_hwnd) return 1;

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    return (int)msg.wParam;
}
