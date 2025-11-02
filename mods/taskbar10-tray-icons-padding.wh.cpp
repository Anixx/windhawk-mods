// ==WindhawkMod==
// @id              taskbar10-tray-icons-padding
// @name            Win10 taskbar tray icons padding
// @description     Adjust tray icon padding on Win10 taskbar (under Win10 or Win11)
// @version         1.0.0
// @author          anixx
// @github          https://github.com/Anixx
// @include         explorer.exe
// @compilerOptions -lcomctl32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*

Allows you to adjust the padding around tray icons in the Win10 taskbar, running either under Win10 or Win11.

*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- padding: 2
  $name: Padding
  $description: Tray icons padding
*/
// ==/WindhawkModSettings==

#include <CommCtrl.h>
#include <unordered_set>

static int padding;
DWORD dwTaskbarThreadId = 0;

std::unordered_set<ULONGLONG> trayPatterns;
bool learningPhase = true;

using GetSystemMetrics_t = decltype(&GetSystemMetrics);
GetSystemMetrics_t GetSystemMetrics_Orig;

int WINAPI GetSystemMetrics_Hook(int nIndex)
{
    int nRet = GetSystemMetrics_Orig(nIndex);
    
    if ((GetCurrentThreadId() == dwTaskbarThreadId) && (nIndex == SM_CXSMICON))
    {
        void *retaddr = __builtin_return_address(0);
        ULONGLONG pattern = ((ULONGLONG)retaddr) & 0xFFFFF;
        
        if (learningPhase)
        {
            // Record all patterns during initialization
            if (trayPatterns.find(pattern) == trayPatterns.end())
            {
                trayPatterns.insert(pattern);
                Wh_Log(L"Detected pattern: 0x%05llx", pattern);
            }
            // Don't modify during learning
            return nRet;
        }
        
        // After learning, only modify tray patterns
        if (trayPatterns.find(pattern) != trayPatterns.end())
        {
            return (nRet + padding) / 2;
        }
    }

    return nRet;
}

static LRESULT CALLBACK TrayToolbarSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, 
                                                 LPARAM lParam, UINT_PTR uIdSubclass, 
                                                 DWORD_PTR dwRefData)
{
    if (uMsg == TB_SETPADDING)
    {
        if (learningPhase)
        {
            learningPhase = false;
            Wh_Log(L"Learning complete: %zu pattern(s) detected", trayPatterns.size());
        }
        
        lParam &= ~0xFFFF;
        lParam |= ((padding / 2 * 2) & 0xFFFF);
        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
    else if (uMsg == WM_NCDESTROY)
    {
        RemoveWindowSubclass(hWnd, TrayToolbarSubclassProc, uIdSubclass);
    }
    
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

HWND FindTrayToolbar(HWND hTrayNotifyWnd)
{
    return FindWindowEx(hTrayNotifyWnd, NULL, L"ToolbarWindow32", NULL);
}

using CreateWindowExW_t = decltype(&CreateWindowExW);
CreateWindowExW_t CreateWindowExW_Orig;

HWND WINAPI CreateWindowExW_Hook(DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName,
    DWORD dwStyle, int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, 
    HINSTANCE hInstance, LPVOID lpParam)
{
    HWND hWnd = CreateWindowExW_Orig(dwExStyle, lpClassName, lpWindowName, dwStyle, 
                                      X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam);
    
    if ((((ULONG_PTR)lpClassName & ~(ULONG_PTR)0xffff) != 0) && (!wcscmp(lpClassName, L"TrayNotifyWnd")))
    {
        dwTaskbarThreadId = GetCurrentThreadId();
        
        HWND hToolbar = FindTrayToolbar(hWnd);
        if (hToolbar)
        {
            SetWindowSubclass(hToolbar, TrayToolbarSubclassProc, 0, 0);
        }
    }
    else if ((((ULONG_PTR)lpClassName & ~(ULONG_PTR)0xffff) != 0) && 
             (!wcscmp(lpClassName, L"ToolbarWindow32")) && 
             dwTaskbarThreadId != 0 &&
             dwTaskbarThreadId == GetCurrentThreadId())
    {
        WCHAR className[256];
        if (hWndParent && GetClassNameW(hWndParent, className, 256) &&
            wcscmp(className, L"TrayNotifyWnd") == 0)
        {
            SetWindowSubclass(hWnd, TrayToolbarSubclassProc, 0, 0);
        }
    }
    
    return hWnd;
}

BOOL Wh_ModInit(void)
{
    Wh_Log(L"Tray Icons Padding mod initializing");
    
    padding = Wh_GetIntSetting(L"padding");
    dwTaskbarThreadId = GetCurrentThreadId();

    Wh_SetFunctionHook((void*)CreateWindowExW, (void*)CreateWindowExW_Hook, (void**)&CreateWindowExW_Orig);
    Wh_SetFunctionHook((void*)GetSystemMetrics, (void*)GetSystemMetrics_Hook, (void**)&GetSystemMetrics_Orig);

    return TRUE;
}

void Wh_ModSettingsChanged()
{
    padding = Wh_GetIntSetting(L"padding");
    Wh_Log(L"Padding changed to: %d", padding);
}
