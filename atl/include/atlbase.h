/*
 * Combined ATL base header for standalone (non-Windows) builds.
 *
 * Type definitions derived from Wine ATL (wine/include/atlbase.h):
 *   Copyright 2004 Aric Stewart for CodeWeavers
 *   LGPL-2.1-or-later
 *
 * C++ class stubs derived from ReactOS ATL:
 *   https://github.com/reactos/reactos/blob/master/sdk/lib/atl/atlbase.h
 */

#pragma once

extern "C" {

typedef HRESULT (WINAPI _ATL_CREATORARGFUNC)(void* pv, REFIID riid, LPVOID* ppv, DWORD_PTR dw);

typedef struct _ATL_INTMAP_ENTRY_TAG
{
    const IID* piid;
    DWORD_PTR dw;
    _ATL_CREATORARGFUNC* pFunc;
} _ATL_INTMAP_ENTRY;

HRESULT WINAPI AtlInternalQueryInterface(void* pThis, const _ATL_INTMAP_ENTRY* pEntries, REFIID iid, void** ppvObject);

}

#define _ATL_SIMPLEMAPENTRY ((_ATL_CREATORARGFUNC *)1)

class CAtlModule {
public:
  static GUID m_libid;
};

class CComMultiThreadModel { };
class CComSingleThreadModel { };
