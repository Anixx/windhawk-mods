// ==WindhawkMod==
// @id              flexible-explorer-toolbars-deluxe-fork
// @name            Flexible Explorer Toolbars Deluxe - Fork
// @description     Makes Search Bar, Breadcrumb Bar and others into movable toolbars
// @version         1.3
// @author          Anixx
// @github          https://github.com/Anixx
// @include         explorer.exe
// @compilerOptions -lcomctl32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*

# Flexible Explorer Toolbars Deluxe

**!Important!** This mod curently only supports Windows 10 or Windows 11 versions up to 23H2 and 24H2/25H2 builds up to 8037. 
On 24H2 and 25H2 you may have to use vivetool to enable toolbars in Explorer: `vivetool /disable /id:55063786`.
On later, unsuported builds you may need to replace the Explorerframe.dll from an earlier version.

**!Important!** To use this mod, you shoud disable any other mods that hide the classic Navigation bar, such as the `Disable Navigation Bar` mod by ItsProfessional.
This mod hides the Navigation Bar by itself. 

For this mod to work you should enable a mod that restores the Navigation bar, it is recommended to install the `Windows 7 Comand Bar` mod, altough, `Classic Explorer navigation bar`
also would work if you want to retain elements of Windows 11 fluent interface, as well or any modification that restores ribbon.

This mod hides the Navigation Bar and instead creates the following optional toolbars, which could be freely moved and ordered together with the Menu Bar, if it is enabled:

* The Search bar

* The Breadcrumbs Bar

* The Up Buton

The toolbars can be locked and unlocked.
If you are using this mod together with Classic Explorer toolbar (Open Shell), enable that toolbar before enabling this mod, otherwise its enabled state will not be remembered.

**Toolbar visibility** is controlled from the right-click context menu of any of the movable toolbars (or of the Menu Bar itself) — the same menu where "Lock the Toolbars" is located.
Three checkable items are added there ("Search Bar", "Address" and "Up") that let you show/hide the corresponding toolbar on the fly. The choice is stored via the Windhawk Storage API.

By default, only the Search Bar is shown; the Address (breadcrumb) bar and the Up button are hidden until explicitly enabled from that context menu.

# Further adjustments

* It is recommended to install mod [Explorer Unlocked Toolbars Fix (WINAPI)](https://windhawk.net/mods/explorer-no-toolbars-bottom-gripper) to make the unlocked toolbars to appear better.

* To make the toolbars to have the 3D borders, install this mod: [Separators around File Explorer toolbars](https://windhawk.net/mods/explorer-toolbars-separators).

* To fix the appearance of the default text in the search bar under dark Classic theme, install this mod: [Classic Theme Explorer Search Fix](https://windhawk.net/mods/classic-theme-explorer-search-fix).

![screnshot](https://i.imgur.com/1YbTzZt.png)

![screnshot](https://i.imgur.com/OV8NRKJ.png)

![screnshot](https://i.imgur.com/OEthKme.png)

![screnshot](https://i.imgur.com/JXKEXL1.png)

![screnshot](https://i.imgur.com/QFFmczo.png)

*/

// ==/WindhawkModReadme==

#include <windhawk_utils.h>
#include <winternl.h>
#include <commctrl.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <algorithm>
#include <climits>
#include <windowsx.h>

constexpr int UP_BUTTON_ICON_SIZE  = 16;
constexpr UINT LOCK_TOOLBARS_CMD_ID = 41484;

constexpr UINT CMD_TOGGLE_SEARCHBAND = 0xF101;
constexpr UINT CMD_TOGGLE_BREADCRUMB = 0xF102;
constexpr UINT CMD_TOGGLE_UPBUTTON   = 0xF103;

constexpr UINT STR_ID_SEARCHBAR   = 34304; // "Search Box"          (ExplorerFrame.dll)
constexpr UINT STR_ID_BREADCRUMB  = 49952; // "Address"             (ExplorerFrame.dll)
constexpr UINT STR_ID_UPBUTTON    = 9026;  // "Up one level"        (ExplorerFrame.dll)

enum class BandType { Search, Breadcrumb, UpButton };

UINT g_msgDoMove = 0;

struct Settings {
    bool moveSearchBand    = true;
    bool moveBreadcrumb    = true;
    bool moveUpButton      = true;
} g_settings;

void LoadSettings() {
    // Defaults for a fresh install (no saved value yet):
    // only the Search Bar is shown, Breadcrumb/Up are off.
    g_settings.moveSearchBand = Wh_GetIntValue(L"MoveSearchBand", 1) != 0;
    g_settings.moveBreadcrumb = Wh_GetIntValue(L"MoveBreadcrumb", 0) != 0;
    g_settings.moveUpButton   = Wh_GetIntValue(L"MoveUpButton", 0) != 0;
}

void SaveSettingValue(const wchar_t* name, bool value) {
    Wh_SetIntValue(name, value ? 1 : 0);
}

CRITICAL_SECTION g_mutex;

std::unordered_map<HWND,bool>    g_alreadyMoved;
std::unordered_map<HWND,bool>    g_forceHiddenWorkers;
std::unordered_set<HWND>         g_neuteredAddressRoots;
std::unordered_map<HWND,HWND>    g_rebarToCabinet;
std::unordered_map<HWND,HWND>    g_cabinetToMenuRebar;
std::unordered_set<HWND>         g_pendingApply;
std::unordered_map<HWND,int>     g_applyAttempts;

std::unordered_map<HWND,HWND>    g_removedSearchBand;
std::unordered_map<HWND,HWND>    g_removedBreadcrumbBand;
std::unordered_map<HWND,HWND>    g_removedUpButtonBand;

enum ChildFlag { CF_MOVED=1, CF_UPBUTTON=2, CF_BREADCRUMB=4, CF_SEARCH=8 };
std::unordered_map<HWND,int> g_childFlags;

struct ToolbarGuard{int x=0,y=0,cx=0,cy=0;bool hasGood=false;};
std::unordered_map<HWND,ToolbarGuard> g_toolbarGuards;

std::unordered_map<HWND, WindhawkUtils::WH_SUBCLASSPROC> g_subclassedWindows;

thread_local bool g_insideApply = false;
thread_local int  g_rebarLayoutDepth = 0;
thread_local bool g_insideGripperSync = false;
thread_local bool g_insideUpButtonResize = false;

// Set to true only around SendMessage(..., TB_AUTOSIZE, ...) calls
// that WE deliberately issue on a moved Breadcrumb toolbar (e.g. to
// correct its size right after inserting it into the menu rebar).
// BreadcrumbToolbar_SubclassProc's WM_WINDOWPOSCHANGING guard treats
// such calls the same as a resize coming from inside the rebar's own
// layout pass: it lets it through and remembers it as the new "good"
// geometry. Any OTHER, uncontrolled attempt by Explorer to reposition
// this toolbar (e.g. its own address-band logic reacting to mouse
// activity near it) is still blocked and reverted to that cached good
// geometry - this is what prevents the toolbar from randomly jumping
// to another rebar row.
thread_local bool g_allowControlledReposition = false;

bool IsInsideRebarLayout() { return g_rebarLayoutDepth > 0; }

void HookWindow(HWND hwnd, WindhawkUtils::WH_SUBCLASSPROC subclassProc);

int GetDesiredBandHeight() {
    return GetSystemMetrics(SM_CYSIZE) + GetSystemMetrics(SM_CYBORDER) * 2 + 2;
}

using NtSetValueKey_t = NTSTATUS(NTAPI*)(
    HANDLE KeyHandle,
    PUNICODE_STRING ValueName,
    ULONG TitleIndex,
    ULONG Type,
    PVOID Data,
    ULONG DataSize);

NtSetValueKey_t NtSetValueKey_Original = nullptr;

bool UnicodeStringEqualsIgnoreCase(PUNICODE_STRING vn, const wchar_t* target) {
    if (!vn || !vn->Buffer || vn->Length == 0) return false;
    size_t lenChars = vn->Length / sizeof(WCHAR);
    size_t tlen = wcslen(target);
    if (lenChars != tlen) return false;
    for (size_t i = 0; i < tlen; i++) {
        if (towlower(vn->Buffer[i]) != towlower(target[i])) return false;
    }
    return true;
}

NTSTATUS NTAPI NtSetValueKey_Hook(
    HANDLE KeyHandle,
    PUNICODE_STRING ValueName,
    ULONG TitleIndex,
    ULONG Type,
    PVOID Data,
    ULONG DataSize)
{
    if (UnicodeStringEqualsIgnoreCase(ValueName, L"ITBar7Layout")) {
        return 0;
    }
    return NtSetValueKey_Original(KeyHandle, ValueName, TitleIndex, Type, Data, DataSize);
}

bool WasAlreadyMoved(HWND c){EnterCriticalSection(&g_mutex);bool r=g_alreadyMoved.count(c)&&g_alreadyMoved[c];LeaveCriticalSection(&g_mutex);return r;}
void MarkMoved(HWND c){EnterCriticalSection(&g_mutex);g_alreadyMoved[c]=true;LeaveCriticalSection(&g_mutex);}
void UnmarkMoved(HWND c){EnterCriticalSection(&g_mutex);g_alreadyMoved.erase(c);LeaveCriticalSection(&g_mutex);}

bool IsForceHidden(HWND w){EnterCriticalSection(&g_mutex);bool r=g_forceHiddenWorkers.count(w)&&g_forceHiddenWorkers[w];LeaveCriticalSection(&g_mutex);return r;}
void MarkForceHidden(HWND w){EnterCriticalSection(&g_mutex);g_forceHiddenWorkers[w]=true;LeaveCriticalSection(&g_mutex);}
void UnmarkForceHidden(HWND w){EnterCriticalSection(&g_mutex);g_forceHiddenWorkers.erase(w);LeaveCriticalSection(&g_mutex);}

void MarkNeutered(HWND h){EnterCriticalSection(&g_mutex);g_neuteredAddressRoots.insert(h);LeaveCriticalSection(&g_mutex);}
bool IsNeutered(HWND h){EnterCriticalSection(&g_mutex);bool r=g_neuteredAddressRoots.count(h)!=0;LeaveCriticalSection(&g_mutex);return r;}
void UnmarkNeutered(HWND h){EnterCriticalSection(&g_mutex);g_neuteredAddressRoots.erase(h);LeaveCriticalSection(&g_mutex);}

void RegisterRebarCabinet(HWND r,HWND c){EnterCriticalSection(&g_mutex);g_rebarToCabinet[r]=c;LeaveCriticalSection(&g_mutex);}
HWND GetRebarCabinet(HWND r){EnterCriticalSection(&g_mutex);auto it=g_rebarToCabinet.find(r);HWND c=it!=g_rebarToCabinet.end()?it->second:NULL;LeaveCriticalSection(&g_mutex);return c;}
void UnregisterRebarCabinet(HWND r){EnterCriticalSection(&g_mutex);g_rebarToCabinet.erase(r);LeaveCriticalSection(&g_mutex);}

void RegisterCabinetMenuRebar(HWND cab,HWND r){EnterCriticalSection(&g_mutex);g_cabinetToMenuRebar[cab]=r;LeaveCriticalSection(&g_mutex);}
HWND GetCabinetMenuRebar(HWND cab){EnterCriticalSection(&g_mutex);auto it=g_cabinetToMenuRebar.find(cab);HWND r=it!=g_cabinetToMenuRebar.end()?it->second:NULL;LeaveCriticalSection(&g_mutex);return r;}
void UnregisterCabinetMenuRebar(HWND cab){EnterCriticalSection(&g_mutex);g_cabinetToMenuRebar.erase(cab);LeaveCriticalSection(&g_mutex);}

void MarkPendingApply(HWND r){EnterCriticalSection(&g_mutex);g_pendingApply.insert(r);LeaveCriticalSection(&g_mutex);}
bool IsPendingApply(HWND r){EnterCriticalSection(&g_mutex);bool v=g_pendingApply.count(r)!=0;LeaveCriticalSection(&g_mutex);return v;}
void ClearPendingApply(HWND r){EnterCriticalSection(&g_mutex);g_pendingApply.erase(r);LeaveCriticalSection(&g_mutex);}

void SetChildFlag(HWND h,int f){
    EnterCriticalSection(&g_mutex);
    g_childFlags[h]|=f;
    LeaveCriticalSection(&g_mutex);
}
void ClearChildFlag(HWND h,int f){
    EnterCriticalSection(&g_mutex);
    auto it=g_childFlags.find(h);
    if(it!=g_childFlags.end()){it->second&=~f;if(!it->second)g_childFlags.erase(it);}
    LeaveCriticalSection(&g_mutex);
}
bool HasChildFlag(HWND h,int f){
    EnterCriticalSection(&g_mutex);
    auto it=g_childFlags.find(h);
    bool r=it!=g_childFlags.end()&&(it->second&f);
    LeaveCriticalSection(&g_mutex);
    return r;
}
void ClearAllChildFlags(HWND h){EnterCriticalSection(&g_mutex);g_childFlags.erase(h);LeaveCriticalSection(&g_mutex);}

HWND GetCabinetAncestor(HWND hwnd){
    for(HWND c=hwnd;c;c=GetParent(c)){
        WCHAR cls[64];
        if(GetClassName(c,cls,ARRAYSIZE(cls))&&wcscmp(cls,L"CabinetWClass")==0)return c;
    }return NULL;
}

void CleanupCabinetState(HWND cab){
    UnmarkMoved(cab);
    
    HWND mr=GetCabinetMenuRebar(cab);
    if(mr&&IsWindow(mr)){
        UnregisterRebarCabinet(mr);
        ClearPendingApply(mr);
        EnterCriticalSection(&g_mutex);
        g_applyAttempts.erase(mr);
        LeaveCriticalSection(&g_mutex);
        
        int cnt=(int)SendMessage(mr,RB_GETBANDCOUNT,0,0);
        for(int i=0;i<cnt;i++){
            REBARBANDINFO rbi={sizeof(rbi)};rbi.fMask=RBBIM_CHILD;
            if(SendMessage(mr,RB_GETBANDINFO,i,(LPARAM)&rbi)&&rbi.hwndChild){
                ClearAllChildFlags(rbi.hwndChild);
                EnterCriticalSection(&g_mutex);
                g_toolbarGuards.erase(rbi.hwndChild);
                LeaveCriticalSection(&g_mutex);
            }
        }
    }
    UnregisterCabinetMenuRebar(cab);

    EnterCriticalSection(&g_mutex);
    g_removedSearchBand.erase(cab);
    g_removedBreadcrumbBand.erase(cab);
    g_removedUpButtonBand.erase(cab);
    LeaveCriticalSection(&g_mutex);

    // Explicitly clean up neutered leftovers (Breadcrumb Parent / UpBand).
    // These windows are never hooked, so WM_NCDESTROY won't self-clean them.
    EnterCriticalSection(&g_mutex);
    auto it = g_neuteredAddressRoots.begin();
    while (it != g_neuteredAddressRoots.end()) {
        HWND h = *it;
        if (!IsWindow(h) || GetCabinetAncestor(h) == cab) {
            it = g_neuteredAddressRoots.erase(it);
        } else {
            ++it;
        }
    }
    LeaveCriticalSection(&g_mutex);
}

void GetEffectiveClassName(HWND child, wchar_t* out, size_t outCount) {
    if (child && HasChildFlag(child, CF_UPBUTTON)) {
        wcsncpy_s(out, outCount, L"UpButtonToolbar", _TRUNCATE);
    } else if (child && HasChildFlag(child, CF_BREADCRUMB)) {
        wcsncpy_s(out, outCount, L"BreadcrumbToolbar", _TRUNCATE);
    } else if (child) {
        GetClassName(child, out, (int)outCount);
    } else {
        out[0] = L'\0';
    }
}

// ---------------------------------------------------------------------
// Per-class saved layout: order rank + size + break flag.
// Keyed by (effective) class name so it survives a band being removed
// from the rebar entirely (e.g. toolbar hidden via the context menu)
// and can be restored later at exactly the same spot.
// ---------------------------------------------------------------------

int GetSavedOrderRank(const wchar_t* cls){
    if(!cls||!cls[0])return INT_MAX;
    WCHAR ok[160];swprintf_s(ok,ARRAYSIZE(ok),L"OrderRank_%s",cls);
    return Wh_GetIntValue(ok, INT_MAX);
}
void SetSavedOrderRank(const wchar_t* cls,int rank){
    if(!cls||!cls[0])return;
    WCHAR ok[160];swprintf_s(ok,ARRAYSIZE(ok),L"OrderRank_%s",cls);
    Wh_SetIntValue(ok, rank);
}

void SaveBandPositions(HWND rebar){
    if(!rebar||!IsWindow(rebar))return;
    int cnt=(int)SendMessage(rebar,RB_GETBANDCOUNT,0,0);
    for(int i=0;i<cnt;i++){
        REBARBANDINFO rbi={sizeof(rbi)};rbi.fMask=RBBIM_SIZE|RBBIM_STYLE|RBBIM_CHILD;
        if(!SendMessage(rebar,RB_GETBANDINFO,i,(LPARAM)&rbi))continue;
        WCHAR cls[256]=L"";
        if(rbi.hwndChild&&IsWindow(rbi.hwndChild))
            GetEffectiveClassName(rbi.hwndChild,cls,ARRAYSIZE(cls));
        if(!cls[0])continue;
        SetSavedOrderRank(cls, i);
        WCHAR ck[160];swprintf_s(ck,ARRAYSIZE(ck),L"Cx_%s",cls);
        Wh_SetIntValue(ck, (int)rbi.cx);
        WCHAR bk[160];swprintf_s(bk,ARRAYSIZE(bk),L"Break_%s",cls);
        Wh_SetIntValue(bk, (rbi.fStyle&RBBS_BREAK)?1:0);
    }
}

struct BandState{UINT cx;bool brk;};

bool LoadBandState(const wchar_t* cls,BandState& out){
    WCHAR ck[160];swprintf_s(ck,ARRAYSIZE(ck),L"Cx_%s",cls);
    WCHAR bk[160];swprintf_s(bk,ARRAYSIZE(bk),L"Break_%s",cls);
    int cx = Wh_GetIntValue(ck, -1);
    if(cx < 20 || cx > 8000) return false;
    int brk = Wh_GetIntValue(bk, 0);
    out.cx = (UINT)cx;
    out.brk = brk != 0;
    return true;
}

// Actually applies the small icon size to the Up button toolbar.
// TB_SETBITMAPSIZE alone has no documented effect on bitmaps already
// added to the toolbar, so we also rely on the reactive re-invocation
// points below (called on the messages that can make Explorer reset
// this) to make sure the change actually sticks visually.
void ResizeUpButtonToolbar(HWND toolbar) {
    if (!toolbar || !IsWindow(toolbar)) return;
    if (g_insideUpButtonResize) return;
    g_insideUpButtonResize = true;

    SendMessage(toolbar, TB_SETBITMAPSIZE, 0, MAKELONG(UP_BUTTON_ICON_SIZE, UP_BUTTON_ICON_SIZE));
    SendMessage(toolbar, TB_SETPADDING, 0, MAKELONG(4, 4));
    SendMessage(toolbar, TB_AUTOSIZE, 0, 0);

    g_insideUpButtonResize = false;
}

// Applies TB_AUTOSIZE to a moved Breadcrumb toolbar while marking the
// call as "controlled" (see g_allowControlledReposition above), so
// the resulting resize actually takes visual effect immediately and
// is remembered as the toolbar's new "good" geometry, instead of
// being reverted by BreadcrumbToolbar_SubclassProc's protective
// WM_WINDOWPOSCHANGING guard.
void ApplyBreadcrumbAutosize(HWND toolbar) {
    if (!toolbar || !IsWindow(toolbar)) return;
    g_allowControlledReposition = true;
    SendMessage(toolbar, TB_AUTOSIZE, 0, 0);
    g_allowControlledReposition = false;
}

// Re-applies the visual size of the moved Up button / Breadcrumb
// toolbars found inside the given rebar. Called synchronously as part
// of the normal layout pipeline (relayout / saved-layout application),
// not from a timer, so it stays in sync with real layout passes.
void ReapplyBandVisualSizes(HWND menuRebar){
    if(!menuRebar||!IsWindow(menuRebar))return;
    int cnt=(int)SendMessage(menuRebar,RB_GETBANDCOUNT,0,0);
    for(int i=0;i<cnt;i++){
        REBARBANDINFO rbi={sizeof(rbi)};rbi.fMask=RBBIM_CHILD;
        if(!SendMessage(menuRebar,RB_GETBANDINFO,i,(LPARAM)&rbi)||!rbi.hwndChild)continue;
        if(HasChildFlag(rbi.hwndChild, CF_UPBUTTON)){
            ResizeUpButtonToolbar(rbi.hwndChild);
        } else if(HasChildFlag(rbi.hwndChild, CF_BREADCRUMB)){
            ApplyBreadcrumbAutosize(rbi.hwndChild);
        }
    }
}

void ReapplyCx(HWND rebar){
    ClearPendingApply(rebar);
    int cnt=(int)SendMessage(rebar,RB_GETBANDCOUNT,0,0);
    g_insideApply=true;
    bool anyChanged=false;
    for(int i=0;i<cnt;i++){
        REBARBANDINFO rbi={sizeof(rbi)};rbi.fMask=RBBIM_CHILD|RBBIM_SIZE|RBBIM_STYLE;
        if(!SendMessage(rebar,RB_GETBANDINFO,i,(LPARAM)&rbi))continue;
        WCHAR cls[256]=L"";
        if(rbi.hwndChild)GetEffectiveClassName(rbi.hwndChild,cls,ARRAYSIZE(cls));

        if(rbi.hwndChild){
            if(HasChildFlag(rbi.hwndChild, CF_UPBUTTON)){
                ResizeUpButtonToolbar(rbi.hwndChild);
            } else if(HasChildFlag(rbi.hwndChild, CF_BREADCRUMB)){
                ApplyBreadcrumbAutosize(rbi.hwndChild);
            }
        }

        BandState bs;
        if(!LoadBandState(cls,bs))continue;
        bool cxOk=rbi.cx==bs.cx;
        bool brkOk=((rbi.fStyle&RBBS_BREAK)!=0)==bs.brk;
        if(!cxOk||!brkOk){
            REBARBANDINFO set={sizeof(set)};
            set.fMask=RBBIM_SIZE|RBBIM_IDEALSIZE|RBBIM_STYLE;
            set.cx=bs.cx;set.cxIdeal=bs.cx;
            set.fStyle=rbi.fStyle;
            if(bs.brk)set.fStyle|=RBBS_BREAK;
            else set.fStyle&=~RBBS_BREAK;
            SendMessage(rebar,RB_SETBANDINFO,i,(LPARAM)&set);
            anyChanged=true;
        }
    }
    g_insideApply=false;
    if(anyChanged)MarkPendingApply(rebar);
}

void ApplySavedLayout(HWND rebar){
    if(!rebar||!IsWindow(rebar))return;
    int cnt=(int)SendMessage(rebar,RB_GETBANDCOUNT,0,0);
    if(cnt<=0)return;
    g_insideApply=true;

    struct BandInfo{ int rank; HWND child; };
    std::vector<BandInfo> infos;
    infos.reserve(cnt);
    for(int i=0;i<cnt;i++){
        REBARBANDINFO rbi={sizeof(rbi)};rbi.fMask=RBBIM_CHILD;
        if(!SendMessage(rebar,RB_GETBANDINFO,i,(LPARAM)&rbi))continue;
        WCHAR cls[256]=L"";
        if(rbi.hwndChild)GetEffectiveClassName(rbi.hwndChild,cls,ARRAYSIZE(cls));
        int rank=GetSavedOrderRank(cls);
        if(rank==INT_MAX)rank=i; // no saved rank yet -> keep current relative order
        infos.push_back({rank, rbi.hwndChild});
    }

    // stable_sort keeps relative order for equal ranks (e.g. bands
    // that never had a saved rank yet).
    std::stable_sort(infos.begin(), infos.end(),
        [](const BandInfo&a,const BandInfo&b){return a.rank<b.rank;});

    for(int target=0;target<(int)infos.size();target++){
        HWND wantChild=infos[target].child;
        int curCnt=(int)SendMessage(rebar,RB_GETBANDCOUNT,0,0);
        for(int j=target;j<curCnt;j++){
            REBARBANDINFO rbi={sizeof(rbi)};rbi.fMask=RBBIM_CHILD;
            if(!SendMessage(rebar,RB_GETBANDINFO,j,(LPARAM)&rbi))continue;
            if(rbi.hwndChild==wantChild){
                if(j!=target)SendMessage(rebar,RB_MOVEBAND,j,target);
                break;
            }
        }
    }

    cnt=(int)SendMessage(rebar,RB_GETBANDCOUNT,0,0);
    for(int i=0;i<cnt;i++){
        REBARBANDINFO rbi={sizeof(rbi)};rbi.fMask=RBBIM_CHILD|RBBIM_STYLE;
        if(!SendMessage(rebar,RB_GETBANDINFO,i,(LPARAM)&rbi))continue;
        WCHAR cls[256]=L"";
        if(rbi.hwndChild)GetEffectiveClassName(rbi.hwndChild,cls,ARRAYSIZE(cls));

        if(rbi.hwndChild){
            if(HasChildFlag(rbi.hwndChild, CF_UPBUTTON)){
                ResizeUpButtonToolbar(rbi.hwndChild);
            } else if(HasChildFlag(rbi.hwndChild, CF_BREADCRUMB)){
                ApplyBreadcrumbAutosize(rbi.hwndChild);
            }
        }

        BandState bs;
        if(!LoadBandState(cls,bs))continue;
        REBARBANDINFO set={sizeof(set)};
        set.fMask=RBBIM_SIZE|RBBIM_IDEALSIZE|RBBIM_STYLE;
        set.cx=bs.cx;set.cxIdeal=bs.cx;
        set.fStyle=rbi.fStyle;
        if(bs.brk)set.fStyle|=RBBS_BREAK;
        else set.fStyle&=~RBBS_BREAK;
        SendMessage(rebar,RB_SETBANDINFO,i,(LPARAM)&set);
    }
    g_insideApply=false;
    EnterCriticalSection(&g_mutex);
    g_applyAttempts[rebar]=0;
    LeaveCriticalSection(&g_mutex);
    MarkPendingApply(rebar);

    ReapplyBandVisualSizes(rebar);
}

HWND FindMenuBarRebar(HWND c){
    HWND s=FindWindowEx(c,NULL,L"ShellTabWindowClass",NULL);if(!s)return NULL;
    HWND w=FindWindowEx(s,NULL,L"WorkerW",NULL);if(!w)return NULL;
    return FindWindowEx(w,NULL,L"ReBarWindow32",NULL);
}
HWND FindMenuBarWorkerW(HWND cab){
    HWND s=FindWindowEx(cab,NULL,L"ShellTabWindowClass",NULL);if(!s)return NULL;
    return FindWindowEx(s,NULL,L"WorkerW",NULL);
}
HWND FindNavbarRebar(HWND c){
    HWND w=FindWindowEx(c,NULL,L"WorkerW",NULL);
    while(w){HWND r=FindWindowEx(w,NULL,L"ReBarWindow32",NULL);if(r)return r;w=FindWindowEx(c,w,L"WorkerW",NULL);}
    return NULL;
}

HWND FindUpButton(HWND cab) {
    HWND navRebar = FindNavbarRebar(cab);
    if (!navRebar) return NULL;
    int cnt = (int)SendMessage(navRebar, RB_GETBANDCOUNT, 0, 0);
    for (int i = 0; i < cnt; i++) {
        REBARBANDINFO rbi = {sizeof(rbi)};
        rbi.fMask = RBBIM_CHILD;
        if (!SendMessage(navRebar, RB_GETBANDINFO, i, (LPARAM)&rbi)) continue;
        if (!rbi.hwndChild) continue;
        WCHAR cls[256];
        if (GetClassName(rbi.hwndChild, cls, ARRAYSIZE(cls))) {
            if (wcscmp(cls, L"UpBand") == 0) {
                HWND toolbar = FindWindowEx(rbi.hwndChild, NULL, L"ToolbarWindow32", NULL);
                if (toolbar) return toolbar;
            }
        }
    }
    return NULL;
}

HWND FindShellTab(HWND c){return FindWindowEx(c,NULL,L"ShellTabWindowClass",NULL);}
bool IsRebarChildOfDirectWorkerW(HWND r,HWND cab){
    HWND p=GetParent(r);if(!p)return false;
    WCHAR cls[64];if(!GetClassName(p,cls,ARRAYSIZE(cls)))return false;
    if(wcscmp(cls,L"WorkerW")!=0)return false;
    return GetParent(p)==cab;
}
bool ContainsClass(HWND root,const wchar_t* target){
    WCHAR cls[256];if(!GetClassName(root,cls,ARRAYSIZE(cls)))return false;
    if(wcscmp(cls,target)==0)return true;
    struct L{static BOOL CALLBACK cb(HWND h,LPARAM l){
        auto*p=(std::pair<const wchar_t*,bool>*)l;
        WCHAR c[256];if(GetClassName(h,c,ARRAYSIZE(c))&&wcscmp(c,p->first)==0){p->second=true;return FALSE;}
        return TRUE;}};
    std::pair<const wchar_t*,bool> ctx{target,false};
    EnumChildWindows(root,L::cb,(LPARAM)&ctx);return ctx.second;
}
bool ContainsSearchBand(HWND h){
    return ContainsClass(h,L"UniversalSearchBand")||ContainsClass(h,L"Search Box")||ContainsClass(h,L"SearchEditBoxWrapperClass");
}
bool ContainsAddressBand(HWND h){return ContainsClass(h,L"Address Band Root");}
HWND FindBreadcrumbParent(HWND root){
    WCHAR cls[256];if(GetClassName(root,cls,ARRAYSIZE(cls))&&wcscmp(cls,L"Breadcrumb Parent")==0)return root;
    struct L{static BOOL CALLBACK cb(HWND h,LPARAM l){
        WCHAR c[256];if(GetClassName(h,c,ARRAYSIZE(c))&&wcscmp(c,L"Breadcrumb Parent")==0){*(HWND*)l=h;return FALSE;}
        return TRUE;}};
    HWND f=NULL;EnumChildWindows(root,L::cb,(LPARAM)&f);return f;
}
void ExpandShellTabToFillCabinet(HWND cab){
    HWND s=FindShellTab(cab);if(!s||!IsWindow(s))return;
    RECT rc;GetClientRect(cab,&rc);
    SetWindowPos(s,NULL,0,0,rc.right,rc.bottom,SWP_NOZORDER|SWP_NOACTIVATE);
}

void ForceHideNavWorker(HWND w){
    if(!w||!IsWindow(w))return;
    bool alreadyHidden = IsForceHidden(w) && !(GetWindowLongPtr(w,GWL_STYLE) & WS_VISIBLE);
    MarkForceHidden(w);
    if(alreadyHidden)return;
    LONG_PTR style=GetWindowLongPtr(w,GWL_STYLE);
    if(style & WS_VISIBLE)
        SetWindowLongPtr(w,GWL_STYLE,style & ~WS_VISIBLE);
    ShowWindow(w,SW_HIDE);
    SetWindowPos(w,NULL,0,0,0,0,
        SWP_HIDEWINDOW|SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOMOVE|SWP_FRAMECHANGED);
}

void SuppressStrayNavWorkers(HWND cab){
    if(!cab||!IsWindow(cab))return;
    for(HWND w=FindWindowEx(cab,NULL,L"WorkerW",NULL); w; w=FindWindowEx(cab,w,L"WorkerW",NULL)){
        if(FindWindowEx(w,NULL,L"ReBarWindow32",NULL)){
            ForceHideNavWorker(w);
        }
    }
}

// Forces the same layout pass that normally happens on a real resize
// or when the window is reopened, so that bands we just inserted /
// removed get their final geometry immediately instead of only after
// the user manually resizes the window.
void ForceCabinetRelayout(HWND cab) {
    if (!cab || !IsWindow(cab)) return;
    ExpandShellTabToFillCabinet(cab);
    SuppressStrayNavWorkers(cab);
    RECT rc;
    GetClientRect(cab, &rc);
    SendMessage(cab, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom));
    RedrawWindow(cab, NULL, NULL,
        RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_ERASENOW | RDW_FRAME);

    HWND mr = GetCabinetMenuRebar(cab);
    if (mr && IsWindow(mr)) {
        ReapplyBandVisualSizes(mr);
    }
}

DWORD GetReferenceGripperStyle(HWND rebar){
    int cnt=(int)SendMessage(rebar,RB_GETBANDCOUNT,0,0);
    for(int i=0;i<cnt;i++){
        REBARBANDINFO rbi={sizeof(rbi)};
        rbi.fMask=RBBIM_STYLE|RBBIM_CHILD;
        if(!SendMessage(rebar,RB_GETBANDINFO,i,(LPARAM)&rbi))continue;
        if(!rbi.hwndChild)continue;
        if(HasChildFlag(rbi.hwndChild, CF_MOVED))continue;
        return rbi.fStyle & (RBBS_GRIPPERALWAYS|RBBS_NOGRIPPER);
    }
    return RBBS_GRIPPERALWAYS;
}

bool AreToolbarsLockedByGripper(HWND rebar){
    int cnt=(int)SendMessage(rebar,RB_GETBANDCOUNT,0,0);
    for(int i=0;i<cnt;i++){
        REBARBANDINFO rbi={sizeof(rbi)};
        rbi.fMask=RBBIM_STYLE;
        if(!SendMessage(rebar,RB_GETBANDINFO,i,(LPARAM)&rbi))continue;
        return (rbi.fStyle & RBBS_NOGRIPPER) != 0;
    }
    return false;
}

void SyncMovedBandGrippers(HWND rebar){
    if(g_insideGripperSync) return;
    if(!rebar||!IsWindow(rebar))return;
    g_insideGripperSync=true;
    DWORD gripperBits=GetReferenceGripperStyle(rebar);
    int cnt=(int)SendMessage(rebar,RB_GETBANDCOUNT,0,0);
    for(int i=0;i<cnt;i++){
        REBARBANDINFO q={sizeof(q)};
        q.fMask=RBBIM_CHILD|RBBIM_STYLE;
        if(!SendMessage(rebar,RB_GETBANDINFO,i,(LPARAM)&q))continue;
        if(!q.hwndChild||!HasChildFlag(q.hwndChild, CF_MOVED))continue;
        DWORD curBits=q.fStyle&(RBBS_GRIPPERALWAYS|RBBS_NOGRIPPER);
        if(curBits!=gripperBits){
            REBARBANDINFO set={sizeof(set)};
            set.fMask=RBBIM_STYLE;
            set.fStyle=(q.fStyle&~(RBBS_GRIPPERALWAYS|RBBS_NOGRIPPER))|gripperBits;
            SendMessage(rebar,RB_SETBANDINFO,i,(LPARAM)&set);
        }
    }
    g_insideGripperSync=false;
}

void ShowToolbarContextMenu(HWND rebar, int x, int y) {
    if(!rebar || !IsWindow(rebar)) return;

    HWND cab = rebar;
    while (cab) {
        WCHAR cls[64];
        if (GetClassName(cab, cls, ARRAYSIZE(cls)) && !wcscmp(cls, L"CabinetWClass"))
            break;
        cab = GetParent(cab);
    }
    if (!cab) return;

    HWND workerW = FindMenuBarWorkerW(cab);
    if (!workerW) return;

    HMODULE hMod = GetModuleHandleW(L"explorerframe.dll");
    if (!hMod) return;

    HMENU hMenu = LoadMenuW(hMod, MAKEINTRESOURCEW(264));
    if (!hMenu) return;

    HMENU hSubMenu = GetSubMenu(hMenu, 0);
    if (!hSubMenu) { DestroyMenu(hMenu); return; }

    bool locked = AreToolbarsLockedByGripper(rebar);
    int itemCount = GetMenuItemCount(hSubMenu);
    for (int i = 0; i < itemCount; i++) {
        MENUITEMINFOW mii = {sizeof(mii)};
        mii.fMask = MIIM_ID;
        if (GetMenuItemInfoW(hSubMenu, i, TRUE, &mii)) {
            if (mii.wID == LOCK_TOOLBARS_CMD_ID) {
                MENUITEMINFOW setInfo = {sizeof(setInfo)};
                setInfo.fMask = MIIM_STATE;
                setInfo.fState = locked ? MFS_CHECKED : MFS_UNCHECKED;
                SetMenuItemInfoW(hSubMenu, i, TRUE, &setInfo);
                break;
            }
        }
    }

    TrackPopupMenuEx(hSubMenu,
        TPM_RIGHTBUTTON | TPM_LEFTBUTTON,
        x, y,
        workerW,
        NULL);
    
    PostMessage(workerW, WM_NULL, 0, 0);

    DestroyMenu(hMenu);
}

LRESULT CALLBACK UpButton_SubclassProc(HWND hwnd, UINT msg, WPARAM wP, LPARAM lP, DWORD_PTR) {
    if(msg == WM_CONTEXTMENU) {
        HWND rebar = GetParent(hwnd);
        if(rebar && IsWindow(rebar)) {
            POINT pt = { GET_X_LPARAM(lP), GET_Y_LPARAM(lP) };
            if(pt.x == -1 && pt.y == -1) {
                RECT rc; GetWindowRect(hwnd, &rc);
                pt.x = rc.left; pt.y = rc.bottom;
            }
            ShowToolbarContextMenu(rebar, pt.x, pt.y);
            return 0;
        }
    }

    LRESULT r = DefSubclassProc(hwnd, msg, wP, lP);

    // Re-apply our desired icon size *after* letting the default
    // handling run for messages that can reset it (Explorer
    // re-adding images/buttons, theme changes, resizes, etc). This is
    // event-driven (not a timer), so it fires exactly when needed.
    switch (msg) {
        case WM_SIZE:
        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE:
        case WM_SETTINGCHANGE:
        case WM_DPICHANGED:
        case TB_SETIMAGELIST:
        case TB_SETHOTIMAGELIST:
        case TB_ADDBUTTONSW:
        case TB_ADDBUTTONSA:
        case TB_INSERTBUTTONW:
        case TB_INSERTBUTTONA:
        case TB_LOADIMAGES:
        case TB_BUTTONSTRUCTSIZE:
            ResizeUpButtonToolbar(hwnd);
            break;
    }
    return r;
}

LRESULT CALLBACK BreadcrumbToolbar_SubclassProc(HWND hwnd,UINT msg,WPARAM wP,LPARAM lP,DWORD_PTR){
    if(msg == WM_CONTEXTMENU) {
        HWND rebar = GetParent(hwnd);
        if(rebar && IsWindow(rebar)) {
            POINT pt = { GET_X_LPARAM(lP), GET_Y_LPARAM(lP) };
            if(pt.x == -1 && pt.y == -1) {
                RECT rc; GetWindowRect(hwnd, &rc);
                pt.x = rc.left; pt.y = rc.bottom;
            }
            ShowToolbarContextMenu(rebar, pt.x, pt.y);
            return 0;
        }
    }
    if(msg==WM_WINDOWPOSCHANGING){
        auto*pos=(WINDOWPOS*)lP;
        EnterCriticalSection(&g_mutex);
        ToolbarGuard&g=g_toolbarGuards[hwnd];
        if(IsInsideRebarLayout() || g_allowControlledReposition){
            if(!(pos->flags&SWP_NOMOVE)){g.x=pos->x;g.y=pos->y;}
            if(!(pos->flags&SWP_NOSIZE)){g.cx=pos->cx;g.cy=pos->cy;}
            g.hasGood=true;pos->flags&=~SWP_HIDEWINDOW;
        }else if(g.hasGood){
            pos->x=g.x;pos->y=g.y;pos->cx=g.cx;pos->cy=g.cy;
            pos->flags&=~(SWP_HIDEWINDOW|SWP_NOMOVE|SWP_NOSIZE);pos->flags|=SWP_SHOWWINDOW;
        }
        LeaveCriticalSection(&g_mutex);
    }
    if(msg==WM_SHOWWINDOW&&!wP&&!IsInsideRebarLayout())return 0;

    LRESULT r = DefSubclassProc(hwnd,msg,wP,lP);

    switch (msg) {
        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE:
        case WM_SETTINGCHANGE:
        case WM_DPICHANGED:
        case TB_SETIMAGELIST:
        case TB_SETHOTIMAGELIST:
        case TB_ADDBUTTONSW:
        case TB_ADDBUTTONSA:
        case TB_INSERTBUTTONW:
        case TB_INSERTBUTTONA:
        case TB_LOADIMAGES:
            ApplyBreadcrumbAutosize(hwnd);
            break;
    }
    return r;
}

// ---------------------------------------------------------------------
// Toolbar visibility context-menu integration
// ---------------------------------------------------------------------

bool MenuContainsId(HMENU hMenu, UINT id, int depth = 0) {
    if (!hMenu) return false;
    int cnt = GetMenuItemCount(hMenu);
    for (int i = 0; i < cnt; i++) {
        UINT curId = GetMenuItemID(hMenu, i);
        if (curId == id) return true;
        if (curId == (UINT)-1 && depth < 2) {
            HMENU sub = GetSubMenu(hMenu, i);
            if (sub && MenuContainsId(sub, id, depth + 1)) return true;
        }
    }
    return false;
}

bool MenuHasItem(HMENU hMenu, UINT id) {
    MENUITEMINFOW mii = {sizeof(mii)};
    mii.fMask = MIIM_STATE;
    return GetMenuItemInfoW(hMenu, id, FALSE, &mii) != 0;
}

void SetMenuItemChecked(HMENU hMenu, UINT id, bool checked) {
    MENUITEMINFOW mii = {sizeof(mii)};
    mii.fMask = MIIM_STATE;
    mii.fState = checked ? MFS_CHECKED : MFS_UNCHECKED;
    SetMenuItemInfoW(hMenu, id, FALSE, &mii);
}

std::wstring LoadEFString(UINT id, const wchar_t* fallback) {
    HMODULE hMod = GetModuleHandleW(L"explorerframe.dll");
    if (hMod) {
        WCHAR buf[256];
        int len = LoadStringW(hMod, id, buf, ARRAYSIZE(buf));
        if (len > 0) return std::wstring(buf, len);
    }
    return fallback;
}

void AddOrUpdateToolbarMenuItems(HMENU hMenu) {
    if (!MenuHasItem(hMenu, CMD_TOGGLE_SEARCHBAND)) {
        std::wstring sSearch = LoadEFString(STR_ID_SEARCHBAR, L"Search Bar");
        std::wstring sBread  = LoadEFString(STR_ID_BREADCRUMB, L"Address");
        std::wstring sUp     = LoadEFString(STR_ID_UPBUTTON, L"Up");

        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hMenu, MF_STRING, CMD_TOGGLE_SEARCHBAND, sSearch.c_str());
        AppendMenuW(hMenu, MF_STRING, CMD_TOGGLE_BREADCRUMB, sBread.c_str());
        AppendMenuW(hMenu, MF_STRING, CMD_TOGGLE_UPBUTTON,   sUp.c_str());
    }
    SetMenuItemChecked(hMenu, CMD_TOGGLE_SEARCHBAND, g_settings.moveSearchBand);
    SetMenuItemChecked(hMenu, CMD_TOGGLE_BREADCRUMB, g_settings.moveBreadcrumb);
    SetMenuItemChecked(hMenu, CMD_TOGGLE_UPBUTTON,   g_settings.moveUpButton);
}

HWND FindMovedBandChild(HWND rebar, int flagBit) {
    if (!rebar || !IsWindow(rebar)) return NULL;
    int cnt = (int)SendMessage(rebar, RB_GETBANDCOUNT, 0, 0);
    for (int i = 0; i < cnt; i++) {
        REBARBANDINFO rbi = {sizeof(rbi)};
        rbi.fMask = RBBIM_CHILD;
        if (!SendMessage(rebar, RB_GETBANDINFO, i, (LPARAM)&rbi)) continue;
        if (rbi.hwndChild && HasChildFlag(rbi.hwndChild, flagBit)) return rbi.hwndChild;
    }
    return NULL;
}

int FindBandIndexForChild(HWND rebar, HWND child) {
    int cnt = (int)SendMessage(rebar, RB_GETBANDCOUNT, 0, 0);
    for (int i = 0; i < cnt; i++) {
        REBARBANDINFO rbi = {sizeof(rbi)};
        rbi.fMask = RBBIM_CHILD;
        if (!SendMessage(rebar, RB_GETBANDINFO, i, (LPARAM)&rbi)) continue;
        if (rbi.hwndChild == child) return i;
    }
    return -1;
}

void RemoveMovedBand(HWND cab, BandType type) {
    int flagBit = (type == BandType::Search) ? CF_SEARCH :
                  (type == BandType::Breadcrumb) ? CF_BREADCRUMB : CF_UPBUTTON;
    auto& store = (type == BandType::Search) ? g_removedSearchBand :
                  (type == BandType::Breadcrumb) ? g_removedBreadcrumbBand : g_removedUpButtonBand;

    HWND menuRebar = GetCabinetMenuRebar(cab);
    if (!menuRebar || !IsWindow(menuRebar)) return;

    HWND child = FindMovedBandChild(menuRebar, flagBit);
    if (!child) return;

    // Persist the current order/size of every band (including this one)
    // before it disappears from the rebar, so it can be restored later
    // exactly where (and how wide) it was.
    SaveBandPositions(menuRebar);

    int idx = FindBandIndexForChild(menuRebar, child);
    if (idx >= 0) SendMessage(menuRebar, RB_DELETEBAND, idx, 0);

    ShowWindow(child, SW_HIDE);
    ClearAllChildFlags(child);

    EnterCriticalSection(&g_mutex);
    store[cab] = child;
    LeaveCriticalSection(&g_mutex);

    ForceCabinetRelayout(cab);
}

void EnableBand(HWND cab, BandType type) {
    int flagBit = (type == BandType::Search) ? CF_SEARCH :
                  (type == BandType::Breadcrumb) ? CF_BREADCRUMB : CF_UPBUTTON;
    auto& store = (type == BandType::Search) ? g_removedSearchBand :
                  (type == BandType::Breadcrumb) ? g_removedBreadcrumbBand : g_removedUpButtonBand;

    HWND menuRebar = GetCabinetMenuRebar(cab);
    if (!menuRebar || !IsWindow(menuRebar)) return;

    if (FindMovedBandChild(menuRebar, flagBit)) return; // already shown

    HWND child = NULL;
    int width = 200;

    EnterCriticalSection(&g_mutex);
    auto it = store.find(cab);
    if (it != store.end() && IsWindow(it->second)) {
        child = it->second;
        store.erase(it);
    }
    LeaveCriticalSection(&g_mutex);

    if (!child) {
        HWND navRebar = FindNavbarRebar(cab);
        if (!navRebar) return;
        int cnt = (int)SendMessage(navRebar, RB_GETBANDCOUNT, 0, 0);
        HWND leftover = NULL;
        for (int i = 0; i < cnt; i++) {
            REBARBANDINFO rbi = {sizeof(rbi)};
            rbi.fMask = RBBIM_CHILD;
            if (!SendMessage(navRebar, RB_GETBANDINFO, i, (LPARAM)&rbi) || !rbi.hwndChild) continue;

            if (type == BandType::Search && ContainsSearchBand(rbi.hwndChild)) {
                RECT rc; GetWindowRect(rbi.hwndChild, &rc);
                int w = rc.right - rc.left; if (w < 50) w = 200;
                child = rbi.hwndChild; width = w;
                SendMessage(navRebar, RB_DELETEBAND, i, 0);
                break;
            } else if (type == BandType::Breadcrumb && ContainsAddressBand(rbi.hwndChild)) {
                HWND bc = FindBreadcrumbParent(rbi.hwndChild);
                if (bc) {
                    HWND bcToolbar = FindWindowEx(bc, NULL, L"ToolbarWindow32", NULL);
                    if (bcToolbar) {
                        RECT rc; GetWindowRect(bcToolbar, &rc);
                        int w = rc.right - rc.left; if (w < 50) w = 250;
                        child = bcToolbar; width = w; leftover = bc;
                        SendMessage(navRebar, RB_DELETEBAND, i, 0);
                        break;
                    }
                }
            } else if (type == BandType::UpButton) {
                WCHAR cls[256];
                if (GetClassName(rbi.hwndChild, cls, ARRAYSIZE(cls)) && !wcscmp(cls, L"UpBand")) {
                    HWND upButton = FindWindowEx(rbi.hwndChild, NULL, L"ToolbarWindow32", NULL);
                    if (upButton) {
                        RECT rc; GetWindowRect(upButton, &rc);
                        int w = rc.right - rc.left; if (w < 20) w = 30;
                        child = upButton; width = w; leftover = rbi.hwndChild;
                        SendMessage(navRebar, RB_DELETEBAND, i, 0);
                        break;
                    }
                }
            }
        }
        if (!child) return;
        if (leftover && IsWindow(leftover)) {
            MarkNeutered(leftover);
            ShowWindow(leftover, SW_HIDE);
        }
    }

    if (!IsWindow(child)) return;

    SetParent(child, menuRebar);

    WCHAR cls[256] = L"";
    if (type == BandType::UpButton) wcsncpy_s(cls, ARRAYSIZE(cls), L"UpButtonToolbar", _TRUNCATE);
    else if (type == BandType::Breadcrumb) wcsncpy_s(cls, ARRAYSIZE(cls), L"BreadcrumbToolbar", _TRUNCATE);
    else GetClassName(child, cls, ARRAYSIZE(cls));

    BandState bs;
    bool hasSaved = LoadBandState(cls, bs);
    UINT useCx = hasSaved ? bs.cx : (UINT)width;

    if (type == BandType::UpButton) {
        ResizeUpButtonToolbar(child);
        if (!hasSaved) {
            SIZE idealSz{};
            if (SendMessage(child, TB_GETIDEALSIZE, FALSE, (LPARAM)&idealSz) && idealSz.cx > 0) {
                useCx = (UINT)idealSz.cx;
            }
        }
    } else if (type == BandType::Breadcrumb) {
        SendMessage(child, TB_AUTOSIZE, 0, 0);
    }

    int bandHeight = GetDesiredBandHeight();
    DWORD gripperStyle = GetReferenceGripperStyle(menuRebar);

    REBARBANDINFO rbi = {sizeof(rbi)};
    rbi.fMask = RBBIM_STYLE | RBBIM_CHILD | RBBIM_CHILDSIZE | RBBIM_SIZE | RBBIM_IDEALSIZE;
    rbi.fStyle = gripperStyle;
    if (hasSaved && bs.brk) rbi.fStyle |= RBBS_BREAK;
    rbi.hwndChild = child;
    rbi.cyMinChild = bandHeight;
    rbi.cyMaxChild = bandHeight;
    rbi.cyChild = bandHeight;
    rbi.cx = useCx;
    rbi.cxIdeal = useCx;
    rbi.cyIntegral = 1;

    BOOL ins = (BOOL)SendMessage(menuRebar, RB_INSERTBAND, (WPARAM)-1, (LPARAM)&rbi);
    if (ins) {
        int flags = CF_MOVED | flagBit;
        SetChildFlag(child, flags);
        if (type == BandType::UpButton) {
            HookWindow(child, UpButton_SubclassProc);
            ResizeUpButtonToolbar(child);
        } else if (type == BandType::Breadcrumb) {
            HookWindow(child, BreadcrumbToolbar_SubclassProc);
        }
        ShowWindow(child, SW_SHOW);

        // The band was appended at the end; put it back where it used
        // to be (based on the saved per-class order rank) instead of
        // leaving it stuck at the last position. This also reapplies
        // the correct visual size for Up/Breadcrumb bands.
        ApplySavedLayout(menuRebar);
        SyncMovedBandGrippers(menuRebar);
    } else {
        ShowWindow(child, SW_HIDE);
        EnterCriticalSection(&g_mutex);
        store[cab] = child;
        LeaveCriticalSection(&g_mutex);
    }

    ForceCabinetRelayout(cab);
}

void ApplyToolbarVisibilityForAllCabinets(BandType type, bool enable) {
    DWORD curPid = GetCurrentProcessId();
    for (HWND w = GetTopWindow(NULL); w; w = GetNextWindow(w, GW_HWNDNEXT)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid != curPid) continue;
        WCHAR cls[64];
        if (!GetClassName(w, cls, ARRAYSIZE(cls)) || wcscmp(cls, L"CabinetWClass")) continue;
        if (!WasAlreadyMoved(w)) continue;

        if (enable) EnableBand(w, type);
        else RemoveMovedBand(w, type);
    }
}

bool HandleToolbarMenuCommand(UINT cmd) {
    BandType type;
    bool* setting;
    const wchar_t* storageName;

    switch (cmd) {
        case CMD_TOGGLE_SEARCHBAND:
            type = BandType::Search;
            setting = &g_settings.moveSearchBand;
            storageName = L"MoveSearchBand";
            break;
        case CMD_TOGGLE_BREADCRUMB:
            type = BandType::Breadcrumb;
            setting = &g_settings.moveBreadcrumb;
            storageName = L"MoveBreadcrumb";
            break;
        case CMD_TOGGLE_UPBUTTON:
            type = BandType::UpButton;
            setting = &g_settings.moveUpButton;
            storageName = L"MoveUpButton";
            break;
        default:
            return false;
    }

    *setting = !(*setting);
    SaveSettingValue(storageName, *setting);
    ApplyToolbarVisibilityForAllCabinets(type, *setting);
    return true;
}

// ---------------------------------------------------------------------

LRESULT CALLBACK AddressBandRoot_SubclassProc(HWND hwnd,UINT msg,WPARAM wP,LPARAM lP,DWORD_PTR){
    if(IsNeutered(hwnd))return DefWindowProc(hwnd,msg,wP,lP);
    return DefSubclassProc(hwnd,msg,wP,lP);
}

LRESULT CALLBACK NavWorkerW_SubclassProc(HWND hwnd,UINT msg,WPARAM wP,LPARAM lP,DWORD_PTR){
    if(IsForceHidden(hwnd)){
        switch(msg){
            case WM_NCDESTROY:
                UnmarkForceHidden(hwnd);
                return DefSubclassProc(hwnd,msg,wP,lP);
            case WM_STYLECHANGING:
                if(wP==GWL_STYLE){
                    auto*ss=(STYLESTRUCT*)lP;
                    ss->styleNew &= ~WS_VISIBLE;
                }
                return DefSubclassProc(hwnd,msg,wP,lP);
            case WM_WINDOWPOSCHANGING: {
                auto*pos=(WINDOWPOS*)lP;
                pos->flags &= ~SWP_SHOWWINDOW;
                pos->flags |= SWP_HIDEWINDOW;
                pos->cx = 0;
                pos->cy = 0;
                return DefSubclassProc(hwnd,msg,wP,lP);
            }
            case WM_SHOWWINDOW:
                if(wP) ShowWindow(hwnd, SW_HIDE);
                return 0;
            case WM_SIZE:
            case WM_MOVE:
                return 0;
            default:
                return DefSubclassProc(hwnd,msg,wP,lP);
        }
    }
    return DefSubclassProc(hwnd,msg,wP,lP);
}

LRESULT CALLBACK ShellTab_SubclassProc(HWND hwnd,UINT msg,WPARAM wP,LPARAM lP,DWORD_PTR){
    if(msg==WM_WINDOWPOSCHANGING){
        HWND cab=GetParent(hwnd);
        if(cab&&WasAlreadyMoved(cab)){
            auto*p=(WINDOWPOS*)lP;RECT rc;GetClientRect(cab,&rc);
            p->x=0;p->y=0;p->cx=rc.right;p->cy=rc.bottom;
            p->flags=(p->flags&~(SWP_NOMOVE|SWP_NOSIZE|SWP_HIDEWINDOW))|SWP_NOZORDER|SWP_NOACTIVATE;
        }
    }
    return DefSubclassProc(hwnd,msg,wP,lP);
}

LRESULT CALLBACK MenuReBarParent_SubclassProc(HWND hwnd,UINT msg,WPARAM wP,LPARAM lP,DWORD_PTR){
    if(msg==WM_NOTIFY){
        auto*hdr=(NMHDR*)lP;
        if(hdr->code==RBN_LAYOUTCHANGED||hdr->code==RBN_ENDDRAG){
            HWND rebar=hdr->hwndFrom;
            HWND cab=GetRebarCabinet(rebar);
            if(!cab)cab=GetCabinetAncestor(rebar);
            if(cab&&WasAlreadyMoved(cab)&&!g_insideApply)
                SaveBandPositions(rebar);
        }
    }
    return DefSubclassProc(hwnd,msg,wP,lP);
}

LRESULT CALLBACK ReBar_SubclassProc(HWND hwnd,UINT msg,WPARAM wP,LPARAM lP,DWORD_PTR){
    if(msg==WM_CONTEXTMENU){
        POINT pt = { GET_X_LPARAM(lP), GET_Y_LPARAM(lP) };
        bool isMovedBand = false;
        HWND src = (HWND)wP;

        if (pt.x != -1 || pt.y != -1) {
            POINT clientPt = pt;
            ScreenToClient(hwnd, &clientPt);
            RBHITTESTINFO hti = {0};
            hti.pt = clientPt;
            int bandIdx = (int)SendMessage(hwnd, RB_HITTEST, 0, (LPARAM)&hti);
            if (bandIdx >= 0) {
                REBARBANDINFO rbi = {sizeof(rbi)};
                rbi.fMask = RBBIM_CHILD;
                if (SendMessage(hwnd, RB_GETBANDINFO, bandIdx, (LPARAM)&rbi)) {
                    if (rbi.hwndChild && HasChildFlag(rbi.hwndChild, CF_MOVED)) {
                        isMovedBand = true;
                    }
                }
            }
        } else {
            bool srcMoved = src && HasChildFlag(src, CF_MOVED);
            HWND srcParent = src ? GetParent(src) : NULL;
            bool parentMoved = srcParent && HasChildFlag(srcParent, CF_MOVED);
            isMovedBand = srcMoved || parentMoved;
        }

        if (isMovedBand) {
            int x = pt.x, y = pt.y;
            if (x == -1 && y == -1) {
                RECT rc; GetWindowRect(hwnd, &rc);
                x = rc.left; y = rc.top;
            }
            ShowToolbarContextMenu(hwnd, x, y);
            return 0;
        }
    }

    if(msg==RB_SETBANDINFO){
        auto*inf=(REBARBANDINFO*)lP;
        if(inf&&(inf->fMask&RBBIM_CHILDSIZE)){
            HWND ch=NULL;
            if(inf->fMask&RBBIM_CHILD)ch=inf->hwndChild;
            else{REBARBANDINFO q={sizeof(q)};q.fMask=RBBIM_CHILD;if(SendMessage(hwnd,RB_GETBANDINFO,wP,(LPARAM)&q))ch=q.hwndChild;}
            if(ch&&HasChildFlag(ch, CF_MOVED)){
                int bandHeight = GetDesiredBandHeight();
                inf->cyMinChild=inf->cyChild=inf->cyMaxChild=bandHeight;
                inf->cyIntegral=1;
            }
        }
    }
    
    g_rebarLayoutDepth++;
    LRESULT r=DefSubclassProc(hwnd,msg,wP,lP);
    g_rebarLayoutDepth--;

    if(msg==WM_SIZE&&IsPendingApply(hwnd)&&!g_insideApply){
        EnterCriticalSection(&g_mutex);
        int&attempts=g_applyAttempts[hwnd];
        bool canRetry=(attempts<5);
        if(canRetry)attempts++;
        LeaveCriticalSection(&g_mutex);
        if(canRetry)ReapplyCx(hwnd);
        else ClearPendingApply(hwnd);
    }
    if(msg==RB_INSERTBAND){
        HWND cab=GetCabinetAncestor(hwnd);
        if(cab&&!WasAlreadyMoved(cab)&&IsRebarChildOfDirectWorkerW(hwnd,cab))
            PostMessage(cab,g_msgDoMove,0,0);
    }
    if(msg==RB_SETBANDINFO && !g_insideGripperSync){
        HWND cab=GetCabinetAncestor(hwnd);
        if(cab && WasAlreadyMoved(cab)){
            SyncMovedBandGrippers(hwnd);
        }
    }
    if(msg==WM_MOUSEMOVE || msg==WM_LBUTTONUP) {
        HWND cab=GetRebarCabinet(hwnd);
        if(!cab)cab=GetCabinetAncestor(hwnd);
        if(cab&&WasAlreadyMoved(cab)&&!g_insideApply) {
            static DWORD lastSave = 0;
            DWORD now = GetTickCount();
            if(msg==WM_LBUTTONUP || (now - lastSave > 1000)) {
                SaveBandPositions(hwnd);
                lastSave = now;
            }
        }
    }
    return r;
}

bool DoMoveSearchBandToMenuBar(HWND cabinetWnd);

LRESULT CALLBACK Cabinet_SubclassProc(HWND hwnd,UINT msg,WPARAM wP,LPARAM lP,DWORD_PTR){
    if(msg==WM_CLOSE||msg==WM_DESTROY){
        if(WasAlreadyMoved(hwnd)){
            HWND mr=GetCabinetMenuRebar(hwnd);
            if(mr&&IsWindow(mr))SaveBandPositions(mr);
        }
        CleanupCabinetState(hwnd);
    }
    if(msg==g_msgDoMove){DoMoveSearchBandToMenuBar(hwnd);return 0;}
    if((msg==WM_SIZE||msg==WM_WINDOWPOSCHANGED)&&WasAlreadyMoved(hwnd)){
        LRESULT r=DefSubclassProc(hwnd,msg,wP,lP);
        SuppressStrayNavWorkers(hwnd);
        ExpandShellTabToFillCabinet(hwnd);
        return r;
    }
    if((msg==WM_ACTIVATE||msg==WM_SETFOCUS)&&!WasAlreadyMoved(hwnd))
        PostMessage(hwnd,g_msgDoMove,0,0);
    return DefSubclassProc(hwnd,msg,wP,lP);
}

bool DoMoveSearchBandToMenuBar(HWND cabinetWnd){
    if(WasAlreadyMoved(cabinetWnd)){
        ExpandShellTabToFillCabinet(cabinetWnd);
        SuppressStrayNavWorkers(cabinetWnd);
        return true;
    }
    HWND menuRebar=FindMenuBarRebar(cabinetWnd);
    HWND navRebar=FindNavbarRebar(cabinetWnd);
    if(!menuRebar||!navRebar)return false;

    int bandHeight = GetDesiredBandHeight();

    struct BandToMove{
        int navIdx;HWND child;HWND leftover;int width;
        bool isUpButton;bool isBreadcrumb;
    };
    std::vector<BandToMove> toMove;
    int navCnt=(int)SendMessage(navRebar,RB_GETBANDCOUNT,0,0);

    for(int i=0;i<navCnt;i++){
        REBARBANDINFO rbi={sizeof(rbi)};rbi.fMask=RBBIM_CHILD;
        if(!SendMessage(navRebar,RB_GETBANDINFO,i,(LPARAM)&rbi)||!rbi.hwndChild)continue;

        if(g_settings.moveSearchBand&&ContainsSearchBand(rbi.hwndChild)){
            RECT rc;GetWindowRect(rbi.hwndChild,&rc);int w=rc.right-rc.left;if(w<50)w=200;
            toMove.push_back({i,rbi.hwndChild,NULL,w,false,false});
        }else if(g_settings.moveBreadcrumb&&ContainsAddressBand(rbi.hwndChild)){
            HWND bc=FindBreadcrumbParent(rbi.hwndChild);
            if(bc){
                HWND bcToolbar=FindWindowEx(bc,NULL,L"ToolbarWindow32",NULL);
                if(bcToolbar){
                    RECT rc;GetWindowRect(bcToolbar,&rc);int w=rc.right-rc.left;if(w<50)w=250;
                    toMove.push_back({i,bcToolbar,bc,w,false,true});
                }
            }
        }
    }

    if(g_settings.moveUpButton) {
        HWND upButton = FindUpButton(cabinetWnd);
        if(upButton && IsWindow(upButton)) {
            for(int i = 0; i < navCnt; i++) {
                REBARBANDINFO rbi = {sizeof(rbi)};
                rbi.fMask = RBBIM_CHILD;
                if(!SendMessage(navRebar, RB_GETBANDINFO, i, (LPARAM)&rbi)) continue;
                WCHAR cls[256];
                if(rbi.hwndChild && GetClassName(rbi.hwndChild, cls, ARRAYSIZE(cls))) {
                    if(wcscmp(cls, L"UpBand") == 0) {
                        RECT rc;
                        GetWindowRect(upButton, &rc);
                        int w = rc.right - rc.left;
                        if(w < 20) w = 30;
                        toMove.push_back({i, upButton, rbi.hwndChild, w, true, false});
                        break;
                    }
                }
            }
        }
    }

    LONG_PTR st=GetWindowLongPtr(menuRebar,GWL_STYLE);
    st=(st&~RBS_FIXEDORDER)|RBS_VARHEIGHT;
    SetWindowLongPtr(menuRebar,GWL_STYLE,st);
    HWND navWorker=GetParent(navRebar);

    if(!toMove.empty()){
        DWORD gripperStyle = GetReferenceGripperStyle(menuRebar);

        for(auto it=toMove.rbegin();it!=toMove.rend();++it){
            SendMessage(navRebar,RB_DELETEBAND,it->navIdx,0);
            if(it->leftover&&IsWindow(it->leftover)){MarkNeutered(it->leftover);ShowWindow(it->leftover,SW_HIDE);}
        }

        for(auto&b:toMove){
            SetParent(b.child,menuRebar);

            WCHAR cls[256]=L"";
            if(b.isUpButton){
                wcsncpy_s(cls,ARRAYSIZE(cls),L"UpButtonToolbar",_TRUNCATE);
            }else if(b.isBreadcrumb){
                wcsncpy_s(cls,ARRAYSIZE(cls),L"BreadcrumbToolbar",_TRUNCATE);
            }else{
                GetClassName(b.child,cls,ARRAYSIZE(cls));
            }

            BandState bs;bool hasSaved=LoadBandState(cls,bs);
            UINT useCx=hasSaved?bs.cx:(UINT)b.width;

            if(b.isUpButton) {
                ResizeUpButtonToolbar(b.child);
                if(!hasSaved){
                    SIZE idealSz{};
                    if(SendMessage(b.child, TB_GETIDEALSIZE, FALSE, (LPARAM)&idealSz) && idealSz.cx > 0){
                        useCx = (UINT)idealSz.cx;
                    }
                }
            }else if(b.isBreadcrumb){
                SendMessage(b.child, TB_AUTOSIZE, 0, 0);
            }

            REBARBANDINFO rbi={sizeof(rbi)};
            rbi.fMask=RBBIM_STYLE|RBBIM_CHILD|RBBIM_CHILDSIZE|RBBIM_SIZE|RBBIM_IDEALSIZE;
            rbi.fStyle=gripperStyle;
            if(hasSaved&&bs.brk)rbi.fStyle|=RBBS_BREAK;
            rbi.hwndChild=b.child;
            rbi.cyMinChild = bandHeight;
            rbi.cyMaxChild = bandHeight;
            rbi.cyChild = bandHeight;
            rbi.cx = useCx;
            rbi.cxIdeal = useCx;
            rbi.cyIntegral=1;

            BOOL ins=(BOOL)SendMessage(menuRebar,RB_INSERTBAND,(WPARAM)-1,(LPARAM)&rbi);
            if(ins){
                int flags=CF_MOVED;
                if(b.isUpButton)flags|=CF_UPBUTTON;
                else if(b.isBreadcrumb)flags|=CF_BREADCRUMB;
                else flags|=CF_SEARCH;
                SetChildFlag(b.child,flags);
                if(b.isUpButton){
                    HookWindow(b.child, UpButton_SubclassProc);
                    ResizeUpButtonToolbar(b.child);
                }else if(b.isBreadcrumb){
                    HookWindow(b.child, BreadcrumbToolbar_SubclassProc);
                }
                ShowWindow(b.child,SW_SHOW);
            }else{
                SetParent(b.child,b.leftover?b.leftover:navRebar);
            }
        }

        {
            HWND mrParent=GetParent(menuRebar);
            if(mrParent) HookWindow(mrParent, MenuReBarParent_SubclassProc);
        }
        RegisterRebarCabinet(menuRebar,cabinetWnd);
        RegisterCabinetMenuRebar(cabinetWnd,menuRebar);
    }

    if(navWorker && IsWindow(navWorker)){
        HookWindow(navWorker, NavWorkerW_SubclassProc);
        ForceHideNavWorker(navWorker);
    }
    
    MarkMoved(cabinetWnd);

    if(!toMove.empty()){
        // This also reapplies the correct visual size for the moved
        // Up/Breadcrumb bands (see ReapplyBandVisualSizes call at the
        // end of ApplySavedLayout).
        ApplySavedLayout(menuRebar);
        SyncMovedBandGrippers(menuRebar);
    }

    ForceCabinetRelayout(cabinetWnd);
    return true;
}

void HookWindow(HWND hwnd, WindhawkUtils::WH_SUBCLASSPROC subclassProc){
    if(!hwnd||!IsWindow(hwnd))return;
    WindhawkUtils::SetWindowSubclassFromAnyThread(hwnd, subclassProc, 0);
    EnterCriticalSection(&g_mutex);
    g_subclassedWindows[hwnd] = subclassProc;
    LeaveCriticalSection(&g_mutex);
}

void ProcessWindow(HWND hwnd){
    if(!hwnd||!IsWindow(hwnd))return;
    WCHAR cls[256];if(!GetClassName(hwnd,cls,ARRAYSIZE(cls)))return;
    if(!wcscmp(cls,L"CabinetWClass")){HookWindow(hwnd,Cabinet_SubclassProc);return;}
    if(!wcscmp(cls,L"ShellTabWindowClass")){
        HWND p=GetParent(hwnd);WCHAR pc[64];
        if(p&&GetClassName(p,pc,ARRAYSIZE(pc))&&wcscmp(pc,L"CabinetWClass")==0)
            HookWindow(hwnd,ShellTab_SubclassProc);
        return;
    }
    if(!wcscmp(cls,L"WorkerW")){
        HWND p=GetParent(hwnd);WCHAR pc[64];
        if(p&&GetClassName(p,pc,ARRAYSIZE(pc))&&wcscmp(pc,L"CabinetWClass")==0){
            HookWindow(hwnd,NavWorkerW_SubclassProc);
            ForceHideNavWorker(hwnd);
            return;
        }
    }
    if(!wcscmp(cls,L"ReBarWindow32")){
        HookWindow(hwnd,ReBar_SubclassProc);
        HWND cab=GetCabinetAncestor(hwnd);
        if(cab&&IsRebarChildOfDirectWorkerW(hwnd,cab)){
            HWND worker=GetParent(hwnd);
            HookWindow(worker,NavWorkerW_SubclassProc);
            ForceHideNavWorker(worker);
        }
        return;
    }
    if(!wcscmp(cls,L"Address Band Root")){HookWindow(hwnd,AddressBandRoot_SubclassProc);return;}
}

BOOL CALLBACK EnumChildHook(HWND hwnd,LPARAM){ProcessWindow(hwnd);return TRUE;}

using CreateWindowExW_t=decltype(&CreateWindowExW);
CreateWindowExW_t CreateWindowExW_Original;
HWND WINAPI CreateWindowExW_Hook(DWORD s,LPCWSTR c,LPCWSTR wn,DWORD st,int X,int Y,int W,int H,HWND p,HMENU m,HINSTANCE h,LPVOID lp){
    HWND hwnd=CreateWindowExW_Original(s,c,wn,st,X,Y,W,H,p,m,h,lp);
    if(hwnd&&c&&!IS_INTRESOURCE(c)){
        ProcessWindow(hwnd);EnumChildWindows(hwnd,EnumChildHook,0);
        if(wcscmp(c,L"UniversalSearchBand")&&wcscmp(c,L"Address Band Root")&&wcscmp(c,L"Breadcrumb Parent")&&wcscmp(c,L"UpBand")){
            HWND cab=GetCabinetAncestor(hwnd);
            if(cab&&!WasAlreadyMoved(cab))PostMessage(cab,g_msgDoMove,0,0);
        }
    }
    return hwnd;
}

// ---------------------------------------------------------------------
// TrackPopupMenu / TrackPopupMenuEx hooks — inject toolbar toggle items
// ---------------------------------------------------------------------

using TrackPopupMenu_t = BOOL(WINAPI*)(HMENU, UINT, int, int, int, HWND, CONST RECT*);
TrackPopupMenu_t TrackPopupMenu_Original;

BOOL WINAPI TrackPopupMenu_Hook(HMENU hMenu, UINT uFlags, int x, int y, int nReserve, HWND hWnd, CONST RECT* prcRect) {
    if (!MenuContainsId(hMenu, LOCK_TOOLBARS_CMD_ID)) {
        return TrackPopupMenu_Original(hMenu, uFlags, x, y, nReserve, hWnd, prcRect);
    }

    AddOrUpdateToolbarMenuItems(hMenu);

    bool wantReturnCmd = (uFlags & TPM_RETURNCMD) != 0;
    UINT flags = uFlags | TPM_RETURNCMD;
    UINT cmd = (UINT)TrackPopupMenu_Original(hMenu, flags, x, y, nReserve, hWnd, prcRect);

    if (HandleToolbarMenuCommand(cmd)) {
        return wantReturnCmd ? 0 : TRUE;
    }
    if (!wantReturnCmd) {
        if (cmd != 0 && hWnd) PostMessage(hWnd, WM_COMMAND, MAKEWPARAM(LOWORD(cmd), 0), 0);
        return TRUE;
    }
    return (BOOL)cmd;
}

using TrackPopupMenuEx_t = BOOL(WINAPI*)(HMENU, UINT, int, int, HWND, LPTPMPARAMS);
TrackPopupMenuEx_t TrackPopupMenuEx_Original;

BOOL WINAPI TrackPopupMenuEx_Hook(HMENU hMenu, UINT uFlags, int x, int y, HWND hWnd, LPTPMPARAMS lptpm) {
    if (!MenuContainsId(hMenu, LOCK_TOOLBARS_CMD_ID)) {
        return TrackPopupMenuEx_Original(hMenu, uFlags, x, y, hWnd, lptpm);
    }

    AddOrUpdateToolbarMenuItems(hMenu);

    bool wantReturnCmd = (uFlags & TPM_RETURNCMD) != 0;
    UINT flags = uFlags | TPM_RETURNCMD;
    UINT cmd = (UINT)TrackPopupMenuEx_Original(hMenu, flags, x, y, hWnd, lptpm);

    if (HandleToolbarMenuCommand(cmd)) {
        return wantReturnCmd ? 0 : TRUE;
    }
    if (!wantReturnCmd) {
        if (cmd != 0 && hWnd) PostMessage(hWnd, WM_COMMAND, MAKEWPARAM(LOWORD(cmd), 0), 0);
        return TRUE;
    }
    return (BOOL)cmd;
}

BOOL Wh_ModInit(){
    Wh_Log(L"FlexibleExplorer init");
    LoadSettings();
    InitializeCriticalSection(&g_mutex);

    g_msgDoMove = RegisterWindowMessage(L"FlexibleExplorerToolbarsDeluxe_DoMove");
    if(!g_msgDoMove){ Wh_Log(L"RegisterWindowMessage failed"); return FALSE; }

    Wh_SetFunctionHook((void*)CreateWindowExW,(void*)CreateWindowExW_Hook,(void**)&CreateWindowExW_Original);
    Wh_SetFunctionHook(
        (void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtSetValueKey"),
        (void*)NtSetValueKey_Hook,
        (void**)&NtSetValueKey_Original);
    Wh_SetFunctionHook((void*)TrackPopupMenu,(void*)TrackPopupMenu_Hook,(void**)&TrackPopupMenu_Original);
    Wh_SetFunctionHook((void*)TrackPopupMenuEx,(void*)TrackPopupMenuEx_Hook,(void**)&TrackPopupMenuEx_Original);

    DWORD curPid = GetCurrentProcessId();
    for(HWND w=GetTopWindow(NULL);w;w=GetNextWindow(w,GW_HWNDNEXT)){
        DWORD pid=0;
        GetWindowThreadProcessId(w,&pid);
        if(pid!=curPid)continue;
        WCHAR cls[64];
        if(GetClassName(w,cls,ARRAYSIZE(cls))&&!wcscmp(cls,L"CabinetWClass")){
            ProcessWindow(w);EnumChildWindows(w,EnumChildHook,0);PostMessage(w,g_msgDoMove,0,0);
        }
    }
    return TRUE;
}

void Wh_ModUninit(){
    Wh_Log(L"FlexibleExplorer uninit");
    EnterCriticalSection(&g_mutex);
    std::vector<std::pair<HWND, WindhawkUtils::WH_SUBCLASSPROC>> windowsToClean(
        g_subclassedWindows.begin(), g_subclassedWindows.end());
    g_subclassedWindows.clear();
    LeaveCriticalSection(&g_mutex);
    for(auto& pair : windowsToClean){
        if(IsWindow(pair.first))
            WindhawkUtils::RemoveWindowSubclassFromAnyThread(pair.first, pair.second);
    }
    DeleteCriticalSection(&g_mutex);
}
