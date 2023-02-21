/*
 * IMM32 library
 *
 * Copyright 1998 Patrik Stridvall
 * Copyright 2002, 2003, 2007 CodeWeavers, Aric Stewart
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define COBJMACROS

#include <stdarg.h>
#include <stdio.h>

#include "initguid.h"
#include "objbase.h"
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "ntuser.h"
#include "winerror.h"
#include "wine/debug.h"
#include "imm.h"
#include "immdev.h"
#include "winnls.h"
#include "winreg.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(imm);

#define IMM_INIT_MAGIC 0x19650412
BOOL WINAPI User32InitializeImmEntryTable(DWORD);

/* MSIME messages */
static UINT WM_MSIME_SERVICE;
static UINT WM_MSIME_RECONVERTOPTIONS;
static UINT WM_MSIME_MOUSE;
static UINT WM_MSIME_RECONVERTREQUEST;
static UINT WM_MSIME_RECONVERT;
static UINT WM_MSIME_QUERYPOSITION;
static UINT WM_MSIME_DOCUMENTFEED;

struct ime
{
    struct list entry;
    HKL         hkl;
    HMODULE     hIME;
    IMEINFO     imeInfo;
    WCHAR       imeClassName[17]; /* 16 character max */
    ULONG       uSelected;
    HWND        UIWnd;

    /* Function Pointers */
    BOOL (WINAPI *pImeInquire)(IMEINFO *, void *, DWORD);
    BOOL (WINAPI *pImeConfigure)(HKL, HWND, DWORD, void *);
    BOOL (WINAPI *pImeDestroy)(UINT);
    LRESULT (WINAPI *pImeEscape)(HIMC, UINT, void *);
    BOOL (WINAPI *pImeSelect)(HIMC, BOOL);
    BOOL (WINAPI *pImeSetActiveContext)(HIMC, BOOL);
    UINT (WINAPI *pImeToAsciiEx)(UINT, UINT, const BYTE *, TRANSMSGLIST *, UINT, HIMC);
    BOOL (WINAPI *pNotifyIME)(HIMC, DWORD, DWORD, DWORD);
    BOOL (WINAPI *pImeRegisterWord)(const void/*TCHAR*/*, DWORD, const void/*TCHAR*/*);
    BOOL (WINAPI *pImeUnregisterWord)(const void/*TCHAR*/*, DWORD, const void/*TCHAR*/*);
    UINT (WINAPI *pImeEnumRegisterWord)(void */*REGISTERWORDENUMPROCW*/, const void/*TCHAR*/*, DWORD, const void/*TCHAR*/*, void *);
    BOOL (WINAPI *pImeSetCompositionString)(HIMC, DWORD, const void/*TCHAR*/*, DWORD, const void/*TCHAR*/*, DWORD);
    DWORD (WINAPI *pImeConversionList)(HIMC, const void/*TCHAR*/*, CANDIDATELIST*, DWORD, UINT);
    UINT (WINAPI *pImeGetRegisterWordStyle)(UINT, void/*STYLEBUFW*/*);
    BOOL (WINAPI *pImeProcessKey)(HIMC, UINT, LPARAM, const BYTE*);
    DWORD (WINAPI *pImeGetImeMenuItems)(HIMC, DWORD, DWORD, void/*IMEMENUITEMINFOW*/*, void/*IMEMENUITEMINFOW*/*, DWORD);
};

static HRESULT (WINAPI *pCoRevokeInitializeSpy)(ULARGE_INTEGER cookie);
static void (WINAPI *pCoUninitialize)(void);

typedef struct tagInputContextData
{
        HIMC            handle;
        DWORD           dwLock;
        INPUTCONTEXT    IMC;
        DWORD           threadID;

        struct ime     *immKbd;
        UINT            lastVK;
        BOOL            threadDefault;
} InputContextData;

#define WINE_IMC_VALID_MAGIC 0x56434D49

struct coinit_spy
{
    IInitializeSpy IInitializeSpy_iface;
    LONG ref;
    ULARGE_INTEGER cookie;
    enum
    {
        IMM_APT_INIT = 0x1,
        IMM_APT_CREATED = 0x2,
        IMM_APT_CAN_FREE = 0x4,
        IMM_APT_BROKEN = 0x8
    } apt_flags;
};

static struct list ImmHklList = LIST_INIT(ImmHklList);

static const WCHAR szImeRegFmt[] = L"System\\CurrentControlSet\\Control\\Keyboard Layouts\\%08lx";

static inline BOOL is_himc_ime_unicode(const InputContextData *data)
{
    return !!(data->immKbd->imeInfo.fdwProperty & IME_PROP_UNICODE);
}

static inline BOOL is_kbd_ime_unicode( const struct ime *hkl )
{
    return !!(hkl->imeInfo.fdwProperty & IME_PROP_UNICODE);
}

static BOOL IMM_DestroyContext(HIMC hIMC);
static InputContextData* get_imc_data(HIMC hIMC);

static inline WCHAR *strdupAtoW( const char *str )
{
    WCHAR *ret = NULL;
    if (str)
    {
        DWORD len = MultiByteToWideChar( CP_ACP, 0, str, -1, NULL, 0 );
        if ((ret = HeapAlloc( GetProcessHeap(), 0, len * sizeof(WCHAR) )))
            MultiByteToWideChar( CP_ACP, 0, str, -1, ret, len );
    }
    return ret;
}

static inline CHAR *strdupWtoA( const WCHAR *str )
{
    CHAR *ret = NULL;
    if (str)
    {
        DWORD len = WideCharToMultiByte( CP_ACP, 0, str, -1, NULL, 0, NULL, NULL );
        if ((ret = HeapAlloc( GetProcessHeap(), 0, len )))
            WideCharToMultiByte( CP_ACP, 0, str, -1, ret, len, NULL, NULL );
    }
    return ret;
}

static DWORD convert_candidatelist_WtoA(
        LPCANDIDATELIST lpSrc, LPCANDIDATELIST lpDst, DWORD dwBufLen)
{
    DWORD ret, i, len;

    ret = FIELD_OFFSET( CANDIDATELIST, dwOffset[lpSrc->dwCount] );
    if ( lpDst && dwBufLen > 0 )
    {
        *lpDst = *lpSrc;
        lpDst->dwOffset[0] = ret;
    }

    for ( i = 0; i < lpSrc->dwCount; i++)
    {
        LPBYTE src = (LPBYTE)lpSrc + lpSrc->dwOffset[i];

        if ( lpDst && dwBufLen > 0 )
        {
            LPBYTE dest = (LPBYTE)lpDst + lpDst->dwOffset[i];

            len = WideCharToMultiByte(CP_ACP, 0, (LPCWSTR)src, -1,
                                      (LPSTR)dest, dwBufLen, NULL, NULL);

            if ( i + 1 < lpSrc->dwCount )
                lpDst->dwOffset[i+1] = lpDst->dwOffset[i] + len * sizeof(char);
            dwBufLen -= len * sizeof(char);
        }
        else
            len = WideCharToMultiByte(CP_ACP, 0, (LPCWSTR)src, -1, NULL, 0, NULL, NULL);

        ret += len * sizeof(char);
    }

    if ( lpDst )
        lpDst->dwSize = ret;

    return ret;
}

static DWORD convert_candidatelist_AtoW(
        LPCANDIDATELIST lpSrc, LPCANDIDATELIST lpDst, DWORD dwBufLen)
{
    DWORD ret, i, len;

    ret = FIELD_OFFSET( CANDIDATELIST, dwOffset[lpSrc->dwCount] );
    if ( lpDst && dwBufLen > 0 )
    {
        *lpDst = *lpSrc;
        lpDst->dwOffset[0] = ret;
    }

    for ( i = 0; i < lpSrc->dwCount; i++)
    {
        LPBYTE src = (LPBYTE)lpSrc + lpSrc->dwOffset[i];

        if ( lpDst && dwBufLen > 0 )
        {
            LPBYTE dest = (LPBYTE)lpDst + lpDst->dwOffset[i];

            len = MultiByteToWideChar(CP_ACP, 0, (LPCSTR)src, -1,
                                      (LPWSTR)dest, dwBufLen);

            if ( i + 1 < lpSrc->dwCount )
                lpDst->dwOffset[i+1] = lpDst->dwOffset[i] + len * sizeof(WCHAR);
            dwBufLen -= len * sizeof(WCHAR);
        }
        else
            len = MultiByteToWideChar(CP_ACP, 0, (LPCSTR)src, -1, NULL, 0);

        ret += len * sizeof(WCHAR);
    }

    if ( lpDst )
        lpDst->dwSize = ret;

    return ret;
}

static struct coinit_spy *get_thread_coinit_spy(void)
{
    return (struct coinit_spy *)(UINT_PTR)NtUserGetThreadInfo()->client_imm;
}

static void imm_couninit_thread(BOOL cleanup)
{
    struct coinit_spy *spy;

    TRACE("implicit COM deinitialization\n");

    if (!(spy = get_thread_coinit_spy()) || (spy->apt_flags & IMM_APT_BROKEN))
        return;

    if (cleanup && spy->cookie.QuadPart)
    {
        pCoRevokeInitializeSpy(spy->cookie);
        spy->cookie.QuadPart = 0;
    }

    if (!(spy->apt_flags & IMM_APT_INIT))
        return;
    spy->apt_flags &= ~IMM_APT_INIT;

    if (spy->apt_flags & IMM_APT_CREATED)
    {
        spy->apt_flags &= ~IMM_APT_CREATED;
        if (spy->apt_flags & IMM_APT_CAN_FREE)
            pCoUninitialize();
    }
    if (cleanup)
        spy->apt_flags = 0;
}

static inline struct coinit_spy *impl_from_IInitializeSpy(IInitializeSpy *iface)
{
    return CONTAINING_RECORD(iface, struct coinit_spy, IInitializeSpy_iface);
}

static HRESULT WINAPI InitializeSpy_QueryInterface(IInitializeSpy *iface, REFIID riid, void **obj)
{
    if (IsEqualIID(&IID_IInitializeSpy, riid) ||
            IsEqualIID(&IID_IUnknown, riid))
    {
        *obj = iface;
        IInitializeSpy_AddRef(iface);
        return S_OK;
    }

    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI InitializeSpy_AddRef(IInitializeSpy *iface)
{
    struct coinit_spy *spy = impl_from_IInitializeSpy(iface);
    return InterlockedIncrement(&spy->ref);
}

static ULONG WINAPI InitializeSpy_Release(IInitializeSpy *iface)
{
    struct coinit_spy *spy = impl_from_IInitializeSpy(iface);
    LONG ref = InterlockedDecrement(&spy->ref);
    if (!ref)
    {
        HeapFree(GetProcessHeap(), 0, spy);
        NtUserGetThreadInfo()->client_imm = 0;
    }
    return ref;
}

static HRESULT WINAPI InitializeSpy_PreInitialize(IInitializeSpy *iface,
        DWORD coinit, DWORD refs)
{
    struct coinit_spy *spy = impl_from_IInitializeSpy(iface);

    if ((spy->apt_flags & IMM_APT_CREATED) &&
            !(coinit & COINIT_APARTMENTTHREADED) && refs == 1)
    {
        imm_couninit_thread(TRUE);
        spy->apt_flags |= IMM_APT_BROKEN;
    }
    return S_OK;
}

static HRESULT WINAPI InitializeSpy_PostInitialize(IInitializeSpy *iface,
        HRESULT hr, DWORD coinit, DWORD refs)
{
    struct coinit_spy *spy = impl_from_IInitializeSpy(iface);

    if ((spy->apt_flags & IMM_APT_CREATED) && hr == S_FALSE && refs == 2)
        hr = S_OK;
    if (SUCCEEDED(hr))
        spy->apt_flags |= IMM_APT_CAN_FREE;
    return hr;
}

static HRESULT WINAPI InitializeSpy_PreUninitialize(IInitializeSpy *iface, DWORD refs)
{
    return S_OK;
}

static HRESULT WINAPI InitializeSpy_PostUninitialize(IInitializeSpy *iface, DWORD refs)
{
    struct coinit_spy *spy = impl_from_IInitializeSpy(iface);

    TRACE("%lu %p\n", refs, ImmGetDefaultIMEWnd(0));

    if (refs == 1 && !ImmGetDefaultIMEWnd(0))
        imm_couninit_thread(FALSE);
    else if (!refs)
        spy->apt_flags &= ~IMM_APT_CAN_FREE;
    return S_OK;
}

static const IInitializeSpyVtbl InitializeSpyVtbl =
{
    InitializeSpy_QueryInterface,
    InitializeSpy_AddRef,
    InitializeSpy_Release,
    InitializeSpy_PreInitialize,
    InitializeSpy_PostInitialize,
    InitializeSpy_PreUninitialize,
    InitializeSpy_PostUninitialize,
};

static BOOL WINAPI init_ole32_funcs( INIT_ONCE *once, void *param, void **context )
{
    HMODULE module_ole32 = GetModuleHandleA("ole32");
    pCoRevokeInitializeSpy = (void*)GetProcAddress(module_ole32, "CoRevokeInitializeSpy");
    pCoUninitialize = (void*)GetProcAddress(module_ole32, "CoUninitialize");
    return TRUE;
}

static void imm_coinit_thread(void)
{
    struct coinit_spy *spy;
    HRESULT hr;
    static INIT_ONCE init_ole32_once = INIT_ONCE_STATIC_INIT;

    TRACE("implicit COM initialization\n");

    if (!(spy = get_thread_coinit_spy()))
    {
        if (!(spy = HeapAlloc(GetProcessHeap(), 0, sizeof(*spy)))) return;
        spy->IInitializeSpy_iface.lpVtbl = &InitializeSpyVtbl;
        spy->ref = 1;
        spy->cookie.QuadPart = 0;
        spy->apt_flags = 0;
        NtUserGetThreadInfo()->client_imm = (UINT_PTR)spy;

    }

    if (spy->apt_flags & (IMM_APT_INIT | IMM_APT_BROKEN))
        return;
    spy->apt_flags |= IMM_APT_INIT;

    if(!spy->cookie.QuadPart)
    {
        hr = CoRegisterInitializeSpy(&spy->IInitializeSpy_iface, &spy->cookie);
        if (FAILED(hr))
            return;
    }

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr))
        spy->apt_flags |= IMM_APT_CREATED;

    InitOnceExecuteOnce(&init_ole32_once, init_ole32_funcs, NULL, NULL);
}

static BOOL IMM_IsDefaultContext(HIMC imc)
{
    InputContextData *data = get_imc_data(imc);

    if (!data)
        return FALSE;

    return data->threadDefault;
}

static InputContextData *query_imc_data(HIMC handle)
{
    InputContextData *ret;

    if (!handle) return NULL;
    ret = (void *)NtUserQueryInputContext(handle, NtUserInputContextClientPtr);
    return ret && ret->handle == handle ? ret : NULL;
}

static BOOL free_input_context_data(HIMC hIMC)
{
    InputContextData *data = query_imc_data(hIMC);

    if (!data)
        return FALSE;

    TRACE("Destroying %p\n", hIMC);

    data->immKbd->uSelected--;
    data->immKbd->pImeSelect(hIMC, FALSE);
    SendMessageW(data->IMC.hWnd, WM_IME_SELECT, FALSE, (LPARAM)data->immKbd);

    ImmDestroyIMCC(data->IMC.hCompStr);
    ImmDestroyIMCC(data->IMC.hCandInfo);
    ImmDestroyIMCC(data->IMC.hGuideLine);
    ImmDestroyIMCC(data->IMC.hPrivate);
    ImmDestroyIMCC(data->IMC.hMsgBuf);

    HeapFree(GetProcessHeap(), 0, data);

    return TRUE;
}

static void IMM_FreeThreadData(void)
{
    struct coinit_spy *spy;

    free_input_context_data(UlongToHandle(NtUserGetThreadInfo()->default_imc));
    if ((spy = get_thread_coinit_spy()))
        IInitializeSpy_Release(&spy->IInitializeSpy_iface);
}

static HMODULE load_graphics_driver(void)
{
    static const WCHAR key_pathW[] = L"System\\CurrentControlSet\\Control\\Video\\{";
    static const WCHAR displayW[] = L"}\\0000";

    HMODULE ret = 0;
    HKEY hkey;
    DWORD size;
    WCHAR path[MAX_PATH];
    WCHAR key[ARRAY_SIZE( key_pathW ) + ARRAY_SIZE( displayW ) + 40];
    UINT guid_atom = HandleToULong( GetPropW( GetDesktopWindow(), L"__wine_display_device_guid" ));

    if (!guid_atom) return 0;
    memcpy( key, key_pathW, sizeof(key_pathW) );
    if (!GlobalGetAtomNameW( guid_atom, key + lstrlenW(key), 40 )) return 0;
    lstrcatW( key, displayW );
    if (RegOpenKeyW( HKEY_LOCAL_MACHINE, key, &hkey )) return 0;
    size = sizeof(path);
    if (!RegQueryValueExW( hkey, L"GraphicsDriver", NULL, NULL, (BYTE *)path, &size ))
        ret = LoadLibraryW( path );
    RegCloseKey( hkey );
    TRACE( "%s %p\n", debugstr_w(path), ret );
    return ret;
}

BOOL WINAPI ImmFreeLayout( HKL hkl )
{
    FIXME( "hkl %p stub!\n", hkl );
    return FALSE;
}

BOOL WINAPI ImmLoadIME( HKL hkl )
{
    FIXME( "hkl %p stub!\n", hkl );
    return FALSE;
}

/* struct ime loading and freeing */
#define LOAD_FUNCPTR(f) if((ptr->p##f = (LPVOID)GetProcAddress(ptr->hIME, #f)) == NULL){WARN("Can't find function %s in ime\n", #f);}
static struct ime *IMM_GetImmHkl( HKL hkl )
{
    struct ime *ptr;
    WCHAR filename[MAX_PATH];

    TRACE("Seeking ime for keyboard %p\n",hkl);

    LIST_FOR_EACH_ENTRY( ptr, &ImmHklList, struct ime, entry )
    {
        if (ptr->hkl == hkl)
            return ptr;
    }
    /* not found... create it */

    ptr = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(struct ime) );

    ptr->hkl = hkl;
    if (ImmGetIMEFileNameW(hkl, filename, MAX_PATH)) ptr->hIME = LoadLibraryW(filename);
    if (!ptr->hIME) ptr->hIME = load_graphics_driver();
    if (ptr->hIME)
    {
        LOAD_FUNCPTR(ImeInquire);
        if (!ptr->pImeInquire || !ptr->pImeInquire(&ptr->imeInfo, ptr->imeClassName, 0))
        {
            FreeLibrary(ptr->hIME);
            ptr->hIME = NULL;
        }
        else
        {
            LOAD_FUNCPTR(ImeDestroy);
            LOAD_FUNCPTR(ImeSelect);
            if (!ptr->pImeSelect || !ptr->pImeDestroy)
            {
                FreeLibrary(ptr->hIME);
                ptr->hIME = NULL;
            }
            else
            {
                LOAD_FUNCPTR(ImeConfigure);
                LOAD_FUNCPTR(ImeEscape);
                LOAD_FUNCPTR(ImeSetActiveContext);
                LOAD_FUNCPTR(ImeToAsciiEx);
                LOAD_FUNCPTR(NotifyIME);
                LOAD_FUNCPTR(ImeRegisterWord);
                LOAD_FUNCPTR(ImeUnregisterWord);
                LOAD_FUNCPTR(ImeEnumRegisterWord);
                LOAD_FUNCPTR(ImeSetCompositionString);
                LOAD_FUNCPTR(ImeConversionList);
                LOAD_FUNCPTR(ImeProcessKey);
                LOAD_FUNCPTR(ImeGetRegisterWordStyle);
                LOAD_FUNCPTR(ImeGetImeMenuItems);
                /* make sure our classname is WCHAR */
                if (!is_kbd_ime_unicode(ptr))
                {
                    WCHAR bufW[17];
                    MultiByteToWideChar(CP_ACP, 0, (LPSTR)ptr->imeClassName,
                                        -1, bufW, 17);
                    lstrcpyW(ptr->imeClassName, bufW);
                }
            }
        }
    }
    list_add_head(&ImmHklList,&ptr->entry);

    return ptr;
}
#undef LOAD_FUNCPTR


static void IMM_FreeAllImmHkl(void)
{
    struct ime *ptr, *cursor2;

    LIST_FOR_EACH_ENTRY_SAFE( ptr, cursor2, &ImmHklList, struct ime, entry )
    {
        list_remove(&ptr->entry);
        if (ptr->hIME)
        {
            ptr->pImeDestroy(1);
            FreeLibrary(ptr->hIME);
        }
        if (ptr->UIWnd)
            DestroyWindow(ptr->UIWnd);
        HeapFree(GetProcessHeap(),0,ptr);
    }
}

BOOL WINAPI DllMain(HINSTANCE hInstDLL, DWORD fdwReason, LPVOID lpReserved)
{
    TRACE("%p, %lx, %p\n",hInstDLL,fdwReason,lpReserved);
    switch (fdwReason)
    {
        case DLL_PROCESS_ATTACH:
            if (!User32InitializeImmEntryTable(IMM_INIT_MAGIC))
            {
                return FALSE;
            }
            break;
        case DLL_THREAD_ATTACH:
            break;
        case DLL_THREAD_DETACH:
            IMM_FreeThreadData();
            break;
        case DLL_PROCESS_DETACH:
            if (lpReserved) break;
            IMM_FreeThreadData();
            IMM_FreeAllImmHkl();
            break;
    }
    return TRUE;
}

/* for posting messages as the IME */
static void ImmInternalPostIMEMessage(InputContextData *data, UINT msg, WPARAM wParam, LPARAM lParam)
{
    HWND target = GetFocus();
    if (!target)
       PostMessageW(data->IMC.hWnd,msg,wParam,lParam);
    else
       PostMessageW(target, msg, wParam, lParam);
}

/* for sending messages as the IME */
static void ImmInternalSendIMEMessage(InputContextData *data, UINT msg, WPARAM wParam, LPARAM lParam)
{
    HWND target = GetFocus();
    if (!target)
       SendMessageW(data->IMC.hWnd,msg,wParam,lParam);
    else
       SendMessageW(target, msg, wParam, lParam);
}

static LRESULT ImmInternalSendIMENotify(InputContextData *data, WPARAM notify, LPARAM lParam)
{
    HWND target;

    target = data->IMC.hWnd;
    if (!target) target = GetFocus();

    if (target)
       return SendMessageW(target, WM_IME_NOTIFY, notify, lParam);

    return 0;
}

static HIMCC ImmCreateBlankCompStr(void)
{
    HIMCC rc;
    LPCOMPOSITIONSTRING ptr;
    rc = ImmCreateIMCC(sizeof(COMPOSITIONSTRING));
    ptr = ImmLockIMCC(rc);
    memset(ptr,0,sizeof(COMPOSITIONSTRING));
    ptr->dwSize = sizeof(COMPOSITIONSTRING);
    ImmUnlockIMCC(rc);
    return rc;
}

static BOOL IMM_IsCrossThreadAccess(HWND hWnd,  HIMC hIMC)
{
    InputContextData *data;

    if (hWnd)
    {
        DWORD thread = GetWindowThreadProcessId(hWnd, NULL);
        if (thread != GetCurrentThreadId()) return TRUE;
    }
    data = get_imc_data(hIMC);
    if (data && data->threadID != GetCurrentThreadId())
        return TRUE;

    return FALSE;
}

/***********************************************************************
 *		ImmSetActiveContext (IMM32.@)
 */
BOOL WINAPI ImmSetActiveContext(HWND hwnd, HIMC himc, BOOL activate)
{
    InputContextData *data = get_imc_data(himc);

    TRACE("(%p, %p, %x)\n", hwnd, himc, activate);

    if (himc && !data && activate)
        return FALSE;

    imm_coinit_thread();

    if (data)
    {
        data->IMC.hWnd = activate ? hwnd : NULL;

        if (data->immKbd->hIME && data->immKbd->pImeSetActiveContext)
            data->immKbd->pImeSetActiveContext(himc, activate);
    }

    if (IsWindow(hwnd))
    {
        SendMessageW(hwnd, WM_IME_SETCONTEXT, activate, ISC_SHOWUIALL);
        /* TODO: send WM_IME_NOTIFY */
    }
    SetLastError(0);
    return TRUE;
}

/***********************************************************************
 *		ImmAssociateContext (IMM32.@)
 */
HIMC WINAPI ImmAssociateContext(HWND hwnd, HIMC imc)
{
    HIMC old;
    UINT ret;

    TRACE("(%p, %p):\n", hwnd, imc);

    old = NtUserGetWindowInputContext(hwnd);
    ret = NtUserAssociateInputContext(hwnd, imc, 0);
    if (ret == AICR_FOCUS_CHANGED)
    {
        ImmSetActiveContext(hwnd, old, FALSE);
        ImmSetActiveContext(hwnd, imc, TRUE);
    }
    return ret == AICR_FAILED ? 0 : old;
}


/*
 * Helper function for ImmAssociateContextEx
 */
static BOOL CALLBACK _ImmAssociateContextExEnumProc(HWND hwnd, LPARAM lParam)
{
    HIMC hImc = (HIMC)lParam;
    ImmAssociateContext(hwnd,hImc);
    return TRUE;
}

/***********************************************************************
 *              ImmAssociateContextEx (IMM32.@)
 */
BOOL WINAPI ImmAssociateContextEx(HWND hwnd, HIMC imc, DWORD flags)
{
    HIMC old;
    UINT ret;

    TRACE("(%p, %p, 0x%lx):\n", hwnd, imc, flags);

    if (!hwnd)
        return FALSE;

    if (flags == IACE_CHILDREN)
    {
        EnumChildWindows(hwnd, _ImmAssociateContextExEnumProc, (LPARAM)imc);
        return TRUE;
    }

    old = NtUserGetWindowInputContext(hwnd);
    ret = NtUserAssociateInputContext(hwnd, imc, flags);
    if (ret == AICR_FOCUS_CHANGED)
    {
        ImmSetActiveContext(hwnd, old, FALSE);
        ImmSetActiveContext(hwnd, imc, TRUE);
    }
    return ret != AICR_FAILED;
}

/***********************************************************************
 *		ImmConfigureIMEA (IMM32.@)
 */
BOOL WINAPI ImmConfigureIMEA( HKL hkl, HWND hwnd, DWORD mode, void *data )
{
    struct ime *ime = IMM_GetImmHkl( hkl );
    BOOL ret;

    TRACE( "hkl %p, hwnd %p, mode %lu, data %p.\n", hkl, hwnd, mode, data );

    if (mode == IME_CONFIG_REGISTERWORD && !data) return FALSE;
    if (!ime->hIME || !ime->pImeConfigure) return FALSE;

    if (mode != IME_CONFIG_REGISTERWORD || !is_kbd_ime_unicode( ime ))
        ret = ime->pImeConfigure( hkl, hwnd, mode, data );
    else
    {
        REGISTERWORDA *wordA = data;
        REGISTERWORDW wordW;
        wordW.lpWord = strdupAtoW( wordA->lpWord );
        wordW.lpReading = strdupAtoW( wordA->lpReading );
        ret = ime->pImeConfigure( hkl, hwnd, mode, &wordW );
        HeapFree( GetProcessHeap(), 0, wordW.lpReading );
        HeapFree( GetProcessHeap(), 0, wordW.lpWord );
    }

    return ret;
}

/***********************************************************************
 *		ImmConfigureIMEW (IMM32.@)
 */
BOOL WINAPI ImmConfigureIMEW( HKL hkl, HWND hwnd, DWORD mode, void *data )
{
    struct ime *ime = IMM_GetImmHkl( hkl );
    BOOL ret;

    TRACE( "hkl %p, hwnd %p, mode %lu, data %p.\n", hkl, hwnd, mode, data );

    if (mode == IME_CONFIG_REGISTERWORD && !data) return FALSE;
    if (!ime->hIME || !ime->pImeConfigure) return FALSE;

    if (mode != IME_CONFIG_REGISTERWORD || is_kbd_ime_unicode( ime ))
        ret = ime->pImeConfigure( hkl, hwnd, mode, data );
    else
    {
        REGISTERWORDW *wordW = data;
        REGISTERWORDA wordA;
        wordA.lpWord = strdupWtoA( wordW->lpWord );
        wordA.lpReading = strdupWtoA( wordW->lpReading );
        ret = ime->pImeConfigure( hkl, hwnd, mode, &wordA );
        HeapFree( GetProcessHeap(), 0, wordA.lpReading );
        HeapFree( GetProcessHeap(), 0, wordA.lpWord );
    }

    return ret;
}

static InputContextData *create_input_context(HIMC default_imc)
{
    InputContextData *new_context;
    LPGUIDELINE gl;
    LPCANDIDATEINFO ci;
    int i;

    new_context = HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(InputContextData));

    /* Load the IME */
    new_context->threadDefault = !!default_imc;
    new_context->immKbd = IMM_GetImmHkl(GetKeyboardLayout(0));

    if (!new_context->immKbd->hIME)
    {
        TRACE("IME dll could not be loaded\n");
        HeapFree(GetProcessHeap(),0,new_context);
        return 0;
    }

    /* the HIMCCs are never NULL */
    new_context->IMC.hCompStr = ImmCreateBlankCompStr();
    new_context->IMC.hMsgBuf = ImmCreateIMCC(0);
    new_context->IMC.hCandInfo = ImmCreateIMCC(sizeof(CANDIDATEINFO));
    ci = ImmLockIMCC(new_context->IMC.hCandInfo);
    memset(ci,0,sizeof(CANDIDATEINFO));
    ci->dwSize = sizeof(CANDIDATEINFO);
    ImmUnlockIMCC(new_context->IMC.hCandInfo);
    new_context->IMC.hGuideLine = ImmCreateIMCC(sizeof(GUIDELINE));
    gl = ImmLockIMCC(new_context->IMC.hGuideLine);
    memset(gl,0,sizeof(GUIDELINE));
    gl->dwSize = sizeof(GUIDELINE);
    ImmUnlockIMCC(new_context->IMC.hGuideLine);

    for (i = 0; i < ARRAY_SIZE(new_context->IMC.cfCandForm); i++)
        new_context->IMC.cfCandForm[i].dwIndex = ~0u;

    /* Initialize the IME Private */
    new_context->IMC.hPrivate = ImmCreateIMCC(new_context->immKbd->imeInfo.dwPrivateDataSize);

    new_context->IMC.fdwConversion = new_context->immKbd->imeInfo.fdwConversionCaps;
    new_context->IMC.fdwSentence = new_context->immKbd->imeInfo.fdwSentenceCaps;

    if (!default_imc)
        new_context->handle = NtUserCreateInputContext((UINT_PTR)new_context);
    else if (NtUserUpdateInputContext(default_imc, NtUserInputContextClientPtr, (UINT_PTR)new_context))
        new_context->handle = default_imc;
    if (!new_context->handle)
    {
        free_input_context_data(new_context);
        return 0;
    }

    if (!new_context->immKbd->pImeSelect(new_context->handle, TRUE))
    {
        TRACE("Selection of IME failed\n");
        IMM_DestroyContext(new_context);
        return 0;
    }
    new_context->threadID = GetCurrentThreadId();
    SendMessageW(GetFocus(), WM_IME_SELECT, TRUE, (LPARAM)new_context->immKbd);

    new_context->immKbd->uSelected++;
    TRACE("Created context %p\n", new_context);
    return new_context;
}

static InputContextData* get_imc_data(HIMC handle)
{
    InputContextData *ret;

    if ((ret = query_imc_data(handle)) || !handle) return ret;
    return create_input_context(handle);
}

/***********************************************************************
 *		ImmCreateContext (IMM32.@)
 */
HIMC WINAPI ImmCreateContext(void)
{
    InputContextData *new_context;

    if (!(new_context = create_input_context(0))) return 0;
    return new_context->handle;
}

static BOOL IMM_DestroyContext(HIMC hIMC)
{
    if (!free_input_context_data(hIMC)) return FALSE;
    NtUserDestroyInputContext(hIMC);
    return TRUE;
}

/***********************************************************************
 *		ImmDestroyContext (IMM32.@)
 */
BOOL WINAPI ImmDestroyContext(HIMC hIMC)
{
    if (!IMM_IsDefaultContext(hIMC) && !IMM_IsCrossThreadAccess(NULL, hIMC))
        return IMM_DestroyContext(hIMC);
    else
        return FALSE;
}

/***********************************************************************
 *		ImmEnumRegisterWordA (IMM32.@)
 */
UINT WINAPI ImmEnumRegisterWordA( HKL hkl, REGISTERWORDENUMPROCA procA, const char *readingA,
                                  DWORD style, const char *stringA, void *user )
{
    struct ime *ime = IMM_GetImmHkl( hkl );
    UINT ret;

    TRACE( "hkl %p, procA %p, readingA %s, style %lu, stringA %s, user %p.\n", hkl, procA,
           debugstr_a(readingA), style, debugstr_a(stringA), user );

    if (!ime->hIME || !ime->pImeEnumRegisterWord) return 0;

    if (!is_kbd_ime_unicode( ime ))
        ret = ime->pImeEnumRegisterWord( procA, readingA, style, stringA, user );
    else
    {
        WCHAR *readingW = strdupAtoW( readingA ), *stringW = strdupAtoW( stringA );
        ret = ime->pImeEnumRegisterWord( procA, readingW, style, stringW, user );
        HeapFree( GetProcessHeap(), 0, readingW );
        HeapFree( GetProcessHeap(), 0, stringW );
    }

    return ret;
}

/***********************************************************************
 *		ImmEnumRegisterWordW (IMM32.@)
 */
UINT WINAPI ImmEnumRegisterWordW( HKL hkl, REGISTERWORDENUMPROCW procW, const WCHAR *readingW,
                                  DWORD style, const WCHAR *stringW, void *user )
{
    struct ime *ime = IMM_GetImmHkl( hkl );
    UINT ret;

    TRACE( "hkl %p, procW %p, readingW %s, style %lu, stringW %s, user %p.\n", hkl, procW,
           debugstr_w(readingW), style, debugstr_w(stringW), user );

    if (!ime->hIME || !ime->pImeEnumRegisterWord) return 0;

    if (is_kbd_ime_unicode( ime ))
        ret = ime->pImeEnumRegisterWord( procW, readingW, style, stringW, user );
    else
    {
        char *readingA = strdupWtoA( readingW ), *stringA = strdupWtoA( stringW );
        ret = ime->pImeEnumRegisterWord( procW, readingA, style, stringA, user );
        HeapFree( GetProcessHeap(), 0, readingA );
        HeapFree( GetProcessHeap(), 0, stringA );
    }

    return ret;
}

static inline BOOL EscapeRequiresWA(UINT uEscape)
{
        if (uEscape == IME_ESC_GET_EUDC_DICTIONARY ||
            uEscape == IME_ESC_SET_EUDC_DICTIONARY ||
            uEscape == IME_ESC_IME_NAME ||
            uEscape == IME_ESC_GETHELPFILENAME)
        return TRUE;
    return FALSE;
}

/***********************************************************************
 *		ImmEscapeA (IMM32.@)
 */
LRESULT WINAPI ImmEscapeA( HKL hkl, HIMC himc, UINT code, void *data )
{
    struct ime *ime = IMM_GetImmHkl( hkl );
    LRESULT ret;

    TRACE( "hkl %p, himc %p, code %u, data %p.\n", hkl, himc, code, data );

    if (!ime->hIME || !ime->pImeEscape) return 0;

    if (!EscapeRequiresWA( code ) || !is_kbd_ime_unicode( ime ))
        ret = ime->pImeEscape( himc, code, data );
    else
    {
        WCHAR buffer[81]; /* largest required buffer should be 80 */
        if (code == IME_ESC_SET_EUDC_DICTIONARY)
        {
            MultiByteToWideChar( CP_ACP, 0, data, -1, buffer, 81 );
            ret = ime->pImeEscape( himc, code, buffer );
        }
        else
        {
            ret = ime->pImeEscape( himc, code, buffer );
            WideCharToMultiByte( CP_ACP, 0, buffer, -1, data, 80, NULL, NULL );
        }
    }

    return ret;
}

/***********************************************************************
 *		ImmEscapeW (IMM32.@)
 */
LRESULT WINAPI ImmEscapeW( HKL hkl, HIMC himc, UINT code, void *data )
{
    struct ime *ime = IMM_GetImmHkl( hkl );
    LRESULT ret;

    TRACE( "hkl %p, himc %p, code %u, data %p.\n", hkl, himc, code, data );

    if (!ime->hIME || !ime->pImeEscape) return 0;

    if (!EscapeRequiresWA( code ) || is_kbd_ime_unicode( ime ))
        ret = ime->pImeEscape( himc, code, data );
    else
    {
        char buffer[81]; /* largest required buffer should be 80 */
        if (code == IME_ESC_SET_EUDC_DICTIONARY)
        {
            WideCharToMultiByte( CP_ACP, 0, data, -1, buffer, 81, NULL, NULL );
            ret = ime->pImeEscape( himc, code, buffer );
        }
        else
        {
            ret = ime->pImeEscape( himc, code, buffer );
            MultiByteToWideChar( CP_ACP, 0, buffer, -1, data, 80 );
        }
    }

    return ret;
}

/***********************************************************************
 *		ImmGetCandidateListA (IMM32.@)
 */
DWORD WINAPI ImmGetCandidateListA(
  HIMC hIMC, DWORD dwIndex,
  LPCANDIDATELIST lpCandList, DWORD dwBufLen)
{
    InputContextData *data = get_imc_data(hIMC);
    LPCANDIDATEINFO candinfo;
    LPCANDIDATELIST candlist;
    DWORD ret = 0;

    TRACE("%p, %ld, %p, %ld\n", hIMC, dwIndex, lpCandList, dwBufLen);

    if (!data || !data->IMC.hCandInfo)
       return 0;

    candinfo = ImmLockIMCC(data->IMC.hCandInfo);
    if (dwIndex >= candinfo->dwCount || dwIndex >= ARRAY_SIZE(candinfo->dwOffset))
        goto done;

    candlist = (LPCANDIDATELIST)((LPBYTE)candinfo + candinfo->dwOffset[dwIndex]);
    if ( !candlist->dwSize || !candlist->dwCount )
        goto done;

    if ( !is_himc_ime_unicode(data) )
    {
        ret = candlist->dwSize;
        if ( lpCandList && dwBufLen >= ret )
            memcpy(lpCandList, candlist, ret);
    }
    else
        ret = convert_candidatelist_WtoA( candlist, lpCandList, dwBufLen);

done:
    ImmUnlockIMCC(data->IMC.hCandInfo);
    return ret;
}

/***********************************************************************
 *		ImmGetCandidateListCountA (IMM32.@)
 */
DWORD WINAPI ImmGetCandidateListCountA(
  HIMC hIMC, LPDWORD lpdwListCount)
{
    InputContextData *data = get_imc_data(hIMC);
    LPCANDIDATEINFO candinfo;
    DWORD ret, count;

    TRACE("%p, %p\n", hIMC, lpdwListCount);

    if (!data || !lpdwListCount || !data->IMC.hCandInfo)
       return 0;

    candinfo = ImmLockIMCC(data->IMC.hCandInfo);

    *lpdwListCount = count = candinfo->dwCount;

    if ( !is_himc_ime_unicode(data) )
        ret = candinfo->dwSize;
    else
    {
        ret = sizeof(CANDIDATEINFO);
        while ( count-- )
            ret += ImmGetCandidateListA(hIMC, count, NULL, 0);
    }

    ImmUnlockIMCC(data->IMC.hCandInfo);
    return ret;
}

/***********************************************************************
 *		ImmGetCandidateListCountW (IMM32.@)
 */
DWORD WINAPI ImmGetCandidateListCountW(
  HIMC hIMC, LPDWORD lpdwListCount)
{
    InputContextData *data = get_imc_data(hIMC);
    LPCANDIDATEINFO candinfo;
    DWORD ret, count;

    TRACE("%p, %p\n", hIMC, lpdwListCount);

    if (!data || !lpdwListCount || !data->IMC.hCandInfo)
       return 0;

    candinfo = ImmLockIMCC(data->IMC.hCandInfo);

    *lpdwListCount = count = candinfo->dwCount;

    if ( is_himc_ime_unicode(data) )
        ret = candinfo->dwSize;
    else
    {
        ret = sizeof(CANDIDATEINFO);
        while ( count-- )
            ret += ImmGetCandidateListW(hIMC, count, NULL, 0);
    }

    ImmUnlockIMCC(data->IMC.hCandInfo);
    return ret;
}

/***********************************************************************
 *		ImmGetCandidateListW (IMM32.@)
 */
DWORD WINAPI ImmGetCandidateListW(
  HIMC hIMC, DWORD dwIndex,
  LPCANDIDATELIST lpCandList, DWORD dwBufLen)
{
    InputContextData *data = get_imc_data(hIMC);
    LPCANDIDATEINFO candinfo;
    LPCANDIDATELIST candlist;
    DWORD ret = 0;

    TRACE("%p, %ld, %p, %ld\n", hIMC, dwIndex, lpCandList, dwBufLen);

    if (!data || !data->IMC.hCandInfo)
       return 0;

    candinfo = ImmLockIMCC(data->IMC.hCandInfo);
    if (dwIndex >= candinfo->dwCount || dwIndex >= ARRAY_SIZE(candinfo->dwOffset))
        goto done;

    candlist = (LPCANDIDATELIST)((LPBYTE)candinfo + candinfo->dwOffset[dwIndex]);
    if ( !candlist->dwSize || !candlist->dwCount )
        goto done;

    if ( is_himc_ime_unicode(data) )
    {
        ret = candlist->dwSize;
        if ( lpCandList && dwBufLen >= ret )
            memcpy(lpCandList, candlist, ret);
    }
    else
        ret = convert_candidatelist_AtoW( candlist, lpCandList, dwBufLen);

done:
    ImmUnlockIMCC(data->IMC.hCandInfo);
    return ret;
}

/***********************************************************************
 *		ImmGetCandidateWindow (IMM32.@)
 */
BOOL WINAPI ImmGetCandidateWindow(
  HIMC hIMC, DWORD dwIndex, LPCANDIDATEFORM lpCandidate)
{
    InputContextData *data = get_imc_data(hIMC);

    TRACE("%p, %ld, %p\n", hIMC, dwIndex, lpCandidate);

    if (!data || !lpCandidate)
        return FALSE;

    if (dwIndex >= ARRAY_SIZE(data->IMC.cfCandForm))
        return FALSE;

    if (data->IMC.cfCandForm[dwIndex].dwIndex != dwIndex)
        return FALSE;

    *lpCandidate = data->IMC.cfCandForm[dwIndex];

    return TRUE;
}

/***********************************************************************
 *		ImmGetCompositionFontA (IMM32.@)
 */
BOOL WINAPI ImmGetCompositionFontA(HIMC hIMC, LPLOGFONTA lplf)
{
    LOGFONTW lfW;
    BOOL rc;

    TRACE("(%p, %p):\n", hIMC, lplf);

    rc = ImmGetCompositionFontW(hIMC,&lfW);
    if (!rc || !lplf)
        return FALSE;

    memcpy(lplf,&lfW,sizeof(LOGFONTA));
    WideCharToMultiByte(CP_ACP, 0, lfW.lfFaceName, -1, lplf->lfFaceName,
                        LF_FACESIZE, NULL, NULL);
    return TRUE;
}

/***********************************************************************
 *		ImmGetCompositionFontW (IMM32.@)
 */
BOOL WINAPI ImmGetCompositionFontW(HIMC hIMC, LPLOGFONTW lplf)
{
    InputContextData *data = get_imc_data(hIMC);

    TRACE("(%p, %p):\n", hIMC, lplf);

    if (!data || !lplf)
        return FALSE;

    *lplf = data->IMC.lfFont.W;

    return TRUE;
}


/* Helpers for the GetCompositionString functions */

/* Source encoding is defined by context, source length is always given in respective characters. Destination buffer
   length is always in bytes. */
static INT CopyCompStringIMEtoClient(const InputContextData *data, const void *src, INT src_len, void *dst,
        INT dst_len, BOOL unicode)
{
    int char_size = unicode ? sizeof(WCHAR) : sizeof(char);
    INT ret;

    if (is_himc_ime_unicode(data) ^ unicode)
    {
        if (unicode)
            ret = MultiByteToWideChar(CP_ACP, 0, src, src_len, dst, dst_len / sizeof(WCHAR));
        else
            ret = WideCharToMultiByte(CP_ACP, 0, src, src_len, dst, dst_len, NULL, NULL);
        ret *= char_size;
    }
    else
    {
        if (dst_len)
        {
            ret = min(src_len * char_size, dst_len);
            memcpy(dst, src, ret);
        }
        else
            ret = src_len * char_size;
    }

    return ret;
}

/* Composition string encoding is defined by context, returned attributes correspond to string, converted according to
   passed mode. String length is in characters, attributes are in byte arrays. */
static INT CopyCompAttrIMEtoClient(const InputContextData *data, const BYTE *src, INT src_len, const void *comp_string,
        INT str_len, BYTE *dst, INT dst_len, BOOL unicode)
{
    union
    {
        const void *str;
        const WCHAR *strW;
        const char *strA;
    } string;
    INT rc;

    string.str = comp_string;

    if (is_himc_ime_unicode(data) && !unicode)
    {
        rc = WideCharToMultiByte(CP_ACP, 0, string.strW, str_len, NULL, 0, NULL, NULL);
        if (dst_len)
        {
            int i, j = 0, k = 0;

            if (rc < dst_len)
                dst_len = rc;
            for (i = 0; i < str_len; ++i)
            {
                int len;

                len = WideCharToMultiByte(CP_ACP, 0, string.strW + i, 1, NULL, 0, NULL, NULL);
                for (; len > 0; --len)
                {
                    dst[j++] = src[k];

                    if (j >= dst_len)
                        goto end;
                }
                ++k;
            }
        end:
            rc = j;
        }
    }
    else if (!is_himc_ime_unicode(data) && unicode)
    {
        rc = MultiByteToWideChar(CP_ACP, 0, string.strA, str_len, NULL, 0);
        if (dst_len)
        {
            int i, j = 0;

            if (rc < dst_len)
                dst_len = rc;
            for (i = 0; i < str_len; ++i)
            {
                if (IsDBCSLeadByte(string.strA[i]))
                    continue;

                dst[j++] = src[i];

                if (j >= dst_len)
                    break;
            }
            rc = j;
        }
    }
    else
    {
        memcpy(dst, src, min(src_len, dst_len));
        rc = src_len;
    }

    return rc;
}

static INT CopyCompClauseIMEtoClient(InputContextData *data, LPBYTE source, INT slen, LPBYTE ssource,
                                     LPBYTE target, INT tlen, BOOL unicode )
{
    INT rc;

    if (is_himc_ime_unicode(data) && !unicode)
    {
        if (tlen)
        {
            int i;

            if (slen < tlen)
                tlen = slen;
            tlen /= sizeof (DWORD);
            for (i = 0; i < tlen; ++i)
            {
                ((DWORD *)target)[i] = WideCharToMultiByte(CP_ACP, 0, (LPWSTR)ssource,
                                                          ((DWORD *)source)[i],
                                                          NULL, 0,
                                                          NULL, NULL);
            }
            rc = sizeof (DWORD) * i;
        }
        else
            rc = slen;
    }
    else if (!is_himc_ime_unicode(data) && unicode)
    {
        if (tlen)
        {
            int i;

            if (slen < tlen)
                tlen = slen;
            tlen /= sizeof (DWORD);
            for (i = 0; i < tlen; ++i)
            {
                ((DWORD *)target)[i] = MultiByteToWideChar(CP_ACP, 0, (LPSTR)ssource,
                                                          ((DWORD *)source)[i],
                                                          NULL, 0);
            }
            rc = sizeof (DWORD) * i;
        }
        else
            rc = slen;
    }
    else
    {
        memcpy( target, source, min(slen,tlen));
        rc = slen;
    }

    return rc;
}

static INT CopyCompOffsetIMEtoClient(InputContextData *data, DWORD offset, LPBYTE ssource, BOOL unicode)
{
    int rc;

    if (is_himc_ime_unicode(data) && !unicode)
    {
        rc = WideCharToMultiByte(CP_ACP, 0, (LPWSTR)ssource, offset, NULL, 0, NULL, NULL);
    }
    else if (!is_himc_ime_unicode(data) && unicode)
    {
        rc = MultiByteToWideChar(CP_ACP, 0, (LPSTR)ssource, offset, NULL, 0);
    }
    else
        rc = offset;

    return rc;
}

static LONG ImmGetCompositionStringT( HIMC hIMC, DWORD dwIndex, LPVOID lpBuf,
                                      DWORD dwBufLen, BOOL unicode)
{
    LONG rc = 0;
    InputContextData *data = get_imc_data(hIMC);
    LPCOMPOSITIONSTRING compstr;
    LPBYTE compdata;

    TRACE("(%p, 0x%lx, %p, %ld)\n", hIMC, dwIndex, lpBuf, dwBufLen);

    if (!data)
       return FALSE;

    if (!data->IMC.hCompStr)
       return FALSE;

    compdata = ImmLockIMCC(data->IMC.hCompStr);
    compstr = (LPCOMPOSITIONSTRING)compdata;

    switch (dwIndex)
    {
    case GCS_RESULTSTR:
        TRACE("GCS_RESULTSTR\n");
        rc = CopyCompStringIMEtoClient(data, compdata + compstr->dwResultStrOffset, compstr->dwResultStrLen, lpBuf, dwBufLen, unicode);
        break;
    case GCS_COMPSTR:
        TRACE("GCS_COMPSTR\n");
        rc = CopyCompStringIMEtoClient(data, compdata + compstr->dwCompStrOffset, compstr->dwCompStrLen, lpBuf, dwBufLen, unicode);
        break;
    case GCS_COMPATTR:
        TRACE("GCS_COMPATTR\n");
        rc = CopyCompAttrIMEtoClient(data, compdata + compstr->dwCompAttrOffset, compstr->dwCompAttrLen,
                                     compdata + compstr->dwCompStrOffset, compstr->dwCompStrLen,
                                     lpBuf, dwBufLen, unicode);
        break;
    case GCS_COMPCLAUSE:
        TRACE("GCS_COMPCLAUSE\n");
        rc = CopyCompClauseIMEtoClient(data, compdata + compstr->dwCompClauseOffset,compstr->dwCompClauseLen,
                                       compdata + compstr->dwCompStrOffset,
                                       lpBuf, dwBufLen, unicode);
        break;
    case GCS_RESULTCLAUSE:
        TRACE("GCS_RESULTCLAUSE\n");
        rc = CopyCompClauseIMEtoClient(data, compdata + compstr->dwResultClauseOffset,compstr->dwResultClauseLen,
                                       compdata + compstr->dwResultStrOffset,
                                       lpBuf, dwBufLen, unicode);
        break;
    case GCS_RESULTREADSTR:
        TRACE("GCS_RESULTREADSTR\n");
        rc = CopyCompStringIMEtoClient(data, compdata + compstr->dwResultReadStrOffset, compstr->dwResultReadStrLen, lpBuf, dwBufLen, unicode);
        break;
    case GCS_RESULTREADCLAUSE:
        TRACE("GCS_RESULTREADCLAUSE\n");
        rc = CopyCompClauseIMEtoClient(data, compdata + compstr->dwResultReadClauseOffset,compstr->dwResultReadClauseLen,
                                       compdata + compstr->dwResultStrOffset,
                                       lpBuf, dwBufLen, unicode);
        break;
    case GCS_COMPREADSTR:
        TRACE("GCS_COMPREADSTR\n");
        rc = CopyCompStringIMEtoClient(data, compdata + compstr->dwCompReadStrOffset, compstr->dwCompReadStrLen, lpBuf, dwBufLen, unicode);
        break;
    case GCS_COMPREADATTR:
        TRACE("GCS_COMPREADATTR\n");
        rc = CopyCompAttrIMEtoClient(data, compdata + compstr->dwCompReadAttrOffset, compstr->dwCompReadAttrLen,
                                     compdata + compstr->dwCompReadStrOffset, compstr->dwCompReadStrLen,
                                     lpBuf, dwBufLen, unicode);
        break;
    case GCS_COMPREADCLAUSE:
        TRACE("GCS_COMPREADCLAUSE\n");
        rc = CopyCompClauseIMEtoClient(data, compdata + compstr->dwCompReadClauseOffset,compstr->dwCompReadClauseLen,
                                       compdata + compstr->dwCompStrOffset,
                                       lpBuf, dwBufLen, unicode);
        break;
    case GCS_CURSORPOS:
        TRACE("GCS_CURSORPOS\n");
        rc = CopyCompOffsetIMEtoClient(data, compstr->dwCursorPos, compdata + compstr->dwCompStrOffset, unicode);
        break;
    case GCS_DELTASTART:
        TRACE("GCS_DELTASTART\n");
        rc = CopyCompOffsetIMEtoClient(data, compstr->dwDeltaStart, compdata + compstr->dwCompStrOffset, unicode);
        break;
    default:
        FIXME("Unhandled index 0x%lx\n",dwIndex);
        break;
    }

    ImmUnlockIMCC(data->IMC.hCompStr);

    return rc;
}

/***********************************************************************
 *		ImmGetCompositionStringA (IMM32.@)
 */
LONG WINAPI ImmGetCompositionStringA(
  HIMC hIMC, DWORD dwIndex, LPVOID lpBuf, DWORD dwBufLen)
{
    return ImmGetCompositionStringT(hIMC, dwIndex, lpBuf, dwBufLen, FALSE);
}


/***********************************************************************
 *		ImmGetCompositionStringW (IMM32.@)
 */
LONG WINAPI ImmGetCompositionStringW(
  HIMC hIMC, DWORD dwIndex,
  LPVOID lpBuf, DWORD dwBufLen)
{
    return ImmGetCompositionStringT(hIMC, dwIndex, lpBuf, dwBufLen, TRUE);
}

/***********************************************************************
 *		ImmGetCompositionWindow (IMM32.@)
 */
BOOL WINAPI ImmGetCompositionWindow(HIMC hIMC, LPCOMPOSITIONFORM lpCompForm)
{
    InputContextData *data = get_imc_data(hIMC);

    TRACE("(%p, %p)\n", hIMC, lpCompForm);

    if (!data)
        return FALSE;

    *lpCompForm = data->IMC.cfCompForm;
    return TRUE;
}

/***********************************************************************
 *		ImmGetContext (IMM32.@)
 *
 */
HIMC WINAPI ImmGetContext(HWND hWnd)
{
    HIMC rc;

    TRACE("%p\n", hWnd);

    rc = NtUserGetWindowInputContext(hWnd);

    if (rc)
    {
        InputContextData *data = get_imc_data(rc);
        if (data) data->IMC.hWnd = hWnd;
        else rc = 0;
    }

    TRACE("returning %p\n", rc);

    return rc;
}

/***********************************************************************
 *		ImmGetConversionListA (IMM32.@)
 */
DWORD WINAPI ImmGetConversionListA( HKL hkl, HIMC himc, const char *srcA, CANDIDATELIST *listA,
                                    DWORD lengthA, UINT flags )
{
    struct ime *ime = IMM_GetImmHkl( hkl );
    DWORD ret;

    TRACE( "hkl %p, himc %p, srcA %s, listA %p, lengthA %lu, flags %#x.\n", hkl, himc,
           debugstr_a(srcA), listA, lengthA, flags );

    if (!ime->hIME || !ime->pImeConversionList) return 0;

    if (!is_kbd_ime_unicode( ime ))
        ret = ime->pImeConversionList( himc, srcA, listA, lengthA, flags );
    else
    {
        CANDIDATELIST *listW;
        WCHAR *srcW = strdupAtoW( srcA );
        DWORD lengthW = ime->pImeConversionList( himc, srcW, NULL, 0, flags );

        if (!(listW = HeapAlloc( GetProcessHeap(), 0, lengthW ))) ret = 0;
        else
        {
            ime->pImeConversionList( himc, srcW, listW, lengthW, flags );
            ret = convert_candidatelist_WtoA( listW, listA, lengthA );
            HeapFree( GetProcessHeap(), 0, listW );
        }
        HeapFree( GetProcessHeap(), 0, srcW );
    }

    return ret;
}

/***********************************************************************
 *		ImmGetConversionListW (IMM32.@)
 */
DWORD WINAPI ImmGetConversionListW( HKL hkl, HIMC himc, const WCHAR *srcW, CANDIDATELIST *listW,
                                    DWORD lengthW, UINT flags )
{
    struct ime *ime = IMM_GetImmHkl( hkl );
    DWORD ret;

    TRACE( "hkl %p, himc %p, srcW %s, listW %p, lengthW %lu, flags %#x.\n", hkl, himc,
           debugstr_w(srcW), listW, lengthW, flags );

    if (!ime->hIME || !ime->pImeConversionList) return 0;

    if (is_kbd_ime_unicode( ime ))
        ret = ime->pImeConversionList( himc, srcW, listW, lengthW, flags );
    else
    {
        CANDIDATELIST *listA;
        char *srcA = strdupWtoA( srcW );
        DWORD lengthA = ime->pImeConversionList( himc, srcA, NULL, 0, flags );

        if (!(listA = HeapAlloc( GetProcessHeap(), 0, lengthA ))) ret = 0;
        else
        {
            ime->pImeConversionList( himc, srcA, listA, lengthA, flags );
            ret = convert_candidatelist_AtoW( listA, listW, lengthW );
            HeapFree( GetProcessHeap(), 0, listA );
        }
        HeapFree( GetProcessHeap(), 0, srcA );
    }

    return ret;
}

/***********************************************************************
 *		ImmGetConversionStatus (IMM32.@)
 */
BOOL WINAPI ImmGetConversionStatus(
  HIMC hIMC, LPDWORD lpfdwConversion, LPDWORD lpfdwSentence)
{
    InputContextData *data = get_imc_data(hIMC);

    TRACE("%p %p %p\n", hIMC, lpfdwConversion, lpfdwSentence);

    if (!data)
        return FALSE;

    if (lpfdwConversion)
        *lpfdwConversion = data->IMC.fdwConversion;
    if (lpfdwSentence)
        *lpfdwSentence = data->IMC.fdwSentence;

    return TRUE;
}

/***********************************************************************
 *		ImmGetDefaultIMEWnd (IMM32.@)
 */
HWND WINAPI ImmGetDefaultIMEWnd(HWND hWnd)
{
    return NtUserGetDefaultImeWindow(hWnd);
}

/***********************************************************************
 *		ImmGetDescriptionA (IMM32.@)
 */
UINT WINAPI ImmGetDescriptionA(
  HKL hKL, LPSTR lpszDescription, UINT uBufLen)
{
  WCHAR *buf;
  DWORD len;

  TRACE("%p %p %d\n", hKL, lpszDescription, uBufLen);

  /* find out how many characters in the unicode buffer */
  len = ImmGetDescriptionW( hKL, NULL, 0 );
  if (!len)
    return 0;

  /* allocate a buffer of that size */
  buf = HeapAlloc( GetProcessHeap(), 0, (len + 1) * sizeof (WCHAR) );
  if( !buf )
  return 0;

  /* fetch the unicode buffer */
  len = ImmGetDescriptionW( hKL, buf, len + 1 );

  /* convert it back to ANSI */
  len = WideCharToMultiByte( CP_ACP, 0, buf, len + 1,
                             lpszDescription, uBufLen, NULL, NULL );

  HeapFree( GetProcessHeap(), 0, buf );

  if (len == 0)
    return 0;

  return len - 1;
}

/***********************************************************************
 *		ImmGetDescriptionW (IMM32.@)
 */
UINT WINAPI ImmGetDescriptionW(HKL hKL, LPWSTR lpszDescription, UINT uBufLen)
{
  FIXME("(%p, %p, %d): semi stub\n", hKL, lpszDescription, uBufLen);

  if (!hKL) return 0;
  if (!uBufLen) return lstrlenW(L"Wine XIM" );
  lstrcpynW( lpszDescription, L"Wine XIM", uBufLen );
  return lstrlenW( lpszDescription );
}

/***********************************************************************
 *		ImmGetGuideLineA (IMM32.@)
 */
DWORD WINAPI ImmGetGuideLineA(
  HIMC hIMC, DWORD dwIndex, LPSTR lpBuf, DWORD dwBufLen)
{
  FIXME("(%p, %ld, %s, %ld): stub\n",
    hIMC, dwIndex, debugstr_a(lpBuf), dwBufLen
  );
  SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
  return 0;
}

/***********************************************************************
 *		ImmGetGuideLineW (IMM32.@)
 */
DWORD WINAPI ImmGetGuideLineW(HIMC hIMC, DWORD dwIndex, LPWSTR lpBuf, DWORD dwBufLen)
{
  FIXME("(%p, %ld, %s, %ld): stub\n",
    hIMC, dwIndex, debugstr_w(lpBuf), dwBufLen
  );
  SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
  return 0;
}

/***********************************************************************
 *		ImmGetIMEFileNameA (IMM32.@)
 */
UINT WINAPI ImmGetIMEFileNameA( HKL hKL, LPSTR lpszFileName, UINT uBufLen)
{
    LPWSTR bufW = NULL;
    UINT wBufLen = uBufLen;
    UINT rc;

    if (uBufLen && lpszFileName)
        bufW = HeapAlloc(GetProcessHeap(),0,uBufLen * sizeof(WCHAR));
    else /* We need this to get the number of byte required */
    {
        bufW = HeapAlloc(GetProcessHeap(),0,MAX_PATH * sizeof(WCHAR));
        wBufLen = MAX_PATH;
    }

    rc = ImmGetIMEFileNameW(hKL,bufW,wBufLen);

    if (rc > 0)
    {
        if (uBufLen && lpszFileName)
            rc = WideCharToMultiByte(CP_ACP, 0, bufW, -1, lpszFileName,
                                 uBufLen, NULL, NULL);
        else /* get the length */
            rc = WideCharToMultiByte(CP_ACP, 0, bufW, -1, NULL, 0, NULL,
                                     NULL);
    }

    HeapFree(GetProcessHeap(),0,bufW);
    return rc;
}

/***********************************************************************
 *		ImmGetIMEFileNameW (IMM32.@)
 */
UINT WINAPI ImmGetIMEFileNameW(HKL hKL, LPWSTR lpszFileName, UINT uBufLen)
{
    HKEY hkey;
    DWORD length;
    DWORD rc;
    WCHAR regKey[ARRAY_SIZE(szImeRegFmt)+8];

    wsprintfW( regKey, szImeRegFmt, (ULONG_PTR)hKL );
    rc = RegOpenKeyW( HKEY_LOCAL_MACHINE, regKey, &hkey);
    if (rc != ERROR_SUCCESS)
    {
        SetLastError(rc);
        return 0;
    }

    length = 0;
    rc = RegGetValueW(hkey, NULL, L"Ime File", RRF_RT_REG_SZ, NULL, NULL, &length);

    if (rc != ERROR_SUCCESS)
    {
        RegCloseKey(hkey);
        SetLastError(rc);
        return 0;
    }
    if (length > uBufLen * sizeof(WCHAR) || !lpszFileName)
    {
        RegCloseKey(hkey);
        if (lpszFileName)
        {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        else
            return length / sizeof(WCHAR);
    }

    RegGetValueW(hkey, NULL, L"Ime File", RRF_RT_REG_SZ, NULL, lpszFileName, &length);

    RegCloseKey(hkey);

    return length / sizeof(WCHAR);
}

/***********************************************************************
 *		ImmGetOpenStatus (IMM32.@)
 */
BOOL WINAPI ImmGetOpenStatus(HIMC hIMC)
{
  InputContextData *data = get_imc_data(hIMC);
  static int i;

    if (!data)
        return FALSE;

    TRACE("(%p): semi-stub\n", hIMC);

    if (!i++)
      FIXME("(%p): semi-stub\n", hIMC);

  return data->IMC.fOpen;
}

/***********************************************************************
 *		ImmGetProperty (IMM32.@)
 */
DWORD WINAPI ImmGetProperty( HKL hkl, DWORD index )
{
    struct ime *ime;
    DWORD ret;

    TRACE( "hkl %p, index %lu.\n", hkl, index );

    ime = IMM_GetImmHkl( hkl );
    if (!ime || !ime->hIME) return 0;

    switch (index)
    {
    case IGP_PROPERTY: ret = ime->imeInfo.fdwProperty; break;
    case IGP_CONVERSION: ret = ime->imeInfo.fdwConversionCaps; break;
    case IGP_SENTENCE: ret = ime->imeInfo.fdwSentenceCaps; break;
    case IGP_SETCOMPSTR: ret = ime->imeInfo.fdwSCSCaps; break;
    case IGP_SELECT: ret = ime->imeInfo.fdwSelectCaps; break;
    case IGP_GETIMEVERSION: ret = IMEVER_0400; break;
    case IGP_UI: ret = 0; break;
    default: ret = 0; break;
    }

    return ret;
}

/***********************************************************************
 *		ImmGetRegisterWordStyleA (IMM32.@)
 */
UINT WINAPI ImmGetRegisterWordStyleA( HKL hkl, UINT count, STYLEBUFA *styleA )
{
    struct ime *ime = IMM_GetImmHkl( hkl );
    UINT ret;

    TRACE( "hkl %p, count %u, styleA %p.\n", hkl, count, styleA );

    if (!ime->hIME || !ime->pImeGetRegisterWordStyle) return 0;

    if (!is_kbd_ime_unicode( ime ))
        ret = ime->pImeGetRegisterWordStyle( count, styleA );
    else
    {
        STYLEBUFW styleW;
        ret = ime->pImeGetRegisterWordStyle( count, &styleW );
        WideCharToMultiByte( CP_ACP, 0, styleW.szDescription, -1, styleA->szDescription, 32, NULL, NULL );
        styleA->dwStyle = styleW.dwStyle;
    }

    return ret;
}

/***********************************************************************
 *		ImmGetRegisterWordStyleW (IMM32.@)
 */
UINT WINAPI ImmGetRegisterWordStyleW( HKL hkl, UINT count, STYLEBUFW *styleW )
{
    struct ime *ime = IMM_GetImmHkl( hkl );
    UINT ret;

    TRACE( "hkl %p, count %u, styleW %p.\n", hkl, count, styleW );

    if (!ime->hIME || !ime->pImeGetRegisterWordStyle) return 0;

    if (is_kbd_ime_unicode( ime ))
        ret = ime->pImeGetRegisterWordStyle( count, styleW );
    else
    {
        STYLEBUFA styleA;
        ret = ime->pImeGetRegisterWordStyle( count, &styleA );
        MultiByteToWideChar( CP_ACP, 0, styleA.szDescription, -1, styleW->szDescription, 32 );
        styleW->dwStyle = styleA.dwStyle;
    }

    return ret;
}

/***********************************************************************
 *		ImmGetStatusWindowPos (IMM32.@)
 */
BOOL WINAPI ImmGetStatusWindowPos(HIMC hIMC, LPPOINT lpptPos)
{
    InputContextData *data = get_imc_data(hIMC);

    TRACE("(%p, %p)\n", hIMC, lpptPos);

    if (!data || !lpptPos)
        return FALSE;

    *lpptPos = data->IMC.ptStatusWndPos;

    return TRUE;
}

/***********************************************************************
 *		ImmGetVirtualKey (IMM32.@)
 */
UINT WINAPI ImmGetVirtualKey(HWND hWnd)
{
  OSVERSIONINFOA version;
  InputContextData *data = get_imc_data( ImmGetContext( hWnd ));
  TRACE("%p\n", hWnd);

  if ( data )
      return data->lastVK;

  version.dwOSVersionInfoSize = sizeof(OSVERSIONINFOA);
  GetVersionExA( &version );
  switch(version.dwPlatformId)
  {
  case VER_PLATFORM_WIN32_WINDOWS:
      return VK_PROCESSKEY;
  case VER_PLATFORM_WIN32_NT:
      return 0;
  default:
      FIXME("%ld not supported\n",version.dwPlatformId);
      return VK_PROCESSKEY;
  }
}

/***********************************************************************
 *		ImmInstallIMEA (IMM32.@)
 */
HKL WINAPI ImmInstallIMEA(
  LPCSTR lpszIMEFileName, LPCSTR lpszLayoutText)
{
    LPWSTR lpszwIMEFileName;
    LPWSTR lpszwLayoutText;
    HKL hkl;

    TRACE ("(%s, %s)\n", debugstr_a(lpszIMEFileName),
                         debugstr_a(lpszLayoutText));

    lpszwIMEFileName = strdupAtoW(lpszIMEFileName);
    lpszwLayoutText = strdupAtoW(lpszLayoutText);

    hkl = ImmInstallIMEW(lpszwIMEFileName, lpszwLayoutText);

    HeapFree(GetProcessHeap(),0,lpszwIMEFileName);
    HeapFree(GetProcessHeap(),0,lpszwLayoutText);
    return hkl;
}

/***********************************************************************
 *		ImmInstallIMEW (IMM32.@)
 */
HKL WINAPI ImmInstallIMEW(
  LPCWSTR lpszIMEFileName, LPCWSTR lpszLayoutText)
{
    INT lcid = GetUserDefaultLCID();
    INT count;
    HKL hkl;
    DWORD rc;
    HKEY hkey;
    WCHAR regKey[ARRAY_SIZE(szImeRegFmt)+8];

    TRACE ("(%s, %s):\n", debugstr_w(lpszIMEFileName),
                          debugstr_w(lpszLayoutText));

    /* Start with 2.  e001 will be blank and so default to the wine internal IME */
    count = 2;

    while (count < 0xfff)
    {
        DWORD disposition = 0;

        hkl = (HKL)MAKELPARAM( lcid, 0xe000 | count );
        wsprintfW( regKey, szImeRegFmt, (ULONG_PTR)hkl);

        rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, regKey, 0, NULL, 0, KEY_WRITE, NULL, &hkey, &disposition);
        if (rc == ERROR_SUCCESS && disposition == REG_CREATED_NEW_KEY)
            break;
        else if (rc == ERROR_SUCCESS)
            RegCloseKey(hkey);

        count++;
    }

    if (count == 0xfff)
    {
        WARN("Unable to find slot to install IME\n");
        return 0;
    }

    if (rc == ERROR_SUCCESS)
    {
        rc = RegSetValueExW(hkey, L"Ime File", 0, REG_SZ, (const BYTE*)lpszIMEFileName,
                            (lstrlenW(lpszIMEFileName) + 1) * sizeof(WCHAR));
        if (rc == ERROR_SUCCESS)
            rc = RegSetValueExW(hkey, L"Layout Text", 0, REG_SZ, (const BYTE*)lpszLayoutText,
                                (lstrlenW(lpszLayoutText) + 1) * sizeof(WCHAR));
        RegCloseKey(hkey);
        return hkl;
    }
    else
    {
        WARN("Unable to set IME registry values\n");
        return 0;
    }
}

/***********************************************************************
 *		ImmIsIME (IMM32.@)
 */
BOOL WINAPI ImmIsIME(HKL hKL)
{
    struct ime *ptr;
    TRACE("(%p):\n", hKL);
    ptr = IMM_GetImmHkl(hKL);
    return (ptr && ptr->hIME);
}

/***********************************************************************
 *		ImmIsUIMessageA (IMM32.@)
 */
BOOL WINAPI ImmIsUIMessageA(
  HWND hWndIME, UINT msg, WPARAM wParam, LPARAM lParam)
{
    TRACE("(%p, %x, %Id, %Id)\n", hWndIME, msg, wParam, lParam);
    if ((msg >= WM_IME_STARTCOMPOSITION && msg <= WM_IME_KEYLAST) ||
            (msg == WM_IME_SETCONTEXT) ||
            (msg == WM_IME_NOTIFY) ||
            (msg == WM_IME_COMPOSITIONFULL) ||
            (msg == WM_IME_SELECT) ||
            (msg == 0x287 /* FIXME: WM_IME_SYSTEM */))
    {
        if (hWndIME)
            SendMessageA(hWndIME, msg, wParam, lParam);

        return TRUE;
    }
    return FALSE;
}

/***********************************************************************
 *		ImmIsUIMessageW (IMM32.@)
 */
BOOL WINAPI ImmIsUIMessageW(
  HWND hWndIME, UINT msg, WPARAM wParam, LPARAM lParam)
{
    TRACE("(%p, %x, %Id, %Id)\n", hWndIME, msg, wParam, lParam);
    if ((msg >= WM_IME_STARTCOMPOSITION && msg <= WM_IME_KEYLAST) ||
            (msg == WM_IME_SETCONTEXT) ||
            (msg == WM_IME_NOTIFY) ||
            (msg == WM_IME_COMPOSITIONFULL) ||
            (msg == WM_IME_SELECT) ||
            (msg == 0x287 /* FIXME: WM_IME_SYSTEM */))
    {
        if (hWndIME)
            SendMessageW(hWndIME, msg, wParam, lParam);

        return TRUE;
    }
    return FALSE;
}

/***********************************************************************
 *		ImmNotifyIME (IMM32.@)
 */
BOOL WINAPI ImmNotifyIME(
  HIMC hIMC, DWORD dwAction, DWORD dwIndex, DWORD dwValue)
{
    InputContextData *data = get_imc_data(hIMC);

    TRACE("(%p, %ld, %ld, %ld)\n",
        hIMC, dwAction, dwIndex, dwValue);

    if (hIMC == NULL)
    {
        SetLastError(ERROR_SUCCESS);
        return FALSE;
    }

    if (!data || ! data->immKbd->pNotifyIME)
    {
        return FALSE;
    }

    return data->immKbd->pNotifyIME(hIMC,dwAction,dwIndex,dwValue);
}

/***********************************************************************
 *		ImmRegisterWordA (IMM32.@)
 */
BOOL WINAPI ImmRegisterWordA( HKL hkl, const char *readingA, DWORD style, const char *stringA )
{
    struct ime *ime = IMM_GetImmHkl( hkl );
    BOOL ret;

    TRACE( "hkl %p, readingA %s, style %lu, stringA %s.\n", hkl, debugstr_a(readingA), style, debugstr_a(stringA) );

    if (!ime->hIME || !ime->pImeRegisterWord) return FALSE;

    if (!is_kbd_ime_unicode( ime ))
        ret = ime->pImeRegisterWord( readingA, style, stringA );
    else
    {
        WCHAR *readingW = strdupAtoW( readingA ), *stringW = strdupAtoW( stringA );
        ret = ime->pImeRegisterWord( readingW, style, stringW );
        HeapFree( GetProcessHeap(), 0, readingW );
        HeapFree( GetProcessHeap(), 0, stringW );
    }

    return ret;
}

/***********************************************************************
 *		ImmRegisterWordW (IMM32.@)
 */
BOOL WINAPI ImmRegisterWordW( HKL hkl, const WCHAR *readingW, DWORD style, const WCHAR *stringW )
{
    struct ime *ime = IMM_GetImmHkl( hkl );
    BOOL ret;

    TRACE( "hkl %p, readingW %s, style %lu, stringW %s.\n", hkl, debugstr_w(readingW), style, debugstr_w(stringW) );

    if (!ime->hIME || !ime->pImeRegisterWord) return FALSE;

    if (is_kbd_ime_unicode( ime ))
        ret = ime->pImeRegisterWord( readingW, style, stringW );
    else
    {
        char *readingA = strdupWtoA( readingW ), *stringA = strdupWtoA( stringW );
        ret = ime->pImeRegisterWord( readingA, style, stringA );
        HeapFree( GetProcessHeap(), 0, readingA );
        HeapFree( GetProcessHeap(), 0, stringA );
    }

    return ret;
}

/***********************************************************************
 *		ImmReleaseContext (IMM32.@)
 */
BOOL WINAPI ImmReleaseContext(HWND hWnd, HIMC hIMC)
{
  static BOOL shown = FALSE;

  if (!shown) {
     FIXME("(%p, %p): stub\n", hWnd, hIMC);
     shown = TRUE;
  }
  return TRUE;
}

/***********************************************************************
*              ImmRequestMessageA(IMM32.@)
*/
LRESULT WINAPI ImmRequestMessageA(HIMC hIMC, WPARAM wParam, LPARAM lParam)
{
    InputContextData *data = get_imc_data(hIMC);

    TRACE("%p %Id %Id\n", hIMC, wParam, wParam);

    if (data) return SendMessageA(data->IMC.hWnd, WM_IME_REQUEST, wParam, lParam);

    SetLastError(ERROR_INVALID_HANDLE);
    return 0;
}

/***********************************************************************
*              ImmRequestMessageW(IMM32.@)
*/
LRESULT WINAPI ImmRequestMessageW(HIMC hIMC, WPARAM wParam, LPARAM lParam)
{
    InputContextData *data = get_imc_data(hIMC);

    TRACE("%p %Id %Id\n", hIMC, wParam, wParam);

    if (data) return SendMessageW(data->IMC.hWnd, WM_IME_REQUEST, wParam, lParam);

    SetLastError(ERROR_INVALID_HANDLE);
    return 0;
}

/***********************************************************************
 *		ImmSetCandidateWindow (IMM32.@)
 */
BOOL WINAPI ImmSetCandidateWindow(
  HIMC hIMC, LPCANDIDATEFORM lpCandidate)
{
    InputContextData *data = get_imc_data(hIMC);

    TRACE("(%p, %p)\n", hIMC, lpCandidate);

    if (!data || !lpCandidate)
        return FALSE;

    if (IMM_IsCrossThreadAccess(NULL, hIMC))
        return FALSE;

    TRACE("\t%lx, %lx, %s, %s\n",
          lpCandidate->dwIndex, lpCandidate->dwStyle,
          wine_dbgstr_point(&lpCandidate->ptCurrentPos),
          wine_dbgstr_rect(&lpCandidate->rcArea));

    if (lpCandidate->dwIndex >= ARRAY_SIZE(data->IMC.cfCandForm))
        return FALSE;

    data->IMC.cfCandForm[lpCandidate->dwIndex] = *lpCandidate;
    ImmNotifyIME(hIMC, NI_CONTEXTUPDATED, 0, IMC_SETCANDIDATEPOS);
    ImmInternalSendIMENotify(data, IMN_SETCANDIDATEPOS, 1 << lpCandidate->dwIndex);

    return TRUE;
}

/***********************************************************************
 *		ImmSetCompositionFontA (IMM32.@)
 */
BOOL WINAPI ImmSetCompositionFontA(HIMC hIMC, LPLOGFONTA lplf)
{
    InputContextData *data = get_imc_data(hIMC);
    TRACE("(%p, %p)\n", hIMC, lplf);

    if (!data || !lplf)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    if (IMM_IsCrossThreadAccess(NULL, hIMC))
        return FALSE;

    memcpy(&data->IMC.lfFont.W,lplf,sizeof(LOGFONTA));
    MultiByteToWideChar(CP_ACP, 0, lplf->lfFaceName, -1, data->IMC.lfFont.W.lfFaceName,
                        LF_FACESIZE);
    ImmNotifyIME(hIMC, NI_CONTEXTUPDATED, 0, IMC_SETCOMPOSITIONFONT);
    ImmInternalSendIMENotify(data, IMN_SETCOMPOSITIONFONT, 0);

    return TRUE;
}

/***********************************************************************
 *		ImmSetCompositionFontW (IMM32.@)
 */
BOOL WINAPI ImmSetCompositionFontW(HIMC hIMC, LPLOGFONTW lplf)
{
    InputContextData *data = get_imc_data(hIMC);
    TRACE("(%p, %p)\n", hIMC, lplf);

    if (!data || !lplf)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    if (IMM_IsCrossThreadAccess(NULL, hIMC))
        return FALSE;

    data->IMC.lfFont.W = *lplf;
    ImmNotifyIME(hIMC, NI_CONTEXTUPDATED, 0, IMC_SETCOMPOSITIONFONT);
    ImmInternalSendIMENotify(data, IMN_SETCOMPOSITIONFONT, 0);

    return TRUE;
}

/***********************************************************************
 *		ImmSetCompositionStringA (IMM32.@)
 */
BOOL WINAPI ImmSetCompositionStringA(
  HIMC hIMC, DWORD dwIndex,
  LPCVOID lpComp, DWORD dwCompLen,
  LPCVOID lpRead, DWORD dwReadLen)
{
    DWORD comp_len;
    DWORD read_len;
    WCHAR *CompBuffer = NULL;
    WCHAR *ReadBuffer = NULL;
    BOOL rc;
    InputContextData *data = get_imc_data(hIMC);

    TRACE("(%p, %ld, %p, %ld, %p, %ld):\n",
            hIMC, dwIndex, lpComp, dwCompLen, lpRead, dwReadLen);

    if (!data)
        return FALSE;

    if (IMM_IsCrossThreadAccess(NULL, hIMC))
        return FALSE;

    if (!(dwIndex == SCS_SETSTR ||
          dwIndex == SCS_CHANGEATTR ||
          dwIndex == SCS_CHANGECLAUSE ||
          dwIndex == SCS_SETRECONVERTSTRING ||
          dwIndex == SCS_QUERYRECONVERTSTRING))
        return FALSE;

    if (!is_himc_ime_unicode(data))
        return data->immKbd->pImeSetCompositionString(hIMC, dwIndex, lpComp,
                        dwCompLen, lpRead, dwReadLen);

    comp_len = MultiByteToWideChar(CP_ACP, 0, lpComp, dwCompLen, NULL, 0);
    if (comp_len)
    {
        CompBuffer = HeapAlloc(GetProcessHeap(),0,comp_len * sizeof(WCHAR));
        MultiByteToWideChar(CP_ACP, 0, lpComp, dwCompLen, CompBuffer, comp_len);
    }

    read_len = MultiByteToWideChar(CP_ACP, 0, lpRead, dwReadLen, NULL, 0);
    if (read_len)
    {
        ReadBuffer = HeapAlloc(GetProcessHeap(),0,read_len * sizeof(WCHAR));
        MultiByteToWideChar(CP_ACP, 0, lpRead, dwReadLen, ReadBuffer, read_len);
    }

    rc =  ImmSetCompositionStringW(hIMC, dwIndex, CompBuffer, comp_len,
                                   ReadBuffer, read_len);

    HeapFree(GetProcessHeap(), 0, CompBuffer);
    HeapFree(GetProcessHeap(), 0, ReadBuffer);

    return rc;
}

/***********************************************************************
 *		ImmSetCompositionStringW (IMM32.@)
 */
BOOL WINAPI ImmSetCompositionStringW(
	HIMC hIMC, DWORD dwIndex,
	LPCVOID lpComp, DWORD dwCompLen,
	LPCVOID lpRead, DWORD dwReadLen)
{
    DWORD comp_len;
    DWORD read_len;
    CHAR *CompBuffer = NULL;
    CHAR *ReadBuffer = NULL;
    BOOL rc;
    InputContextData *data = get_imc_data(hIMC);

    TRACE("(%p, %ld, %p, %ld, %p, %ld):\n",
            hIMC, dwIndex, lpComp, dwCompLen, lpRead, dwReadLen);

    if (!data)
        return FALSE;

    if (IMM_IsCrossThreadAccess(NULL, hIMC))
        return FALSE;

    if (!(dwIndex == SCS_SETSTR ||
          dwIndex == SCS_CHANGEATTR ||
          dwIndex == SCS_CHANGECLAUSE ||
          dwIndex == SCS_SETRECONVERTSTRING ||
          dwIndex == SCS_QUERYRECONVERTSTRING))
        return FALSE;

    if (is_himc_ime_unicode(data))
        return data->immKbd->pImeSetCompositionString(hIMC, dwIndex, lpComp,
                        dwCompLen, lpRead, dwReadLen);

    comp_len = WideCharToMultiByte(CP_ACP, 0, lpComp, dwCompLen, NULL, 0, NULL,
                                   NULL);
    if (comp_len)
    {
        CompBuffer = HeapAlloc(GetProcessHeap(),0,comp_len);
        WideCharToMultiByte(CP_ACP, 0, lpComp, dwCompLen, CompBuffer, comp_len,
                            NULL, NULL);
    }

    read_len = WideCharToMultiByte(CP_ACP, 0, lpRead, dwReadLen, NULL, 0, NULL,
                                   NULL);
    if (read_len)
    {
        ReadBuffer = HeapAlloc(GetProcessHeap(),0,read_len);
        WideCharToMultiByte(CP_ACP, 0, lpRead, dwReadLen, ReadBuffer, read_len,
                            NULL, NULL);
    }

    rc =  ImmSetCompositionStringA(hIMC, dwIndex, CompBuffer, comp_len,
                                   ReadBuffer, read_len);

    HeapFree(GetProcessHeap(), 0, CompBuffer);
    HeapFree(GetProcessHeap(), 0, ReadBuffer);

    return rc;
}

/***********************************************************************
 *		ImmSetCompositionWindow (IMM32.@)
 */
BOOL WINAPI ImmSetCompositionWindow(
  HIMC hIMC, LPCOMPOSITIONFORM lpCompForm)
{
    BOOL reshow = FALSE;
    InputContextData *data = get_imc_data(hIMC);

    TRACE("(%p, %p)\n", hIMC, lpCompForm);
    if (lpCompForm)
        TRACE("\t%lx, %s, %s\n", lpCompForm->dwStyle,
              wine_dbgstr_point(&lpCompForm->ptCurrentPos),
              wine_dbgstr_rect(&lpCompForm->rcArea));

    if (!data)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    if (IMM_IsCrossThreadAccess(NULL, hIMC))
        return FALSE;

    data->IMC.cfCompForm = *lpCompForm;

    if (IsWindowVisible(data->immKbd->UIWnd))
    {
        reshow = TRUE;
        ShowWindow(data->immKbd->UIWnd,SW_HIDE);
    }

    /* FIXME: this is a partial stub */

    if (reshow)
        ShowWindow(data->immKbd->UIWnd,SW_SHOWNOACTIVATE);

    ImmInternalSendIMENotify(data, IMN_SETCOMPOSITIONWINDOW, 0);
    return TRUE;
}

/***********************************************************************
 *		ImmSetConversionStatus (IMM32.@)
 */
BOOL WINAPI ImmSetConversionStatus(
  HIMC hIMC, DWORD fdwConversion, DWORD fdwSentence)
{
    DWORD oldConversion, oldSentence;
    InputContextData *data = get_imc_data(hIMC);

    TRACE("%p %ld %ld\n", hIMC, fdwConversion, fdwSentence);

    if (!data)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    if (IMM_IsCrossThreadAccess(NULL, hIMC))
        return FALSE;

    if ( fdwConversion != data->IMC.fdwConversion )
    {
        oldConversion = data->IMC.fdwConversion;
        data->IMC.fdwConversion = fdwConversion;
        ImmNotifyIME(hIMC, NI_CONTEXTUPDATED, oldConversion, IMC_SETCONVERSIONMODE);
        ImmInternalSendIMENotify(data, IMN_SETCONVERSIONMODE, 0);
    }
    if ( fdwSentence != data->IMC.fdwSentence )
    {
        oldSentence = data->IMC.fdwSentence;
        data->IMC.fdwSentence = fdwSentence;
        ImmNotifyIME(hIMC, NI_CONTEXTUPDATED, oldSentence, IMC_SETSENTENCEMODE);
        ImmInternalSendIMENotify(data, IMN_SETSENTENCEMODE, 0);
    }

    return TRUE;
}

/***********************************************************************
 *		ImmSetOpenStatus (IMM32.@)
 */
BOOL WINAPI ImmSetOpenStatus(HIMC hIMC, BOOL fOpen)
{
    InputContextData *data = get_imc_data(hIMC);

    TRACE("%p %d\n", hIMC, fOpen);

    if (!data)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    if (IMM_IsCrossThreadAccess(NULL, hIMC))
        return FALSE;

    if (data->immKbd->UIWnd == NULL)
    {
        /* create the ime window */
        data->immKbd->UIWnd = CreateWindowExW( WS_EX_TOOLWINDOW,
                    data->immKbd->imeClassName, NULL, WS_POPUP, 0, 0, 1, 1, 0,
                    0, data->immKbd->hIME, 0);
        SetWindowLongPtrW(data->immKbd->UIWnd, IMMGWL_IMC, (LONG_PTR)data);
    }
    else if (fOpen)
        SetWindowLongPtrW(data->immKbd->UIWnd, IMMGWL_IMC, (LONG_PTR)data);

    if (!fOpen != !data->IMC.fOpen)
    {
        data->IMC.fOpen = fOpen;
        ImmNotifyIME( hIMC, NI_CONTEXTUPDATED, 0, IMC_SETOPENSTATUS);
        ImmInternalSendIMENotify(data, IMN_SETOPENSTATUS, 0);
    }

    return TRUE;
}

/***********************************************************************
 *		ImmSetStatusWindowPos (IMM32.@)
 */
BOOL WINAPI ImmSetStatusWindowPos(HIMC hIMC, LPPOINT lpptPos)
{
    InputContextData *data = get_imc_data(hIMC);

    TRACE("(%p, %p)\n", hIMC, lpptPos);

    if (!data || !lpptPos)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    if (IMM_IsCrossThreadAccess(NULL, hIMC))
        return FALSE;

    TRACE("\t%s\n", wine_dbgstr_point(lpptPos));

    data->IMC.ptStatusWndPos = *lpptPos;
    ImmNotifyIME( hIMC, NI_CONTEXTUPDATED, 0, IMC_SETSTATUSWINDOWPOS);
    ImmInternalSendIMENotify(data, IMN_SETSTATUSWINDOWPOS, 0);

    return TRUE;
}

/***********************************************************************
 *              ImmCreateSoftKeyboard(IMM32.@)
 */
HWND WINAPI ImmCreateSoftKeyboard(UINT uType, UINT hOwner, int x, int y)
{
    FIXME("(%d, %d, %d, %d): stub\n", uType, hOwner, x, y);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

/***********************************************************************
 *              ImmDestroySoftKeyboard(IMM32.@)
 */
BOOL WINAPI ImmDestroySoftKeyboard(HWND hSoftWnd)
{
    FIXME("(%p): stub\n", hSoftWnd);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/***********************************************************************
 *              ImmShowSoftKeyboard(IMM32.@)
 */
BOOL WINAPI ImmShowSoftKeyboard(HWND hSoftWnd, int nCmdShow)
{
    FIXME("(%p, %d): stub\n", hSoftWnd, nCmdShow);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/***********************************************************************
 *		ImmSimulateHotKey (IMM32.@)
 */
BOOL WINAPI ImmSimulateHotKey(HWND hWnd, DWORD dwHotKeyID)
{
  FIXME("(%p, %ld): stub\n", hWnd, dwHotKeyID);
  SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
  return FALSE;
}

/***********************************************************************
 *		ImmUnregisterWordA (IMM32.@)
 */
BOOL WINAPI ImmUnregisterWordA( HKL hkl, const char *readingA, DWORD style, const char *stringA )
{
    struct ime *ime = IMM_GetImmHkl( hkl );
    BOOL ret;

    TRACE( "hkl %p, readingA %s, style %lu, stringA %s.\n", hkl, debugstr_a(readingA), style, debugstr_a(stringA) );

    if (!ime->hIME || !ime->pImeUnregisterWord) return FALSE;

    if (!is_kbd_ime_unicode( ime ))
        ret = ime->pImeUnregisterWord( readingA, style, stringA );
    else
    {
        WCHAR *readingW = strdupAtoW( readingA ), *stringW = strdupAtoW( stringA );
        ret = ime->pImeUnregisterWord( readingW, style, stringW );
        HeapFree( GetProcessHeap(), 0, readingW );
        HeapFree( GetProcessHeap(), 0, stringW );
    }

    return ret;
}

/***********************************************************************
 *		ImmUnregisterWordW (IMM32.@)
 */
BOOL WINAPI ImmUnregisterWordW( HKL hkl, const WCHAR *readingW, DWORD style, const WCHAR *stringW )
{
    struct ime *ime = IMM_GetImmHkl( hkl );
    BOOL ret;

    TRACE( "hkl %p, readingW %s, style %lu, stringW %s.\n", hkl, debugstr_w(readingW), style, debugstr_w(stringW) );

    if (!ime->hIME || !ime->pImeUnregisterWord) return FALSE;

    if (is_kbd_ime_unicode( ime ))
        ret = ime->pImeUnregisterWord( readingW, style, stringW );
    else
    {
        char *readingA = strdupWtoA( readingW ), *stringA = strdupWtoA( stringW );
        ret = ime->pImeUnregisterWord( readingA, style, stringA );
        HeapFree( GetProcessHeap(), 0, readingA );
        HeapFree( GetProcessHeap(), 0, stringA );
    }

    return ret;
}

/***********************************************************************
 *		ImmGetImeMenuItemsA (IMM32.@)
 */
DWORD WINAPI ImmGetImeMenuItemsA( HIMC himc, DWORD flags, DWORD type, IMEMENUITEMINFOA *parentA,
                                  IMEMENUITEMINFOA *menuA, DWORD size )
{
    InputContextData *data = get_imc_data( himc );
    DWORD ret;

    TRACE( "himc %p, flags %#lx, type %lu, parentA %p, menuA %p, size %lu.\n",
           himc, flags, type, parentA, menuA, size );

    if (!data)
    {
        SetLastError( ERROR_INVALID_HANDLE );
        return 0;
    }

    if (!data->immKbd->hIME || !data->immKbd->pImeGetImeMenuItems) return 0;

    if (!is_himc_ime_unicode( data ) || (!parentA && !menuA))
        ret = data->immKbd->pImeGetImeMenuItems( himc, flags, type, parentA, menuA, size );
    else
    {
        IMEMENUITEMINFOW tmpW, *menuW, *parentW = parentA ? &tmpW : NULL;

        if (!menuA) menuW = NULL;
        else
        {
            int count = size / sizeof(LPIMEMENUITEMINFOA);
            size = count * sizeof(IMEMENUITEMINFOW);
            menuW = HeapAlloc( GetProcessHeap(), 0, size );
        }

        ret = data->immKbd->pImeGetImeMenuItems( himc, flags, type, parentW, menuW, size );

        if (parentA)
        {
            memcpy( parentA, parentW, sizeof(IMEMENUITEMINFOA) );
            parentA->hbmpItem = parentW->hbmpItem;
            WideCharToMultiByte( CP_ACP, 0, parentW->szString, -1, parentA->szString,
                                 IMEMENUITEM_STRING_SIZE, NULL, NULL );
        }
        if (menuA && ret)
        {
            unsigned int i;
            for (i = 0; i < ret; i++)
            {
                memcpy( &menuA[i], &menuW[1], sizeof(IMEMENUITEMINFOA) );
                menuA[i].hbmpItem = menuW[i].hbmpItem;
                WideCharToMultiByte( CP_ACP, 0, menuW[i].szString, -1, menuA[i].szString,
                                     IMEMENUITEM_STRING_SIZE, NULL, NULL );
            }
        }
        HeapFree( GetProcessHeap(), 0, menuW );
    }

    return ret;
}

/***********************************************************************
*		ImmGetImeMenuItemsW (IMM32.@)
*/
DWORD WINAPI ImmGetImeMenuItemsW( HIMC himc, DWORD flags, DWORD type, IMEMENUITEMINFOW *parentW,
                                  IMEMENUITEMINFOW *menuW, DWORD size )
{
    InputContextData *data = get_imc_data( himc );
    DWORD ret;

    TRACE( "himc %p, flags %#lx, type %lu, parentW %p, menuW %p, size %lu.\n",
           himc, flags, type, parentW, menuW, size );

    if (!data)
    {
        SetLastError( ERROR_INVALID_HANDLE );
        return 0;
    }

    if (!data->immKbd->hIME || !data->immKbd->pImeGetImeMenuItems) return 0;

    if (is_himc_ime_unicode( data ) || (!parentW && !menuW))
        ret = data->immKbd->pImeGetImeMenuItems( himc, flags, type, parentW, menuW, size );
    else
    {
        IMEMENUITEMINFOA tmpA, *menuA, *parentA = parentW ? &tmpA : NULL;

        if (!menuW) menuA = NULL;
        else
        {
            int count = size / sizeof(LPIMEMENUITEMINFOW);
            size = count * sizeof(IMEMENUITEMINFOA);
            menuA = HeapAlloc( GetProcessHeap(), 0, size );
        }

        ret = data->immKbd->pImeGetImeMenuItems( himc, flags, type, parentA, menuA, size );

        if (parentW)
        {
            memcpy( parentW, parentA, sizeof(IMEMENUITEMINFOA) );
            parentW->hbmpItem = parentA->hbmpItem;
            MultiByteToWideChar( CP_ACP, 0, parentA->szString, -1, parentW->szString, IMEMENUITEM_STRING_SIZE );
        }
        if (menuW && ret)
        {
            unsigned int i;
            for (i = 0; i < ret; i++)
            {
                memcpy( &menuW[i], &menuA[1], sizeof(IMEMENUITEMINFOA) );
                menuW[i].hbmpItem = menuA[i].hbmpItem;
                MultiByteToWideChar( CP_ACP, 0, menuA[i].szString, -1, menuW[i].szString, IMEMENUITEM_STRING_SIZE );
            }
        }
        HeapFree( GetProcessHeap(), 0, menuA );
    }

    return ret;
}

/***********************************************************************
*		ImmLockIMC(IMM32.@)
*/
LPINPUTCONTEXT WINAPI ImmLockIMC(HIMC hIMC)
{
    InputContextData *data = get_imc_data(hIMC);

    if (!data)
        return NULL;
    data->dwLock++;
    return &data->IMC;
}

/***********************************************************************
*		ImmUnlockIMC(IMM32.@)
*/
BOOL WINAPI ImmUnlockIMC(HIMC hIMC)
{
    InputContextData *data = get_imc_data(hIMC);

    if (!data)
        return FALSE;
    if (data->dwLock)
        data->dwLock--;
    return TRUE;
}

/***********************************************************************
*		ImmGetIMCLockCount(IMM32.@)
*/
DWORD WINAPI ImmGetIMCLockCount(HIMC hIMC)
{
    InputContextData *data = get_imc_data(hIMC);
    if (!data)
        return 0;
    return data->dwLock;
}

/***********************************************************************
*		ImmCreateIMCC(IMM32.@)
*/
HIMCC  WINAPI ImmCreateIMCC(DWORD size)
{
    return GlobalAlloc(GMEM_ZEROINIT | GMEM_MOVEABLE, size);
}

/***********************************************************************
*       ImmDestroyIMCC(IMM32.@)
*/
HIMCC WINAPI ImmDestroyIMCC(HIMCC block)
{
    return GlobalFree(block);
}

/***********************************************************************
*		ImmLockIMCC(IMM32.@)
*/
LPVOID WINAPI ImmLockIMCC(HIMCC imcc)
{
    return GlobalLock(imcc);
}

/***********************************************************************
*		ImmUnlockIMCC(IMM32.@)
*/
BOOL WINAPI ImmUnlockIMCC(HIMCC imcc)
{
    return GlobalUnlock(imcc);
}

/***********************************************************************
*		ImmGetIMCCLockCount(IMM32.@)
*/
DWORD WINAPI ImmGetIMCCLockCount(HIMCC imcc)
{
    return GlobalFlags(imcc) & GMEM_LOCKCOUNT;
}

/***********************************************************************
*		ImmReSizeIMCC(IMM32.@)
*/
HIMCC  WINAPI ImmReSizeIMCC(HIMCC imcc, DWORD size)
{
    return GlobalReAlloc(imcc, size, GMEM_ZEROINIT | GMEM_MOVEABLE);
}

/***********************************************************************
*		ImmGetIMCCSize(IMM32.@)
*/
DWORD WINAPI ImmGetIMCCSize(HIMCC imcc)
{
    return GlobalSize(imcc);
}

/***********************************************************************
*		ImmGenerateMessage(IMM32.@)
*/
BOOL WINAPI ImmGenerateMessage(HIMC hIMC)
{
    InputContextData *data = get_imc_data(hIMC);

    if (!data)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    TRACE("%li messages queued\n",data->IMC.dwNumMsgBuf);
    if (data->IMC.dwNumMsgBuf > 0)
    {
        LPTRANSMSG lpTransMsg;
        HIMCC hMsgBuf;
        DWORD i, dwNumMsgBuf;

        /* We are going to detach our hMsgBuff so that if processing messages
           generates new messages they go into a new buffer */
        hMsgBuf = data->IMC.hMsgBuf;
        dwNumMsgBuf = data->IMC.dwNumMsgBuf;

        data->IMC.hMsgBuf = ImmCreateIMCC(0);
        data->IMC.dwNumMsgBuf = 0;

        lpTransMsg = ImmLockIMCC(hMsgBuf);
        for (i = 0; i < dwNumMsgBuf; i++)
            ImmInternalSendIMEMessage(data, lpTransMsg[i].message, lpTransMsg[i].wParam, lpTransMsg[i].lParam);

        ImmUnlockIMCC(hMsgBuf);
        ImmDestroyIMCC(hMsgBuf);
    }

    return TRUE;
}

/***********************************************************************
*       ImmTranslateMessage(IMM32.@)
*       ( Undocumented, call internally and from user32.dll )
*/
BOOL WINAPI ImmTranslateMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lKeyData)
{
    InputContextData *data;
    HIMC imc = ImmGetContext(hwnd);
    BYTE state[256];
    UINT scancode;
    TRANSMSGLIST *list = NULL;
    UINT msg_count;
    UINT uVirtKey;
    static const DWORD list_count = 10;

    TRACE("%p %x %x %x\n",hwnd, msg, (UINT)wParam, (UINT)lKeyData);

    if (!(data = get_imc_data( imc ))) return FALSE;

    if (!data->immKbd->hIME || !data->immKbd->pImeToAsciiEx || data->lastVK == VK_PROCESSKEY)
        return FALSE;

    GetKeyboardState(state);
    scancode = lKeyData >> 0x10 & 0xff;

    list = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, list_count * sizeof(TRANSMSG) + sizeof(DWORD));
    list->uMsgCount = list_count;

    if (data->immKbd->imeInfo.fdwProperty & IME_PROP_KBD_CHAR_FIRST)
    {
        WCHAR chr;

        if (!is_himc_ime_unicode(data))
            ToAscii(data->lastVK, scancode, state, &chr, 0);
        else
            ToUnicodeEx(data->lastVK, scancode, state, &chr, 1, 0, GetKeyboardLayout(0));
        uVirtKey = MAKELONG(data->lastVK,chr);
    }
    else
        uVirtKey = data->lastVK;

    msg_count = data->immKbd->pImeToAsciiEx(uVirtKey, scancode, state, list, 0, imc);
    TRACE("%i messages generated\n",msg_count);
    if (msg_count && msg_count <= list_count)
    {
        UINT i;
        LPTRANSMSG msgs = list->TransMsg;

        for (i = 0; i < msg_count; i++)
            ImmInternalPostIMEMessage(data, msgs[i].message, msgs[i].wParam, msgs[i].lParam);
    }
    else if (msg_count > list_count)
        ImmGenerateMessage(imc);

    HeapFree(GetProcessHeap(),0,list);

    data->lastVK = VK_PROCESSKEY;

    return (msg_count > 0);
}

/***********************************************************************
*		ImmProcessKey(IMM32.@)
*       ( Undocumented, called from user32.dll )
*/
BOOL WINAPI ImmProcessKey(HWND hwnd, HKL hKL, UINT vKey, LPARAM lKeyData, DWORD unknown)
{
    InputContextData *data;
    HIMC imc = ImmGetContext(hwnd);
    BYTE state[256];

    TRACE("%p %p %x %x %lx\n",hwnd, hKL, vKey, (UINT)lKeyData, unknown);

    if (!(data = get_imc_data( imc ))) return FALSE;

    /* Make sure we are inputting to the correct keyboard */
    if (data->immKbd->hkl != hKL)
    {
        struct ime *new_hkl = IMM_GetImmHkl( hKL );
        if (new_hkl)
        {
            data->immKbd->pImeSelect(imc, FALSE);
            data->immKbd->uSelected--;
            data->immKbd = new_hkl;
            data->immKbd->pImeSelect(imc, TRUE);
            data->immKbd->uSelected++;
        }
        else
            return FALSE;
    }

    if (!data->immKbd->hIME || !data->immKbd->pImeProcessKey)
        return FALSE;

    GetKeyboardState(state);
    if (data->immKbd->pImeProcessKey(imc, vKey, lKeyData, state))
    {
        data->lastVK = vKey;
        return TRUE;
    }

    data->lastVK = VK_PROCESSKEY;
    return FALSE;
}

/***********************************************************************
*		ImmDisableTextFrameService(IMM32.@)
*/
BOOL WINAPI ImmDisableTextFrameService(DWORD idThread)
{
    FIXME("Stub\n");
    return FALSE;
}

/***********************************************************************
 *              ImmEnumInputContext(IMM32.@)
 */

BOOL WINAPI ImmEnumInputContext(DWORD idThread, IMCENUMPROC lpfn, LPARAM lParam)
{
    FIXME("Stub\n");
    return FALSE;
}

/***********************************************************************
 *              ImmGetHotKey(IMM32.@)
 */

BOOL WINAPI ImmGetHotKey(DWORD hotkey, UINT *modifiers, UINT *key, HKL *hkl)
{
    FIXME("%lx, %p, %p, %p: stub\n", hotkey, modifiers, key, hkl);
    return FALSE;
}

/***********************************************************************
 *              ImmDisableLegacyIME(IMM32.@)
 */
BOOL WINAPI ImmDisableLegacyIME(void)
{
    FIXME("stub\n");
    return TRUE;
}

static HWND get_ui_window(HKL hkl)
{
    struct ime *immHkl = IMM_GetImmHkl( hkl );
    return immHkl->UIWnd;
}

static BOOL is_ime_ui_msg(UINT msg)
{
    switch (msg)
    {
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_ENDCOMPOSITION:
    case WM_IME_COMPOSITION:
    case WM_IME_SETCONTEXT:
    case WM_IME_NOTIFY:
    case WM_IME_CONTROL:
    case WM_IME_COMPOSITIONFULL:
    case WM_IME_SELECT:
    case WM_IME_CHAR:
    case WM_IME_REQUEST:
    case WM_IME_KEYDOWN:
    case WM_IME_KEYUP:
        return TRUE;
    default:
        return msg == WM_MSIME_RECONVERTOPTIONS ||
            msg == WM_MSIME_SERVICE ||
            msg == WM_MSIME_MOUSE ||
            msg == WM_MSIME_RECONVERTREQUEST ||
            msg == WM_MSIME_RECONVERT ||
            msg == WM_MSIME_QUERYPOSITION ||
            msg == WM_MSIME_DOCUMENTFEED;
    }
}

static LRESULT ime_internal_msg( WPARAM wparam, LPARAM lparam)
{
    HWND hwnd = (HWND)lparam;
    HIMC himc;

    switch (wparam)
    {
    case IME_INTERNAL_ACTIVATE:
    case IME_INTERNAL_DEACTIVATE:
        himc = ImmGetContext(hwnd);
        ImmSetActiveContext(hwnd, himc, wparam == IME_INTERNAL_ACTIVATE);
        ImmReleaseContext(hwnd, himc);
        break;
    default:
        FIXME("wparam = %Ix\n", wparam);
        break;
    }

    return 0;
}

static void init_messages(void)
{
    static BOOL initialized;

    if (initialized) return;

    WM_MSIME_SERVICE = RegisterWindowMessageW(L"MSIMEService");
    WM_MSIME_RECONVERTOPTIONS = RegisterWindowMessageW(L"MSIMEReconvertOptions");
    WM_MSIME_MOUSE = RegisterWindowMessageW(L"MSIMEMouseOperation");
    WM_MSIME_RECONVERTREQUEST = RegisterWindowMessageW(L"MSIMEReconvertRequest");
    WM_MSIME_RECONVERT = RegisterWindowMessageW(L"MSIMEReconvert");
    WM_MSIME_QUERYPOSITION = RegisterWindowMessageW(L"MSIMEQueryPosition");
    WM_MSIME_DOCUMENTFEED = RegisterWindowMessageW(L"MSIMEDocumentFeed");
    initialized = TRUE;
}

LRESULT WINAPI __wine_ime_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, BOOL ansi)
{
    HWND uiwnd;

    switch (msg)
    {
    case WM_CREATE:
        init_messages();
        return TRUE;

    case WM_DESTROY:
        {
            HWND default_hwnd = ImmGetDefaultIMEWnd(0);
            if (!default_hwnd || hwnd == default_hwnd)
                imm_couninit_thread(TRUE);
        }
        return TRUE;

    case WM_IME_INTERNAL:
        return ime_internal_msg(wparam, lparam);
    }

    if (is_ime_ui_msg(msg))
    {
        if ((uiwnd = get_ui_window(NtUserGetKeyboardLayout(0))))
        {
            if (ansi)
                return SendMessageA(uiwnd, msg, wparam, lparam);
            else
                return SendMessageW(uiwnd, msg, wparam, lparam);
        }
        return FALSE;
    }

    if (ansi)
        return DefWindowProcA(hwnd, msg, wparam, lparam);
    else
        return DefWindowProcW(hwnd, msg, wparam, lparam);
}
