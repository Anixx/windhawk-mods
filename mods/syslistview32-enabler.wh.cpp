// ==WindhawkMod==
// @id              syslistview32-enabler
// @name            Enable SyslistView32
// @description     Enables SysListView32 folder layout in Explorer
// @version         1.0.2
// @author          anixx
// @github          https://github.com/Anixx
// @include         explorer.exe
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
Enables the SysListView32 control in explorer.exe process windows.  
This makes the view more compact and allows icon rearrangement.  
SysListView32 control has been used by default before Windows 7.  

> ⚠️ **Warning**  
> The mod alter by default every explorer.exe windows, including their child.  
> However, a setting is available to limit the mod specifically to File Explorer windows.  
> If you're using a custom start menu and have keyboard navigation issues, check this setting.

Before:

![Default view](https://i.imgur.com/rPpiFEU.png)

After:

![SysListView32 mode](https://i.imgur.com/oqYf1YW.png)

*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- file_explorer_detection: false
  $name: File Explorer windows restriction
  $description: When enabled, this option restricts the mod's effect to File Explorer windows
*/
// ==/WindhawkModSettings==


#include <minwindef.h>
#include <windhawk_api.h>
#include <windows.h>
#include <windhawk_utils.h>
#include <winnt.h>
#include <cstddef>
#include <unordered_map>
#include <list>
#include <future>

#ifdef _WIN64
#define CALCON __cdecl
#define SCALCON L"__cdecl"
#else
#define CALCON __thiscall
#define SCALCON L"__thiscall"
#endif

/* Utils, constants declarations */

bool IsWindowsExplorerWindow(void);
template <class T = bool> class HWNDCache;
const size_t kMaxCacheEntries = 128;
struct {
    bool fileExplorerDetection;
} g_settings;

/* SysListView32 */

using CDefView__UseItemsView_t = BOOL (CALCON *)(void *);
CDefView__UseItemsView_t CDefView__UseItemsView_orig = nullptr;
BOOL CALCON CDefView__UseItemsView_hook(void* /*pThis*/) {
    return g_settings.fileExplorerDetection && !IsWindowsExplorerWindow();
}

/* Hooking */

void LoadSettings()
{
    g_settings.fileExplorerDetection = Wh_GetIntSetting(L"file_explorer_detection");
}

BOOL Wh_ModInit(void)
{   
    LoadSettings();
    HMODULE hShell32 = LoadLibraryW(L"shell32.dll");
    if (!hShell32)
    {
        Wh_Log(L"Failed to load shell32.dll");
        return FALSE;
    }

    WindhawkUtils::SYMBOL_HOOK hooks[] = {
        {
            {
                L"private: int "
                SCALCON
                L" CDefView::_UseItemsView(void)"
            },
            &CDefView__UseItemsView_orig,
            CDefView__UseItemsView_hook,
            false
        }
    };

    if (!HookSymbols(hShell32, hooks, ARRAYSIZE(hooks)))
    {
        Wh_Log(L"Failed to hook one or more symbol functions");
        return FALSE;
    }
    return TRUE;
}

/* Utils */
template <class T> class HWNDCache
{
public:
    HWNDCache(size_t maxSize) : m_maxSize(maxSize) {}

    T TryGet(HWND hwnd, T& result)
    {
        auto it = m_cacheMap.find(hwnd);
        if (it == m_cacheMap.end()) return false;

        m_cacheList.splice(m_cacheList.begin(), m_cacheList, it->second.second);
        result = it->second.first;
        return true;
    }

    void Insert(HWND hwnd, T value)
    {
        auto it = m_cacheMap.find(hwnd);
        if (it != m_cacheMap.end()) 
        {
            it->second.first = value;
            m_cacheList.splice(m_cacheList.begin(), m_cacheList, it->second.second);
            return;
        }

        m_cacheList.push_front(hwnd);
        m_cacheMap[hwnd] = { value, m_cacheList.begin() };

        if (m_cacheMap.size() > m_maxSize) 
        {
            HWND lru = m_cacheList.back();
            m_cacheList.pop_back();
            m_cacheMap.erase(lru);
        }
    }

    std::future<void> InsertAsync(HWND hwnd, T value)
    {
        return std::async(std::launch::async, [this, hwnd, value]() {
            std::lock_guard<std::mutex> lock(m_mutex);
            this->Insert(hwnd, value);
        });
    }

    size_t GetCacheSize()
    {
        return m_cacheList.size();
    }

protected:
    size_t m_maxSize;
    std::list<HWND> m_cacheList;
    std::unordered_map<HWND, std::pair<T, std::list<HWND>::iterator>> m_cacheMap;
    std::mutex m_mutex;
};

HWNDCache g_hwndCache(kMaxCacheEntries);

bool IsWindowsExplorerWindow() {
    Wh_Log(L"Cache size: %d", g_hwndCache.GetCacheSize());

    HWND currentWindow = GetForegroundWindow();
    if (!currentWindow) {
        Wh_Log(L"No foreground window found");
        return false;
    }

    bool cachedResult = false;
    if (g_hwndCache.TryGet(currentWindow, cachedResult)) {
        Wh_Log(L"Using cached result for HWND %p: %d", currentWindow, cachedResult);
        return cachedResult;
    }
    
    wchar_t className[MAX_CLASS_NAME] = {};
    if (GetClassNameW(currentWindow, className, _countof(className)) == 0) {
        Wh_Log(L"Failed to get class name for HWND %p", currentWindow);
        return false;
    }

    bool isExplorer = wcsstr(className, L"CabinetWClass") != nullptr;
    g_hwndCache.InsertAsync(currentWindow, isExplorer);

    return isExplorer;
}