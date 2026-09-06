// ==WindhawkMod==
// @id classic-taskbar-buttons-lite
// @name Classic Taskbar 3D buttons Lite
// @description Lightweight mod, restoring the 3D buttons in classic theme
// @version 1.4
// @author Anixx
// @github https://github.com/Anixx
// @include explorer.exe
// @compilerOptions -lgdi32
// ==/WindhawkMod==

#include <windhawk_utils.h>
#ifdef _WIN64
#define CALCON __cdecl
#define SCALCON L"__cdecl"
#else
#define CALCON __thiscall
#define SCALCON L"__thiscall"
#endif
typedef struct tagBUTTONRENDERINFOSTATES { char data[12]; } BUTTONRENDERINFOSTATES, *PBUTTONRENDERINFOSTATES;

typedef void (* CTaskBtnGroup__DrawBar_t)(void *, HDC, void *, void *);
CTaskBtnGroup__DrawBar_t CTaskBtnGroup__DrawBar_orig;
void CALCON CTaskBtnGroup__DrawBar_hook(void *pThis, HDC hDC, void *pRenderInfo, PBUTTONRENDERINFOSTATES pRenderStates)
{
    LPRECT lprcDest = (LPRECT)((char *)pRenderInfo + 4);
    UINT uState = DFCS_BUTTONPUSH;
    if (pRenderStates->data[2]) uState |= DFCS_CHECKED;
    else if (pRenderStates->data[4]) uState |= DFCS_PUSHED;
    DrawFrameControl(hDC, lprcDest, DFC_BUTTON, uState);
    if (pRenderStates->data[2] || pRenderStates->data[4]) { lprcDest->top++; lprcDest->bottom++; lprcDest->left++; lprcDest->right++; }
}

typedef long (* CTaskBtnGroup_SetLocation_t)(void *, int, int, LPRECT);
CTaskBtnGroup_SetLocation_t CTaskBtnGroup_SetLocation_orig;

long __cdecl CTaskBtnGroup_SetLocation_hook(void *pThis, int i1, int i2, LPRECT lprc)
{
    if (!lprc) return CTaskBtnGroup_SetLocation_orig(pThis, i1, i2, lprc);
    int w = lprc->right - lprc->left;
    int h = lprc->bottom - lprc->top;
    if (w <= 2 || h <= 2) return CTaskBtnGroup_SetLocation_orig(pThis, i1, i2, lprc);

    // одинаково как на первом:
    // горизонтальный таскбар - широкий (>100) - режем right, любой ряд
    // вертикальный - узкий (<100) - не режем, только ровняем left чтобы не было лесенки
    if (w >= 100) {
        lprc->right -= 2;
    } else {
        // вертикальный второй - фиксируем left = 2, иначе каждая колонка съезжает
        lprc->left = 2;
        lprc->right = lprc->left + w;
    }

    return CTaskBtnGroup_SetLocation_orig(pThis, i1, i2, lprc);
}

BOOL Wh_ModInit(void)
{
    HMODULE hExplorer = GetModuleHandleW(NULL);
    WindhawkUtils::SYMBOL_HOOK hooks[] = {
        { { L"private: void " SCALCON L" CTaskBtnGroup::_DrawBar(struct HDC__ *,struct BUTTONRENDERINFO const &,struct BUTTONRENDERINFOSTATES const &)" }, (void **)&CTaskBtnGroup__DrawBar_orig, (void *)CTaskBtnGroup__DrawBar_hook, FALSE },
        { { L"public: virtual long __cdecl CTaskBtnGroup::SetLocation(int,int,struct tagRECT const *)" }, (void **)&CTaskBtnGroup_SetLocation_orig, (void*)CTaskBtnGroup_SetLocation_hook, FALSE }
    };
    return WindhawkUtils::HookSymbols(hExplorer, hooks, ARRAYSIZE(hooks));
}
