/*
 * PURPOSE: Load a DLL and run an entry point with the specified parameters
 *
 * Copyright 2002 Alberto Massari
 * Copyright 2001-2003 Aric Stewart for CodeWeavers
 * Copyright 2003 Mike McCormack for CodeWeavers
 * Copyright 2020 Alexandre Abgrall
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
 *
 */

/*
 *
 *  rundll32 dllname,entrypoint [arguments]
 *
 *  Documentation for this utility found on KB Q164787
 *
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Exclude rarely-used stuff from Windows headers */
#define WIN32_LEAN_AND_MEAN
#include "windows.h"
#include "wine/winbase16.h"
#include "wine/asm.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(rundll32);

static const WCHAR SZ_RUNDLL32[]   = {'\\','r','u','n','d','l','l','3','2','.','e','x','e',0};

#ifdef __i386__
/* wrapper for dlls that declare the entry point incorrectly */
extern void call_entry_point( void *func, HWND hwnd, HINSTANCE inst, void *cmdline, int show );
__ASM_GLOBAL_FUNC( call_entry_point,
                   "pushl %ebp\n\t"
                   __ASM_CFI(".cfi_adjust_cfa_offset 4\n\t")
                   __ASM_CFI(".cfi_rel_offset %ebp,0\n\t")
                   "movl %esp,%ebp\n\t"
                   __ASM_CFI(".cfi_def_cfa_register %ebp\n\t")
                   "pushl %edi\n\t"
                   __ASM_CFI(".cfi_rel_offset %edi,-4\n\t")
                   "pushl %esi\n\t"
                   __ASM_CFI(".cfi_rel_offset %esi,-8\n\t")
                   "pushl %ebx\n\t"
                   __ASM_CFI(".cfi_rel_offset %ebx,-12\n\t")
                   "subl $12,%esp\n\t"
                   "pushl 24(%ebp)\n\t"
                   "pushl 20(%ebp)\n\t"
                   "pushl 16(%ebp)\n\t"
                   "pushl 12(%ebp)\n\t"
                   "call *8(%ebp)\n\t"
                   "leal -12(%ebp),%esp\n\t"
                   "popl %ebx\n\t"
                   __ASM_CFI(".cfi_same_value %ebx\n\t")
                   "popl %esi\n\t"
                   __ASM_CFI(".cfi_same_value %esi\n\t")
                   "popl %edi\n\t"
                   __ASM_CFI(".cfi_same_value %edi\n\t")
                   "leave\n\t"
                   __ASM_CFI(".cfi_def_cfa %esp,4\n\t")
                   __ASM_CFI(".cfi_same_value %ebp\n\t")
                   "ret" )
#else
static void call_entry_point( void *func, HWND hwnd, HINSTANCE inst, void *cmdline, int show )
{
    void (WINAPI *entry_point)( HWND hwnd, HINSTANCE inst, void *cmdline, int show ) = func;
    entry_point( hwnd, inst, cmdline, show );
}
#endif

/*
 * Control_RunDLL needs to have a window. So lets make us a very
 * simple window class.
 */
static const WCHAR szTitle[] = {'r','u','n','d','l','l','3','2',0};
static const WCHAR szWindowClass[] = {'c','l','a','s','s','_','r','u','n','d','l','l','3','2',0};
static const WCHAR kernel32[] = {'k','e','r','n','e','l','3','2','.','d','l','l',0};
static const WCHAR shell32[] = {'s','h','e','l','l','3','2','.','d','l','l',0};

static HINSTANCE16 (WINAPI *pLoadLibrary16)(LPCSTR libname);
static FARPROC16 (WINAPI *pGetProcAddress16)(HMODULE16 hModule, LPCSTR name);
static void (WINAPI *pRunDLL_CallEntry16)( FARPROC proc, HWND hwnd, HINSTANCE inst,
                                           LPCSTR cmdline, INT cmdshow );

static ATOM register_class(void)
{
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEXW);

    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = DefWindowProcW;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = NULL;
    wcex.hIcon          = NULL;
    wcex.hCursor        = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wcex.hbrBackground  = (HBRUSH)(COLOR_WINDOW+1);
    wcex.lpszMenuName   = NULL;
    wcex.lpszClassName  = szWindowClass;
    wcex.hIconSm        = NULL;

    return RegisterClassExW(&wcex);
}

static HINSTANCE16 load_dll16( LPCWSTR dll )
{
    HINSTANCE16 ret = 0;
    DWORD len = WideCharToMultiByte( CP_ACP, 0, dll, -1, NULL, 0, NULL, NULL );
    char *dllA = HeapAlloc( GetProcessHeap(), 0, len );

    if (dllA)
    {
        WideCharToMultiByte( CP_ACP, 0, dll, -1, dllA, len, NULL, NULL );
        pLoadLibrary16 = (void *)GetProcAddress( GetModuleHandleW(kernel32), (LPCSTR)35 );
        if (pLoadLibrary16) ret = pLoadLibrary16( dllA );
        HeapFree( GetProcessHeap(), 0, dllA );
    }
    return ret;
}

static FARPROC16 get_entry_point16( HINSTANCE16 inst, LPCWSTR entry )
{
    FARPROC16 ret = 0;
    DWORD len = WideCharToMultiByte( CP_ACP, 0, entry, -1, NULL, 0, NULL, NULL );
    char *entryA = HeapAlloc( GetProcessHeap(), 0, len );

    if (entryA)
    {
        WideCharToMultiByte( CP_ACP, 0, entry, -1, entryA, len, NULL, NULL );
        pGetProcAddress16 = (void *)GetProcAddress( GetModuleHandleW(kernel32), (LPCSTR)37 );
        if (pGetProcAddress16) ret = pGetProcAddress16( inst, entryA );
        HeapFree( GetProcessHeap(), 0, entryA );
    }
    return ret;
}

static void *get_entry_point32( HMODULE module, LPCWSTR entry, BOOL *unicode )
{
    void *ret;

    /* determine if the entry point is an ordinal */
    if (entry[0] == '#')
    {
        INT_PTR ordinal = wcstol( entry + 1, NULL, 10 );
        if (ordinal <= 0)
            return NULL;

        *unicode = TRUE;
        ret = GetProcAddress( module, (LPCSTR)ordinal );
    }
    else
    {
        DWORD len = WideCharToMultiByte( CP_ACP, 0, entry, -1, NULL, 0, NULL, NULL );
        char *entryA = HeapAlloc( GetProcessHeap(), 0, len + 1 );

        if (!entryA)
            return NULL;

        WideCharToMultiByte( CP_ACP, 0, entry, -1, entryA, len, NULL, NULL );

        /* first try the W version */
        *unicode = TRUE;
        strcat( entryA, "W" );
        if (!(ret = GetProcAddress( module, entryA )))
        {
            /* now the A version */
            *unicode = FALSE;
            entryA[strlen(entryA)-1] = 'A';
            if (!(ret = GetProcAddress( module, entryA )))
            {
                /* now the version without suffix */
                entryA[strlen(entryA)-1] = 0;
                ret = GetProcAddress( module, entryA );
            }
        }
        HeapFree( GetProcessHeap(), 0, entryA );
    }
    return ret;
}

static LPWSTR get_next_arg(LPWSTR *cmdline, BOOL can_have_commas)
{
    LPWSTR s;
    LPWSTR arg,d;
    BOOL in_quotes;
    BOOL is_separator;
    int bcount,len=0;

    /* count the chars */
    bcount=0;
    in_quotes=FALSE;
    s=*cmdline;
    while (1) {
        is_separator = (*s=='\t' || *s==' ' || (*s==',' && !can_have_commas));
        if (*s==0 || (is_separator && !in_quotes)) {
            /* end of this command line argument */
            break;
        } else if (*s=='\\') {
            /* '\', count them */
            bcount++;
        } else if ((*s=='"') && ((bcount & 1)==0)) {
            /* unescaped '"' */
            in_quotes=!in_quotes;
            bcount=0;
        } else {
            /* a regular character */
            bcount=0;
        }
        s++;
        len++;
    }
    arg=HeapAlloc(GetProcessHeap(), 0, (len+1)*sizeof(WCHAR));
    if (!arg)
        return NULL;

    bcount=0;
    in_quotes=FALSE;
    d=arg;
    s=*cmdline;
    while (*s) {
        is_separator = (*s=='\t' || *s==' ' || (*s==',' && !can_have_commas));
        if (is_separator && !in_quotes) {
            /* end of this command line argument */
            break;
        } else if (*s=='\\') {
            /* '\\' */
            *d++=*s++;
            bcount++;
        } else if (*s=='"') {
            /* '"' */
            if ((bcount & 1)==0) {
                /* Preceded by an even number of '\', this is half that
                 * number of '\', plus a quote which we erase.
                 */
                d-=bcount/2;
                in_quotes=!in_quotes;
                s++;
            } else {
                /* Preceded by an odd number of '\', this is half that
                 * number of '\' followed by a '"'
                 */
                d=d-bcount/2-1;
                *d++='"';
                s++;
            }
            bcount=0;
        } else {
            /* a regular character */
            *d++=*s++;
            bcount=0;
        }
    }
    *d=0;
    *cmdline=s;

    /* skip the remaining spaces/commas */
    while (**cmdline=='\t' || **cmdline==' ' ||
            (**cmdline==',' && !can_have_commas)) {
        (*cmdline)++;
    }

    return arg;
}

static BOOL try_restart_process( LPWSTR szDllName )
{
    WCHAR path[MAX_PATH];
    PROCESS_INFORMATION pi;
    STARTUPINFOW si;
    PVOID ov;
    DWORD binary_type;
    BOOL is_wow64;

    if (!GetBinaryTypeW(szDllName, &binary_type))
        return FALSE;

    IsWow64Process( GetCurrentProcess(), &is_wow64 );

    switch (binary_type)
    {
        case SCS_32BIT_BINARY:
            if (is_wow64 || !GetSystemWow64DirectoryW( path, MAX_PATH - 1 - lstrlenW(SZ_RUNDLL32) ))
                return FALSE;
            WINE_TRACE("Restarting rundll32 process in 32-bit mode\n");
            break;
        case SCS_64BIT_BINARY:
            Wow64DisableWow64FsRedirection(&ov);
            if (!is_wow64 || !GetSystemDirectoryW( path, MAX_PATH - 1 - lstrlenW(SZ_RUNDLL32) ))
                return FALSE;
            WINE_TRACE("Restarting rundll32 process in 64-bit mode\n");
            break;
        default:
            return FALSE;
    }

    lstrcatW(path, SZ_RUNDLL32);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (CreateProcessW( path, GetCommandLineW(),
            NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi ))
    {
        WaitForSingleObject( pi.hProcess, INFINITE );
        CloseHandle( pi.hProcess );
        CloseHandle( pi.hThread );
    }
    else WINE_ERR("Failed to restart process (%s, err %u)\n",
            wine_dbgstr_w(path), GetLastError());

    return TRUE;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE hOldInstance, LPWSTR szCmdLine, int nCmdShow)
{
    HWND hWnd;
    LPWSTR szDllName,szEntryPoint;
    void *entry_point;
    BOOL unicode = FALSE, win16;
    STARTUPINFOW info;
    HMODULE hDll;

    hWnd=NULL;
    hDll=NULL;
    szDllName=NULL;

    /* Initialize the rundll32 class */
    register_class();
    hWnd = CreateWindowW(szWindowClass, szTitle,
          WS_OVERLAPPEDWINDOW|WS_VISIBLE,
          CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, NULL, NULL, NULL, NULL);

    /* Get the dll name and API EntryPoint */
    WINE_TRACE("CmdLine=%s\n",wine_dbgstr_w(szCmdLine));
    szDllName = get_next_arg(&szCmdLine, 0);
    if (!szDllName || *szDllName==0)
        goto CLEANUP;
    WINE_TRACE("DllName=%s\n",wine_dbgstr_w(szDllName));
    szEntryPoint = get_next_arg(&szCmdLine, 1);
    WINE_TRACE("EntryPoint=%s\n",wine_dbgstr_w(szEntryPoint));

    /* Load the library */
    hDll=LoadLibraryW(szDllName);
    if (hDll)
    {
        win16 = FALSE;
        entry_point = get_entry_point32( hDll, szEntryPoint, &unicode );
    }
    else
    {
        HINSTANCE16 dll = load_dll16( szDllName );
        if (dll <= 32)
        {
            if (!try_restart_process( szDllName ))
            {
                /* Windows has a MessageBox here... */
                WINE_ERR("Unable to load %s\n",wine_dbgstr_w(szDllName));
            }
            goto CLEANUP;
        }
        win16 = TRUE;
        unicode = FALSE;
        entry_point = get_entry_point16( dll, szEntryPoint );
    }

    if (!entry_point)
    {
        /* Windows has a MessageBox here... */
        WINE_ERR( "Unable to find the entry point %s in %s\n",
                  wine_dbgstr_w(szEntryPoint), wine_dbgstr_w(szDllName) );
        goto CLEANUP;
    }

    GetStartupInfoW( &info );
    if (!(info.dwFlags & STARTF_USESHOWWINDOW)) info.wShowWindow = SW_SHOWDEFAULT;

    if (unicode)
    {
        WINE_TRACE( "Calling %s (%p,%p,%s,%d)\n", wine_dbgstr_w(szEntryPoint),
                    hWnd, instance, wine_dbgstr_w(szCmdLine), info.wShowWindow );

        call_entry_point( entry_point, hWnd, instance, szCmdLine, info.wShowWindow );
    }
    else
    {
        DWORD len = WideCharToMultiByte( CP_ACP, 0, szCmdLine, -1, NULL, 0, NULL, NULL );
        char *cmdline = HeapAlloc( GetProcessHeap(), 0, len );

        if (!cmdline)
            goto CLEANUP;

        WideCharToMultiByte( CP_ACP, 0, szCmdLine, -1, cmdline, len, NULL, NULL );

        WINE_TRACE( "Calling %s (%p,%p,%s,%d)\n", wine_dbgstr_w(szEntryPoint),
                    hWnd, instance, wine_dbgstr_a(cmdline), info.wShowWindow );

        if (win16)
        {
            HMODULE shell = LoadLibraryW( shell32 );
            if (shell) pRunDLL_CallEntry16 = (void *)GetProcAddress( shell, (LPCSTR)122 );
            if (pRunDLL_CallEntry16)
                pRunDLL_CallEntry16( entry_point, hWnd, instance, cmdline, info.wShowWindow );
        }
        else call_entry_point( entry_point, hWnd, instance, cmdline, info.wShowWindow );

        HeapFree( GetProcessHeap(), 0, cmdline );
    }

CLEANUP:
    if (hWnd)
        DestroyWindow(hWnd);
    if (hDll)
        FreeLibrary(hDll);
    HeapFree(GetProcessHeap(),0,szDllName);
    return 0; /* rundll32 always returns 0! */
}
