/*
 * uihost_stub.dll — minimal in-process COM server providing a no-op
 * implementation of CLSID_UIHostNoLaunch (Windows touch-keyboard host)
 * and IID_ITipInvocation (its Toggle() interface).
 *
 * Why: Some Windows VST plugins (e.g. Mercuriall X50II) call
 *   ITipInvocation* tip;
 *   CoCreateInstance(CLSID_UIHostNoLaunch, NULL, CLSCTX_LOCAL_SERVER,
 *                    IID_ITipInvocation, (void**)&tip);
 *   tip->Toggle(hwnd);
 * without checking the HRESULT. Wine has no implementation of this
 * class so the call fails, tip stays NULL, the next vtbl deref
 * segfaults effEditOpen, the wine debugger attaches, and the editor
 * thread hangs. With this stub registered, CoCreateInstance returns
 * S_OK with a real (no-op) object → no crash → editor proceeds.
 *
 * Build via scripts/build-uihost-stub.sh (both arches; x64 + x86).
 */

#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <objbase.h>

/* CLSIDs / IIDs we own here (DEFINE_GUID + INITGUID = real symbols).
 *
 * CLSID_UIHostNoLaunch       = {4CE576FA-83DC-4F88-951C-9D0782B4E376}
 * IID_ITipInvocation         = {37C994E7-432B-4834-A2F7-DCE1F13B834B}
 * CLSID_VirtualDesktopManager= {AA509086-5CA9-4C25-8F95-589D3C07B48A}
 * IID_IVirtualDesktopManager = {A5CD92FF-29BE-454C-8D04-D82879FB3F1B}
 *
 * VirtualDesktopManager came in Windows 10 (twinapi.dll) and modern
 * JUCE-based plugins (AmpCraft, some Neural DSP) query it to detect
 * which virtual desktop their top-level window lives on. Wine has no
 * twinapi.dll; without a stub, CoCreateInstance returns CLASS_NOT_REG
 * and plugins that don't check the HRESULT SIGSEGV on the NULL out
 * pointer's vtable offset 0x10 (Release slot). */
DEFINE_GUID(CLSID_UIHostNoLaunch,
    0x4ce576fa, 0x83dc, 0x4f88,
    0x95, 0x1c, 0x9d, 0x07, 0x82, 0xb4, 0xe3, 0x76);
DEFINE_GUID(IID_ITipInvocation,
    0x37c994e7, 0x432b, 0x4834,
    0xa2, 0xf7, 0xdc, 0xe1, 0xf1, 0x3b, 0x83, 0x4b);
DEFINE_GUID(CLSID_VirtualDesktopManager,
    0xaa509086, 0x5ca9, 0x4c25,
    0x8f, 0x95, 0x58, 0x9d, 0x3c, 0x07, 0xb4, 0x8a);
DEFINE_GUID(IID_IVirtualDesktopManager,
    0xa5cd92ff, 0x29be, 0x454c,
    0x8d, 0x04, 0xd8, 0x28, 0x79, 0xfb, 0x3f, 0x1b);

typedef struct ITipInvocation ITipInvocation;
typedef struct ITipInvocationVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ITipInvocation*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ITipInvocation*);
    ULONG   (STDMETHODCALLTYPE *Release)(ITipInvocation*);
    HRESULT (STDMETHODCALLTYPE *Toggle)(ITipInvocation*, HWND);
} ITipInvocationVtbl;
struct ITipInvocation {
    const ITipInvocationVtbl* lpVtbl;
};

/* --- ITipInvocation singleton --- */

static HRESULT STDMETHODCALLTYPE tip_QueryInterface(ITipInvocation* self,
                                                    REFIID iid, void** out) {
    if (!out) return E_POINTER;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_ITipInvocation)) {
        *out = self;
        self->lpVtbl->AddRef(self);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE tip_AddRef(ITipInvocation* self)  { (void)self; return 2; }
static ULONG STDMETHODCALLTYPE tip_Release(ITipInvocation* self) { (void)self; return 1; }
static HRESULT STDMETHODCALLTYPE tip_Toggle(ITipInvocation* self, HWND hwnd) {
    /* No-op. The plugin asked the OS to show/hide its touch keyboard;
     * we have no real touch keyboard. Returning S_OK lets the plugin
     * proceed. Keyboard input forwarding from Android is handled
     * separately via X11 KeyPress injection. */
    (void)self; (void)hwnd;
    return S_OK;
}
static const ITipInvocationVtbl g_tip_vtbl = {
    tip_QueryInterface, tip_AddRef, tip_Release, tip_Toggle,
};
static ITipInvocation g_tip = { &g_tip_vtbl };

/* --- IVirtualDesktopManager singleton --- */

typedef struct IVirtualDesktopManager IVirtualDesktopManager;
typedef struct IVirtualDesktopManagerVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IVirtualDesktopManager*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IVirtualDesktopManager*);
    ULONG   (STDMETHODCALLTYPE *Release)(IVirtualDesktopManager*);
    HRESULT (STDMETHODCALLTYPE *IsWindowOnCurrentVirtualDesktop)(IVirtualDesktopManager*, HWND, BOOL*);
    HRESULT (STDMETHODCALLTYPE *GetWindowDesktopId)(IVirtualDesktopManager*, HWND, GUID*);
    HRESULT (STDMETHODCALLTYPE *MoveWindowToDesktop)(IVirtualDesktopManager*, HWND, REFGUID);
} IVirtualDesktopManagerVtbl;
struct IVirtualDesktopManager {
    const IVirtualDesktopManagerVtbl* lpVtbl;
};

static HRESULT STDMETHODCALLTYPE vdm_QueryInterface(IVirtualDesktopManager* self,
                                                    REFIID iid, void** out) {
    if (!out) return E_POINTER;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IVirtualDesktopManager)) {
        *out = self;
        self->lpVtbl->AddRef(self);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE vdm_AddRef(IVirtualDesktopManager* self)  { (void)self; return 2; }
static ULONG STDMETHODCALLTYPE vdm_Release(IVirtualDesktopManager* self) { (void)self; return 1; }
static HRESULT STDMETHODCALLTYPE vdm_IsWindowOnCurrentVirtualDesktop(
        IVirtualDesktopManager* self, HWND hwnd, BOOL* result) {
    /* Single-desktop assumption is correct for wine — there are no
     * Windows-style virtual desktops here. Tell the caller every window
     * is on the current desktop. */
    (void)self; (void)hwnd;
    if (!result) return E_POINTER;
    *result = TRUE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE vdm_GetWindowDesktopId(
        IVirtualDesktopManager* self, HWND hwnd, GUID* desktopId) {
    /* Return a zero GUID — same desktop for all windows. */
    (void)self; (void)hwnd;
    if (!desktopId) return E_POINTER;
    memset(desktopId, 0, sizeof(*desktopId));
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE vdm_MoveWindowToDesktop(
        IVirtualDesktopManager* self, HWND hwnd, REFGUID desktopId) {
    /* No-op — we don't have virtual desktops. */
    (void)self; (void)hwnd; (void)desktopId;
    return S_OK;
}
static const IVirtualDesktopManagerVtbl g_vdm_vtbl = {
    vdm_QueryInterface, vdm_AddRef, vdm_Release,
    vdm_IsWindowOnCurrentVirtualDesktop,
    vdm_GetWindowDesktopId,
    vdm_MoveWindowToDesktop,
};
static IVirtualDesktopManager g_vdm = { &g_vdm_vtbl };

/* --- IClassFactory for CLSID_UIHostNoLaunch --- */

static HRESULT STDMETHODCALLTYPE cf_QueryInterface(IClassFactory* self,
                                                    REFIID iid, void** out) {
    if (!out) return E_POINTER;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IClassFactory)) {
        *out = self;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE cf_AddRef(IClassFactory* self)  { (void)self; return 2; }
static ULONG STDMETHODCALLTYPE cf_Release(IClassFactory* self) { (void)self; return 1; }
/* Per-class CreateInstance routers. Wine's DllGetClassObject picks the
 * right factory based on the CLSID so each factory only knows about its
 * own interface set. */
static HRESULT STDMETHODCALLTYPE cf_uihost_CreateInstance(IClassFactory* self,
                                                    IUnknown* outer,
                                                    REFIID iid, void** out) {
    (void)self;
    if (!out) return E_POINTER;
    if (outer) { *out = NULL; return CLASS_E_NOAGGREGATION; }
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_ITipInvocation)) {
        *out = &g_tip;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE cf_vdm_CreateInstance(IClassFactory* self,
                                                    IUnknown* outer,
                                                    REFIID iid, void** out) {
    (void)self;
    if (!out) return E_POINTER;
    if (outer) { *out = NULL; return CLASS_E_NOAGGREGATION; }
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IVirtualDesktopManager)) {
        *out = &g_vdm;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static HRESULT STDMETHODCALLTYPE cf_LockServer(IClassFactory* self, BOOL lock) {
    (void)self; (void)lock;
    return S_OK;
}
static const IClassFactoryVtbl g_cf_uihost_vtbl = {
    cf_QueryInterface, cf_AddRef, cf_Release,
    cf_uihost_CreateInstance, cf_LockServer,
};
static IClassFactory g_cf_uihost = { &g_cf_uihost_vtbl };

static const IClassFactoryVtbl g_cf_vdm_vtbl = {
    cf_QueryInterface, cf_AddRef, cf_Release,
    cf_vdm_CreateInstance, cf_LockServer,
};
static IClassFactory g_cf_vdm = { &g_cf_vdm_vtbl };

/* --- DLL exports --- */

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    if (IsEqualCLSID(clsid, &CLSID_UIHostNoLaunch))
        return g_cf_uihost.lpVtbl->QueryInterface(&g_cf_uihost, iid, out);
    if (IsEqualCLSID(clsid, &CLSID_VirtualDesktopManager))
        return g_cf_vdm.lpVtbl->QueryInterface(&g_cf_vdm, iid, out);
    *out = NULL;
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllCanUnloadNow(void) {
    /* Static singletons — never unload. */
    return S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)hinst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(hinst);
    return TRUE;
}
