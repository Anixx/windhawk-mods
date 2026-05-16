// ==WindhawkMod==
// @id              taskbar-icon-size
// @name            Taskbar height and icon size
// @description     Control the taskbar height and icon size, improve icon quality (Windows 11 only)
// @version         1.3.7
// @author          m417z
// @github          https://github.com/m417z
// @twitter         https://twitter.com/m417z
// @homepage        https://m417z.com/
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -DWINVER=0x0A00 -lole32 -loleaut32 -lruntimeobject -lshcore -lversion
// ==/WindhawkMod==

// Source code is published under The GNU General Public License v3.0.
//
// For bug reports and feature requests, please open an issue here:
// https://github.com/ramensoftware/windhawk-mods/issues
//
// For pull requests, development takes place here:
// https://github.com/m417z/my-windhawk-mods

// ==WindhawkModReadme==
/*
# Taskbar height and icon size

Control the taskbar height and icon size. Make the taskbar icons large and
crisp, or small and compact.

By default, the Windows 11 taskbar shows taskbar icons with the 24x24 size.
Since icons in Windows are either 16x16 or 32x32, the 24x24 icons are downscaled
versions of the 32x32 variants, which makes them blurry. This mod allows to
change the size of icons, and so the original quality icons can be used, as well
as any other icon size.

![Before screenshot](https://i.imgur.com/TLza5fp.png) \
*Taskbar height: 48, icon size: 24x24 (Windows 11 default)*

![After screenshot, large icons](https://i.imgur.com/3b8h40F.png) \
*Taskbar height: 52, icon size: 32x32*

![After screenshot, small icons](https://i.imgur.com/Xy04Zcu.png) \
*Taskbar height: 34, icon size: 16x16*

![After screenshot, small and narrow icons](https://i.imgur.com/fsx8C56.png) \
*Taskbar height: 34, icon size: 16x16, taskbar button width: 28*

Only Windows 11 is supported. For older Windows versions check out [7+ Taskbar
Tweaker](https://tweaker.ramensoftware.com/).

Also check out the **Taskbar tray icon spacing and grid** mod.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- TaskbarHeight: 52
  $name: Taskbar height
  $description: >-
    The height, in pixels, of the taskbar (Windows 11 default: 48)
- IconSize: 32
  $name: Icon size
  $description: >-
    The size, in pixels, of icons on the taskbar (Windows 11 default: 24)
- TaskbarButtonWidth: 44
  $name: Taskbar button width
  $description: >-
    The width, in pixels, of the taskbar buttons (Windows 11 default: 44)
- IconSizeSmall: 16
  $name: Small icon size
  $description: >-
    The size, in pixels, of small icons on the taskbar (Windows 11 default: 16)

    Used in newer Windows 11 builds with support for small taskbar icons (around
    July 2025)
- TaskbarButtonWidthSmall: 32
  $name: Small taskbar button width
  $description: >-
    The width, in pixels, of the small taskbar buttons (Windows 11 default: 32)

    Used in newer Windows 11 builds with support for small taskbar icons (around
    July 2025)
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>
#include <windows.h>

static bool IsExplorerWindow(HWND hwnd)
{
    if (!hwnd) return false;
    wchar_t cls[256] = {};
    if (!GetClassNameW(hwnd, cls, ARRAYSIZE(cls))) return false;
    return wcscmp(cls, L"CabinetWClass") == 0 || wcscmp(cls, L"ExploreWClass") == 0;
}

static LRESULT CALLBACK SubclassWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, DWORD_PTR dwRefData)
{
    if (msg == WM_GETMINMAXINFO)
    {
        LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = 1;
        mmi->ptMinTrackSize.y = 1;
        return result;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static void SubclassWindow(HWND hwnd)
{
    if (!IsExplorerWindow(hwnd)) return;
    if (WindhawkUtils::SetWindowSubclassFromAnyThread(hwnd, SubclassWndProc, 0))
        Wh_Log(L"Subclassed HWND %p", hwnd);
}

static void UnsubclassWindow(HWND hwnd)
{
    WindhawkUtils::RemoveWindowSubclassFromAnyThread(hwnd, SubclassWndProc);
}

static BOOL CALLBACK EnumSubclass(HWND hwnd, LPARAM)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) SubclassWindow(hwnd);
    return TRUE;
}

static BOOL CALLBACK EnumUnsubclass(HWND hwnd, LPARAM)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) UnsubclassWindow(hwnd);
    return TRUE;
}

using CreateWindowExW_t = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
CreateWindowExW_t originalCreateWindowExW = nullptr;

HWND WINAPI CreateWindowExWHook(DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName, DWORD dwStyle, int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam)
{
    HWND hwnd = originalCreateWindowExW(dwExStyle, lpClassName, lpWindowName, dwStyle, X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam);
    if (hwnd) SubclassWindow(hwnd);
    return hwnd;
}

BOOL Wh_ModInit()
{
    Wh_Log(L"init");
    Wh_SetFunctionHook(reinterpret_cast<void*>(CreateWindowExW), reinterpret_cast<void*>(CreateWindowExWHook), reinterpret_cast<void**>(&originalCreateWindowExW));
    EnumWindows(EnumSubclass, 0);
    return TRUE;
}

void Wh_ModUninit()
{
    Wh_Log(L"uninit");
    EnumWindows(EnumUnsubclass, 0);
}

void Wh_ModSettingsChanged()
{
}
