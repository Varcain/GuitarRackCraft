/*
 * vst_host.exe — runs under Wine inside the Android app's chroot.
 *
 * Loads a VST2 plugin DLL via the real Win32 API, drives it through
 * VSTPluginMain → AEffect, and pumps audio over a shared-memory ring with
 * the Android side. Replaces the no-libc PE loader (external/vst_guest)
 * for the general-purpose plugin path.
 *
 * Argv:
 *   argv[1] = path to the shared-memory file (already created by Android)
 *   argv[2] = path to the plugin DLL (Windows-side path; the file is
 *             reachable through Wine's drive_c bind by default)
 *
 * Build:
 *   x86_64-w64-mingw32-gcc -O2 -static -o vst_host.exe \
 *       -I../vst2 -I.. \
 *       vst_host.c -lkernel32
 *
 * Why mingw-w64: this binary needs to be a real Windows PE so Wine treats
 * it as a Windows program. Its argv[0] will be /opt/wine/bin/wine and the
 * actual .exe path comes via Z:\ drive mapping for Linux file paths.
 */

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#include "vst2.h"
#include "shared_layout.h"

/* Vectored exception handler + setjmp/longjmp form a more robust crash
 * recovery path than __try/__except across the FEX-Emu ARM64EC ↔ x86_64
 * boundary: __C_specific_handler doesn't seem to honour the filter's
 * EXCEPTION_EXECUTE_HANDLER return when the exception is raised inside
 * FEX-translated code (wine's seh trace shows the filter result of "1"
 * being interpreted as ExceptionContinueSearch instead of executing the
 * handler body). The vectored path runs before any SEH frame walking,
 * letting us longjmp out cleanly. */
static volatile LONG g_pluginmain_jmp_armed = 0;
static jmp_buf g_pluginmain_jmp;
static volatile unsigned long g_pluginmain_fault_code;
static volatile uintptr_t     g_pluginmain_fault_addr;

static LONG WINAPI pluginmain_veh(PEXCEPTION_POINTERS info) {
    DWORD code = info->ExceptionRecord->ExceptionCode;

    /* Load-time bracketed catch (longjmp recovery during VSTPluginMain). */
    if (g_pluginmain_jmp_armed) {
        /* Only catch fatal access-class exceptions during the bracketed
         * VSTPluginMain call; everything else (debug breaks, etc.) falls
         * through to normal handling. */
        if (code != EXCEPTION_ACCESS_VIOLATION &&
            code != EXCEPTION_ILLEGAL_INSTRUCTION &&
            code != EXCEPTION_PRIV_INSTRUCTION &&
            code != EXCEPTION_STACK_OVERFLOW &&
            code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
            code != 0xc06d007e /* DELAY_LOAD_FAILURE */) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        g_pluginmain_fault_code = code;
        g_pluginmain_fault_addr = (uintptr_t)info->ExceptionRecord->ExceptionAddress;
        g_pluginmain_jmp_armed = 0;
        longjmp(g_pluginmain_jmp, 1);
        /* unreachable */
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    /* vstpoc 2026-05-24: runtime NULL-deref catch (TH-U dismiss UAF and
     * similar). TH-U's drag-drop dismiss code at +0x2DF0C3 reads through
     * a NULL pointer at offset 0x10 when wine's set_focus completes
     * asynchronously (patch 027) before TH-U's state machine finishes —
     * a pre-existing TH-U bug that also races on real Windows but rarely
     * hits there. Without intervention, wine's unhandled-exception path
     * spawns winedbg → hangs the host forever on Android (no debugger to
     * spawn). We swallow the read by terminating the offending thread:
     * other plugin threads survive, audio recovers, user reloads plugin
     * to recover the editor.
     *
     * Tight signature: AccessViolation, READ access, faulting address
     * inside the first page (NULL + small offset). Matches the TH-U bug
     * exactly; any unrelated NULL-deref bug elsewhere matches too — which
     * is fine, the alternative is the whole host hanging.
     *
     * NOTE: leaks any thread-held lock or resource. Watchdog will catch
     * any secondary deadlock from orphaned locks. Acceptable trade vs
     * winedbg hang. */
    if (code == EXCEPTION_ACCESS_VIOLATION
        && info->ExceptionRecord->NumberParameters >= 2
        && info->ExceptionRecord->ExceptionInformation[0] == 0       /* 0 = read, 1 = write */
        && info->ExceptionRecord->ExceptionInformation[1] < 0x1000)  /* NULL + low offset */
    {
        /* vstpoc 2026-05-25: surgical recovery from TH-U-64.dll+0x2DF0C3
         * vector race. Disassembly shows a "pop_back" pattern where r15
         * holds vector* {data@+0, count@+0xc}; the crashing load is
         *     1802df0c3: 4c 8b 34 07   mov r14, [rdi+rax*1]
         * with rax (= vec->data) loaded as NULL despite count > 0 — a
         * std::vector move/swap race that even strict FEX TSO can't
         * close. Killing the thread destroys the editor → UI dead.
         *
         * Naive recovery (skip just the load, set r14=0) was unsafe:
         * the following block re-reads the racy count and calls an AVX
         * memcpy whose size param can be NON-zero by the time of the
         * second read; the memcpy then crashes on the NULL src+0x18
         * (vmovdqu ymm0, [rdx] at +0xc53f9d). So we skip the ENTIRE
         * block — load + 4 setup instructions + the call — landing
         * at +0x2DF0E3:
         *     1802df0e3: ff 4f 0c       dec DWORD PTR [r15+0xc]   (decrement count)
         *     1802df0e7: 4d 85 f6       test r14, r14
         *     1802df0ea: 74 0d          je 0x1802df0f9            (taken, r14=0)
         *     1802df0f9: ... function epilogue
         *
         * Net effect: count decrements by 1 (consistent with "removed
         * one element"), no element actually destructed (data was NULL
         * so none to destruct), function returns clean, editor stays
         * alive. Validated by reading the expected 4 bytes at the
         * crash site to avoid false positives. */
#ifdef _WIN64
        if (info->ExceptionRecord->ExceptionAddress == (PVOID)0x1802DF0C3ULL) {
            static const unsigned char kExpectedBytes[4] = { 0x4c, 0x8b, 0x34, 0x07 };
            if (memcmp((const void*)0x1802DF0C3ULL, kExpectedBytes, 4) == 0) {
                info->ContextRecord->R14 = 0;
                info->ContextRecord->Rip = 0x1802DF0E3ULL;
                fprintf(stderr,
                        "[vst_host] vstpoc 2026-05-25 VEH: surgical skip of "
                        "TH-U +0x2DF0C3 vector-race block — r14=0, rip→+0x2DF0E3 "
                        "(skipped load+setup+memcpy call), thread CONTINUES\n");
                fflush(stderr);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }

        /* vstpoc 2026-05-25: effect-removal popup crash at TH-U+0x2A05AF.
         * Disassembly:
         *     1802a05a5: mov ecx, ebx              ; loop counter
         *     1802a05a7: call 0x1807ba140           ; get_item(index)
         *     1802a05ac: mov rcx, rax              ; rcx = item
         *     1802a05af: mov rdx, [rax]            ; CRASH: rax=NULL (item missing)
         *     1802a05b2: call [rdx+0xd0]            ; would tail-call vtable method
         *     1802a05b8: test al, al
         *     1802a05ba: jne 0x1802a0603             ; jump if found
         *     1802a05bc: sub ebx, 0x1
         *     1802a05bf: jns 0x1802a05a5             ; loop back
         *
         * Race: get_item(index) returns NULL despite ebx within bounds
         * (collection mutated by another thread between count-read and
         * get_item call). Recovery: set rax=0 (so test al,al == 0), skip
         * the 3-byte load + 6-byte call = 9 bytes; land at test al,al.
         * Loop continues to next iteration, missing this item — same
         * effect as if the item really didn't exist. */
        if (info->ExceptionRecord->ExceptionAddress == (PVOID)0x1802A05AFULL) {
            static const unsigned char kExpectedBytes[3] = { 0x48, 0x8b, 0x10 };
            if (memcmp((const void*)0x1802A05AFULL, kExpectedBytes, 3) == 0) {
                info->ContextRecord->Rax = 0;
                info->ContextRecord->Rip = 0x1802A05B8ULL;
                fprintf(stderr,
                        "[vst_host] vstpoc 2026-05-25 VEH: surgical skip of "
                        "TH-U +0x2A05AF effect-removal race — rax=0, "
                        "rip→+0x2A05B8 (skip load+vtable-call, continue loop)\n");
                fflush(stderr);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
#endif /* _WIN64 */

        /* vstpoc 2026-05-25: drag-drop crash at TH-U+0x28ECAC was previously
         * attempted via epilogue-replay + RET-to-caller (skip the vtable
         * tail call). That worked mechanically (no thread death) but the
         * caller depended on the tail call's side effects — without them,
         * TH-U state went inconsistent and later operations cascaded into
         * EXCEPTION_ILLEGAL_INSTRUCTION at TH-U+0x609E8D, AVs in wine
         * code, and "drags that didn't drop". So drag-drop falls back to
         * the thread-kill path below: editor dies but the host stays up.
         * Cleaner than corrupting TH-U's state via half-finished tail calls.
         * Revisit only with a deeper understanding of what the tail-called
         * vtable method does. */

        fprintf(stderr,
                "[vst_host] vstpoc 2026-05-24 VEH: catching NULL-deref AV "
                "pc=%p fault_addr=0x%llx (read) — terminating thread to keep host "
                "alive (TH-U dismiss UAF or similar plugin bug)\n",
                info->ExceptionRecord->ExceptionAddress,
                (unsigned long long)info->ExceptionRecord->ExceptionInformation[1]);
        fflush(stderr);
        ExitThread(0xDEADBEEF);
        /* unreachable */
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

#define LOG(...) do { fprintf(stderr, "[vst_host] " __VA_ARGS__); fflush(stderr); } while (0)

/* Set load_status + status_message on the shared region and flush so the
 * host can pick it up even if we exit shortly after. Safe to call with
 * shm==NULL (no-op). msg may be NULL (writes empty). */
static void write_status(VstpocShared* shm, int code, const char* msg) {
    if (!shm) return;
    if (msg) {
        size_t n = strlen(msg);
        if (n >= sizeof(shm->status_message)) n = sizeof(shm->status_message) - 1;
        memcpy((void*)shm->status_message, msg, n);
        shm->status_message[n] = '\0';
    } else {
        shm->status_message[0] = '\0';
    }
    __sync_synchronize();
    shm->load_status = code;
    __sync_synchronize();
}

/* MS-ABI VSTPluginMain entry signature. */
typedef AEffect* (VST_CALL *VstPluginMainFn)(AudioMasterCallback);

/* Multi-plugin chain support. We load N plugins from argv[2..argc-1],
 * chain their processReplacing in sequence (output of plugin K feeds
 * input of plugin K+1), and run one editor thread for each. Audio
 * scratch buffers ping-pong between two pairs to avoid in-place
 * surprises with plugins that don't tolerate src==dst. */
#define VSTHOST_MAX_PLUGINS 8

typedef struct {
    HMODULE  module;
    AEffect* eff;
    const char* path;   /* points into argv */
} PluginEntry;

static PluginEntry g_plugins[VSTHOST_MAX_PLUGINS];
static int g_pluginCount = 0;

/* Globals shared between audio loop and editor thread. The "g_eff" is
 * the FIRST plugin — that's the one whose editor we open first and
 * whose params we publish to Android over the shared-memory ring.
 * Subsequent commits will extend the param ring to route per-plugin. */
static volatile VstpocShared* g_shm = NULL;
static AEffect* g_eff = NULL;

/* Set to 1 by the editor thread once effEditOpen has returned. The audio
 * thread blocks on this before starting to call processReplacing, so the
 * two threads don't fight over the user32 critical section while the
 * editor is mid-CreateWindowExA. The wine user32 CS deadlock observed in
 * the C++ X server path is caused by this exact contention. */
static volatile LONG g_editor_open_done = 0;

/* Sequential editor open: each per-plugin editor thread waits until
 * g_editor_open_index == its plugin index before calling effEditOpen,
 * then bumps the counter. Wine's user32 CreateWindowExA deadlocks if
 * two windows are being created concurrently from different threads;
 * serializing the opens prevents that while still letting each plugin
 * have its own long-running message loop afterwards. */
static volatile LONG g_editor_open_index = 0;

/* vstpoc option-2 prototype (X50II WM_USER storm): VSTPOC_LOAD_ON_EDITOR_THREAD=1
 * makes each per-plugin editor thread ALSO LoadLibrary + VSTPluginMain its
 * plugin, instead of main() loading them all up front. JUCE binds its
 * MessageManager to the thread that initialises it (inside VSTPluginMain) —
 * with main-thread loading, every editor interaction (menus, shadows, async
 * callbacks) becomes a cross-thread send between the editor thread (mouse,
 * ShowWindow) and main (MessageManager, window ownership), which is the
 * substrate of the X50II menu self-feed storm. Loading on the editor thread
 * puts the MessageManager, the window ownership AND the message pump on ONE
 * thread, like a normal single-threaded VST2 host. Audio processReplacing
 * from main stays cross-thread — that's the standard host arrangement.
 * Loads are still serialized (one thread at a time) because the
 * VSTPluginMain crash-recovery jmp_buf is a single global.
 * See memory feedback_x50_stomp_popup_grab_dismiss. */
static int g_load_on_editor_thread = 0;
static const char* g_paths[VSTHOST_MAX_PLUGINS];
static volatile LONG g_load_result[VSTHOST_MAX_PLUGINS]; /* 0=pending 1=ok 2=failed */
static volatile LONG g_editor_go = 0;  /* main: "editors may open now" */
static char g_failure_msg[256];
static int  g_failure_set = 0;
#define RECORD_FAILURE(fmt, ...) do { \
    if (!g_failure_set) { \
        snprintf(g_failure_msg, sizeof(g_failure_msg), fmt, ##__VA_ARGS__); \
        g_failure_set = 1; \
    } \
} while (0)
static int load_one_plugin(int slot, const char* dll_path);
static void publish_param_values(VstpocShared* shm, AEffect* eff);

/* Host-frame window procedure. Used ONLY when VSTPOC_HOST_FRAME=1 (the
 * PC launcher sets this; Android leaves it unset). In that mode each
 * plugin editor is reparented under a top-level frame window so wine's
 * X11 driver decorates it with a title bar + close/min/max buttons.
 *
 * WM_CLOSE on any frame signals stop_flag so the whole host tears down
 * — that gives the user a working "X" button to end the session. */
static LRESULT CALLBACK editor_wndproc(HWND hwnd, UINT msg,
                                       WPARAM wp, LPARAM lp) {
    /* Trace interesting frame-level messages — silenced by default; flip
     * the #if to 1 when debugging close-button routing through wine X11. */
#if 0
    if (msg == WM_CLOSE || msg == WM_DESTROY || msg == WM_SYSCOMMAND
        || msg == WM_NCDESTROY) {
        LOG("wndproc hwnd=%p msg=0x%04x wp=0x%llx lp=0x%llx\n",
            hwnd, msg, (unsigned long long)wp, (unsigned long long)lp);
    }
#endif
    switch (msg) {
        case WM_CLOSE:
            if (g_shm) {
                __atomic_store_n((uint64_t*)&g_shm->stop_flag, 1, __ATOMIC_RELEASE);
            }
            DestroyWindow(hwnd);
            return 0;
        /* WM_SYSCOMMAND with SC_CLOSE arrives when the user picks
         * "Close" from the system menu / Alt+F4. DefWindowProc on
         * SC_CLOSE posts WM_CLOSE, so technically we'd catch it via
         * the WM_CLOSE branch — but some window managers (xdotool
         * windowclose included) skip the SC_CLOSE step and send the
         * WM_DELETE_WINDOW ClientMessage straight at the X11 window;
         * wine translates THAT into a system-tray-style request that
         * goes through SC_CLOSE. Handle both shapes. */
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_CLOSE) {
                if (g_shm) {
                    __atomic_store_n((uint64_t*)&g_shm->stop_flag, 1, __ATOMIC_RELEASE);
                }
                DestroyWindow(hwnd);
                return 0;
            }
            return DefWindowProcA(hwnd, msg, wp, lp);
        case WM_DESTROY:
            return 0;
        default:
            return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

/* Idempotent registration of the host-frame window class. Safe to call
 * from multiple editor threads — the first registration wins; subsequent
 * RegisterClassExA calls fail with ERROR_CLASS_ALREADY_EXISTS, which we
 * ignore. */
#define VSTPOC_HOST_FRAME_CLASS "vstpoc_host_frame"
static void ensure_host_frame_class_registered(void) {
    static volatile LONG done = 0;
    if (InterlockedCompareExchange(&done, 1, 0) != 0) return;
    WNDCLASSEXA wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = editor_wndproc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = VSTPOC_HOST_FRAME_CLASS;
    if (!RegisterClassExA(&wc)) {
        DWORD e = GetLastError();
        if (e != ERROR_CLASS_ALREADY_EXISTS) {
            LOG("RegisterClassExA(host_frame) failed: %lu\n", (unsigned long)e);
        }
    }
}

/* Per-plugin editor thread. Each plugin gets its own thread so they
 * don't share a single message loop: a plugin that blocks inside
 * CreateWindowExA can't stall the others.
 *
 * arg = (intptr_t) plugin index in g_plugins[]. */
static DWORD WINAPI per_plugin_editor_thread(LPVOID arg) {
    int p = (int)(intptr_t)arg;
    if (p < 0 || p >= g_pluginCount) return 0;
    if (g_load_on_editor_thread) {
        /* Option-2 mode: load HERE so JUCE binds its MessageManager to THIS
         * thread — the one that opens the editor and pumps its messages.
         * main() waits on g_load_result before spawning the next loader
         * (single global jmp_buf for VSTPluginMain crash recovery), then
         * releases all editors at once via g_editor_go. */
        int ok = load_one_plugin(p, g_paths[p]);
        InterlockedExchange(&g_load_result[p], ok ? 1 : 2);
        if (!ok || !g_plugins[p].eff) {
            /* Bump the open-turn counter so later editors don't wait 60s
             * for a plugin that never loaded. */
            InterlockedExchange(&g_editor_open_index, p + 1);
            return 0;
        }
        while (!g_editor_go && !(g_shm && g_shm->stop_flag)) Sleep(5);
        if (g_shm && g_shm->stop_flag) return 0;
    }
    AEffect* eff = g_plugins[p].eff;
    if (!eff) return 0;

    if (!(eff->flags & effFlagsHasEditor)) {
        LOG("editor[%d]: plugin has no editor\n", p);
        return 0;
    }
    ERect* rect = NULL;
    eff->dispatcher(eff, /*effEditGetRect=*/13, 0, 0, &rect, 0.0f);
    if (!rect) {
        LOG("editor[%d]: effEditGetRect returned NULL\n", p);
        return 0;
    }
    int w = rect->right - rect->left;
    int h = rect->bottom - rect->top;
    /* Publish the editor's native size so the Android side can size its
     * SurfaceView to match. Only the primary plugin is exposed today.
     * Written under the same memory barrier as guest_ready. */
    if (p == 0 && g_shm) {
        g_shm->editor_width  = w;
        g_shm->editor_height = h;
        __sync_synchronize();
    }
    /* Wait my turn — previous plugins must finish CreateWindowExA before
     * we start ours, or wine deadlocks. */
    LOG("editor[%d]: waiting for turn (current open_index=%ld)\n",
        p, (long)g_editor_open_index);
    {
        int waited_ms = 0;
        while (g_editor_open_index < p && !(g_shm && g_shm->stop_flag)) {
            Sleep(10);
            waited_ms += 10;
            if (waited_ms > 60000) {
                LOG("editor[%d]: timed out waiting for editor[%ld]\n",
                    p, (long)g_editor_open_index);
                return 0;
            }
        }
    }

    LOG("editor[%d]: pre-GetDesktopWindow (size %dx%d)\n", p, w, h);
    HWND desktop = GetDesktopWindow();
    LOG("editor[%d]: post-GetDesktopWindow hwnd=%p\n", p, desktop);

    /* Optionally wrap the editor in a top-level host frame so it becomes a
     * WS_CHILD of a real top-level window instead of WS_CHILD-of-desktop.
     *   VSTPOC_HOST_FRAME=1      -> PC: decorated frame (title bar + borders).
     *   VSTPOC_HOST_FRAME=popup  -> Android: chromeless WS_POPUP frame, client
     *     area == editor size (no non-client inflation), at (0,0). The editor
     *     then activates natively (it is no longer a top-level WS_CHILD of the
     *     desktop), so wine patches 0037-0040 become unnecessary, and the 1:1
     *     surface/touch mapping in X11NativeDisplay is unchanged because the
     *     frame is exactly the editor size.
     * Off by default; the PC launcher (vstpoc_pc) sets =1. */
    HWND parent_hwnd = desktop;
    HWND frame_hwnd  = NULL;
    {
        const char* env = getenv("VSTPOC_HOST_FRAME");
        int use_frame   = (env && *env && *env != '0');
        int popup_mode  = (env && strcmp(env, "popup") == 0);
        if (use_frame) {
            ensure_host_frame_class_registered();
            DWORD style;
            int frame_w, frame_h, x, y;
            if (popup_mode) {
                /* Chromeless top-level window; client area == editor size. */
                style   = WS_POPUP;
                frame_w = w;
                frame_h = h;
                x = 0;
                y = 0;
            } else {
                /* AdjustWindowRect inflates a desired client rect (the plugin's
                 * effEditGetRect size) by the non-client area (borders + title
                 * bar) so the editor lands at (0,0) inside the client and isn't
                 * clipped or letterboxed. */
                RECT cr = { 0, 0, w, h };
                style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU
                      | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
                AdjustWindowRect(&cr, style, FALSE);
                frame_w = cr.right - cr.left;
                frame_h = cr.bottom - cr.top;
                /* Cascade frames so multi-plugin chains don't stack at the same
                 * point. CW_USEDEFAULT works on the first window; for subsequent
                 * ones we offset by ~30 px per plugin. */
                x = (p == 0) ? CW_USEDEFAULT : 60 + p * 30;
                y = (p == 0) ? CW_USEDEFAULT : 60 + p * 30;
            }

            char title[160];
            char eff_name[64] = {0};
            eff->dispatcher(eff, /*effGetEffectName=*/45, 0, 0, eff_name, 0.0f);
            if (eff_name[0]) {
                snprintf(title, sizeof(title), "%s — vst_host", eff_name);
            } else {
                snprintf(title, sizeof(title), "VST plugin %d — vst_host", p);
            }

            frame_hwnd = CreateWindowExA(
                0, VSTPOC_HOST_FRAME_CLASS, title, style,
                x, y, frame_w, frame_h,
                NULL, NULL, GetModuleHandleA(NULL), NULL);
            if (frame_hwnd) {
                ShowWindow(frame_hwnd, SW_SHOW);
                UpdateWindow(frame_hwnd);
                parent_hwnd = frame_hwnd;
                LOG("editor[%d]: host frame hwnd=%p (%dx%d) %s\n",
                    p, frame_hwnd, frame_w, frame_h,
                    popup_mode ? "popup" : "decorated");
            } else {
                LOG("editor[%d]: CreateWindowExA(host_frame) failed: %lu — "
                    "falling back to desktop parent\n",
                    p, (unsigned long)GetLastError());
            }
        }
    }

    LOG("editor[%d]: pre-effEditOpen parent=%p\n", p, parent_hwnd);
    intptr_t openRet = eff->dispatcher(eff, /*effEditOpen=*/14, 0, 0, parent_hwnd, 0.0f);
    LOG("editor[%d]: post-effEditOpen ret=%lld; entering message loop\n",
        p, (long long)openRet);

    /* Signal that this editor is done opening so editor[p+1] can start. */
    InterlockedExchange(&g_editor_open_index, p + 1);

    /* The audio thread gates on the FIRST plugin's editor being open.
     * Once any plugin's effEditOpen returns, the wine user32 CS is no
     * longer held, so processReplacing can run. */
    if (p == 0) InterlockedExchange(&g_editor_open_done, 1);

    DWORD last_idle = GetTickCount();
    DWORD last_param_publish = 0;
    MSG msg;
    while (!(g_shm && g_shm->stop_flag)) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        /* Param drain: routes to the PRIMARY plugin only for now.
         * (Per-plugin param routing lands with the shared-layout change.) */
        if (p == 0 && g_shm) {
            uint64_t ph = __atomic_load_n(&g_shm->param_head, __ATOMIC_ACQUIRE);
            uint64_t pt = __atomic_load_n(&g_shm->param_tail, __ATOMIC_RELAXED);
            while (pt != ph) {
                VstpocParamMsg pmsg = g_shm->params[pt & (VSTPOC_PARAM_RING_MSGS - 1)];
                if (pmsg.index >= 0 && pmsg.index < eff->numParams) {
                    eff->setParameter(eff, pmsg.index, pmsg.value);
                }
                pt++;
            }
            __atomic_store_n(&g_shm->param_tail, pt, __ATOMIC_RELEASE);
        }
        DWORD now = GetTickCount();
        if (p == 0 && g_shm && now - last_param_publish >= 100) {
            publish_param_values((VstpocShared*)g_shm, eff);
            last_param_publish = now;
        }
        if (now - last_idle >= 30) {
            eff->dispatcher(eff, /*effEditIdle=*/19, 0, 0, NULL, 0.0f);
            last_idle = now;
        }
        Sleep(5);
    }

    LOG("editor[%d]: closing\n", p);
    eff->dispatcher(eff, /*effEditClose=*/15, 0, 0, NULL, 0.0f);
    /* If we own a frame and it's still alive (stop came from elsewhere,
     * not the user pressing X), drop it now so the window disappears
     * promptly during teardown. IsWindow guards the case where WM_CLOSE
     * already destroyed it. */
    if (frame_hwnd && IsWindow(frame_hwnd)) {
        DestroyWindow(frame_hwnd);
    }
    return 0;
}

/* Minimal host callback. Returns 0 for everything except a few opcodes the
 * plugin might check at construction time. */
static VST_CALL intptr_t host_callback(AEffect* eff, int32_t opcode,
                                        int32_t idx, intptr_t val,
                                        void* ptr, float opt) {
    (void)eff; (void)idx; (void)val; (void)ptr; (void)opt;
    /* The previous opcode numbers here were wrong (off by one for the first
     * two, swapped/imaginary for sample-rate and block-size). Simple plugins
     * like WagnerSharp tolerated the bogus answers, but JUCE-based plugins
     * query audioMasterVersion (opcode 1) early and return NULL from
     * VSTPluginMain if the host appears to be "version 0". Use the SDK
     * constants from vst2.h instead of bare integers. */
    switch (opcode) {
        case audioMasterAutomate:      return 0;       /* (0) ack a setParam */
        case audioMasterVersion:       return 2400;    /* (1) VST 2.4 */
        case audioMasterCurrentId:     return 0;       /* (2) shell plugins */
        case audioMasterIdle:          return 0;       /* (3) */
        case audioMasterWantMidi:      return 0;       /* (6) deprecated */
        case audioMasterGetTime:       return 0;       /* (7) no time info */
        case audioMasterGetSampleRate: return 48000;   /* (16) */
        case audioMasterGetBlockSize:  return 512;     /* (17) */
        case audioMasterGetVendorString: {             /* (32) */
            if (ptr) strncpy((char*)ptr, "vstpoc", 64);
            return 1;
        }
        case audioMasterGetProductString: {            /* (33) */
            if (ptr) strncpy((char*)ptr, "vst_host", 64);
            return 1;
        }
        default: return 0;
    }
}

/* Map the shared-memory file. The Android side has already created and
 * truncated it to sizeof(VstpocShared). */
static VstpocShared* map_shared(const char* path) {
    HANDLE fh = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        LOG("CreateFileA(%s) failed: %lu\n", path, (unsigned long)GetLastError());
        return NULL;
    }
    DWORD size_lo = (DWORD)(sizeof(VstpocShared) & 0xffffffff);
    DWORD size_hi = (DWORD)((sizeof(VstpocShared) >> 32) & 0xffffffff);
    HANDLE mh = CreateFileMappingA(fh, NULL, PAGE_READWRITE,
                                   size_hi, size_lo, NULL);
    if (!mh) {
        LOG("CreateFileMappingA failed: %lu\n", (unsigned long)GetLastError());
        CloseHandle(fh);
        return NULL;
    }
    VstpocShared* s = (VstpocShared*)MapViewOfFile(
        mh, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(VstpocShared));
    if (!s) {
        LOG("MapViewOfFile failed: %lu\n", (unsigned long)GetLastError());
        CloseHandle(mh);
        CloseHandle(fh);
        return NULL;
    }
    /* fh + mh leak intentionally — the mapping outlives this scope and the
     * process exits soon enough. */
    return s;
}

static int clamped_param_count(AEffect* eff) {
    int n = eff ? eff->numParams : 0;
    if (n < 0) n = 0;
    if (n > VSTPOC_MAX_PARAMS) n = VSTPOC_MAX_PARAMS;
    return n;
}

static float clamp_normalized_param(float value) {
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

static void begin_param_values_write(VstpocShared* shm) {
    __atomic_add_fetch(&shm->param_values_seq, 1, __ATOMIC_ACQ_REL);
    __sync_synchronize();
}

static void end_param_values_write(VstpocShared* shm) {
    __sync_synchronize();
    __atomic_add_fetch(&shm->param_values_seq, 1, __ATOMIC_RELEASE);
}

static void publish_param_values(VstpocShared* shm, AEffect* eff) {
    if (!shm || !eff) return;
    int n = clamped_param_count(eff);
    begin_param_values_write(shm);
    for (int i = 0; i < n; i++) {
        shm->param_values[i] = clamp_normalized_param(eff->getParameter(eff, i));
    }
    end_param_values_write(shm);
}

static void publish_params(VstpocShared* shm, AEffect* eff) {
    int n = clamped_param_count(eff);
    for (int i = 0; i < n; i++) {
        char name[VSTPOC_PARAM_NAME_LEN] = {0};
        eff->dispatcher(eff, /*effGetParamName=*/8, i, 0, name, 0.0f);
        strncpy(shm->param_names[i], name, VSTPOC_PARAM_NAME_LEN - 1);
    }
    shm->param_count = n;
    __sync_synchronize();
    publish_param_values(shm, eff);
}

#define VSTPOC_VST2_STATE_MAGIC "GRCVST2S"
#define VSTPOC_VST2_STATE_VERSION 1u
#define VSTPOC_VST2_STATE_KIND_BANK_CHUNK 1u
#define VSTPOC_VST2_STATE_KIND_PROGRAM_CHUNK 2u
#define VSTPOC_VST2_STATE_KIND_PARAMS 3u
#define VSTPOC_VST2_STATE_KIND_CHUNKS 4u
#define VSTPOC_VST2_NO_PROGRAM UINT32_MAX
#define VSTPOC_GUEST_STATE_MAX_BYTES (64u * 1024u * 1024u)

#pragma pack(push, 1)
typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t kind;
    uint32_t param_count;
    uint32_t reserved;
    uint64_t payload_size;
} VstpocVst2StateHeader;

typedef struct {
    uint32_t current_program;
    uint32_t reserved;
    uint64_t bank_size;
    uint64_t program_size;
} VstpocVst2ChunksPayloadHeader;
#pragma pack(pop)

typedef struct {
    void* data;
    uint64_t size;
} VstpocVst2ChunkCopy;

static int write_exact(FILE* f, const void* data, size_t len) {
    if (len == 0) return 1;
    return fwrite(data, 1, len, f) == len;
}

static int read_exact(FILE* f, void* data, size_t len) {
    if (len == 0) return 1;
    return fread(data, 1, len, f) == len;
}

static int write_vst2_state_file(const char* path, uint32_t kind, uint32_t param_count,
                                 const void* payload, uint64_t payload_size) {
    if (!path || !*path) return 0;
    if (payload_size > VSTPOC_GUEST_STATE_MAX_BYTES) return 0;
    FILE* f = fopen(path, "wb");
    if (!f) return 0;

    VstpocVst2StateHeader h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, VSTPOC_VST2_STATE_MAGIC, sizeof(h.magic));
    h.version = VSTPOC_VST2_STATE_VERSION;
    h.kind = kind;
    h.param_count = param_count;
    h.payload_size = payload_size;

    int ok = write_exact(f, &h, sizeof(h)) &&
             (payload_size == 0 || (payload && write_exact(f, payload, (size_t)payload_size)));
    fclose(f);
    return ok;
}

static const char* vst2_chunk_name(int index) {
    return index == 0 ? "bank" : "program";
}

static void free_vst2_chunk_copy(VstpocVst2ChunkCopy* copy) {
    if (!copy) return;
    if (copy->data) free(copy->data);
    copy->data = NULL;
    copy->size = 0;
}

static int get_vst2_chunk_copy(AEffect* eff, int index, VstpocVst2ChunkCopy* out) {
    if (!eff || !out) return -1;
    out->data = NULL;
    out->size = 0;

    void* chunk = NULL;
    intptr_t bytes = eff->dispatcher(eff, effGetChunk, index, 0, &chunk, 0.0f);
    LOG("state: effGetChunk(%s) -> %lld bytes ptr=%p\n",
        vst2_chunk_name(index), (long long)bytes, chunk);
    if (bytes <= 0 || !chunk) return 0;
    if ((uint64_t)bytes > VSTPOC_GUEST_STATE_MAX_BYTES) return -1;

    void* data = malloc((size_t)bytes);
    if (!data) return -1;
    memcpy(data, chunk, (size_t)bytes);
    out->data = data;
    out->size = (uint64_t)bytes;
    return 1;
}

static uint32_t get_current_vst2_program(AEffect* eff) {
    if (!eff || eff->numPrograms <= 0) return VSTPOC_VST2_NO_PROGRAM;
    intptr_t program = eff->dispatcher(eff, effGetProgram, 0, 0, NULL, 0.0f);
    if (program < 0 || program >= eff->numPrograms) return VSTPOC_VST2_NO_PROGRAM;
    return (uint32_t)program;
}

static void set_vst2_program_if_valid(AEffect* eff, uint32_t program) {
    if (!eff || program == VSTPOC_VST2_NO_PROGRAM) return;
    if (eff->numPrograms <= 0 || program >= (uint32_t)eff->numPrograms) return;
    eff->dispatcher(eff, effSetProgram, 0, (intptr_t)program, NULL, 0.0f);
}

static int write_vst2_chunks_state_file(const char* path,
                                        uint32_t current_program,
                                        const VstpocVst2ChunkCopy* bank,
                                        const VstpocVst2ChunkCopy* program,
                                        uint64_t* out_size) {
    if (out_size) *out_size = 0;
    if (!path || !*path) return 0;

    uint64_t payload_size = (uint64_t)sizeof(VstpocVst2ChunksPayloadHeader);
    const uint64_t bank_size = bank ? bank->size : 0;
    const uint64_t program_size = program ? program->size : 0;
    if (bank_size > VSTPOC_GUEST_STATE_MAX_BYTES - payload_size) return 0;
    payload_size += bank_size;
    if (program_size > VSTPOC_GUEST_STATE_MAX_BYTES - payload_size) return 0;
    payload_size += program_size;

    FILE* f = fopen(path, "wb");
    if (!f) return 0;

    VstpocVst2StateHeader h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, VSTPOC_VST2_STATE_MAGIC, sizeof(h.magic));
    h.version = VSTPOC_VST2_STATE_VERSION;
    h.kind = VSTPOC_VST2_STATE_KIND_CHUNKS;
    h.param_count = 0;
    h.payload_size = payload_size;

    VstpocVst2ChunksPayloadHeader ch;
    memset(&ch, 0, sizeof(ch));
    ch.current_program = current_program;
    ch.bank_size = bank_size;
    ch.program_size = program_size;

    int ok = write_exact(f, &h, sizeof(h)) &&
             write_exact(f, &ch, sizeof(ch)) &&
             (bank_size == 0 || write_exact(f, bank->data, (size_t)bank_size)) &&
             (program_size == 0 || write_exact(f, program->data, (size_t)program_size));
    fclose(f);
    if (!ok) return 0;
    if (out_size) *out_size = (uint64_t)sizeof(h) + payload_size;
    return 1;
}

static int save_vst2_state_to_file(VstpocShared* shm, AEffect* eff,
                                   const char* path, uint64_t* out_size) {
    (void)shm;
    if (!eff || !path || !*path) return VSTPOC_STATE_STATUS_ERROR;
    if (out_size) *out_size = 0;

    if (eff->flags & effFlagsProgramChunks) {
        VstpocVst2ChunkCopy bank = {0};
        VstpocVst2ChunkCopy program = {0};
        int bank_status = get_vst2_chunk_copy(eff, 0, &bank);
        int program_status = get_vst2_chunk_copy(eff, 1, &program);
        if (bank_status < 0 || program_status < 0) {
            free_vst2_chunk_copy(&bank);
            free_vst2_chunk_copy(&program);
            return VSTPOC_STATE_STATUS_ERROR;
        }
        if (bank.size > 0 || program.size > 0) {
            const uint32_t current_program = get_current_vst2_program(eff);
            int ok = write_vst2_chunks_state_file(path, current_program,
                                                  &bank, &program, out_size);
            LOG("state: saved VST2 chunks bank=%llu program=%llu currentProgram=%u ok=%d\n",
                (unsigned long long)bank.size,
                (unsigned long long)program.size,
                current_program == VSTPOC_VST2_NO_PROGRAM ? 0xffffffffu : current_program,
                ok);
            free_vst2_chunk_copy(&bank);
            free_vst2_chunk_copy(&program);
            if (!ok) {
                return VSTPOC_STATE_STATUS_ERROR;
            }
            return VSTPOC_STATE_STATUS_OK;
        }
        free_vst2_chunk_copy(&bank);
        free_vst2_chunk_copy(&program);
    }

    int n = clamped_param_count(eff);
    if (n <= 0) return VSTPOC_STATE_STATUS_UNSUPPORTED;
    float* values = (float*)calloc((size_t)n, sizeof(float));
    if (!values) return VSTPOC_STATE_STATUS_ERROR;
    for (int i = 0; i < n; i++) {
        values[i] = clamp_normalized_param(eff->getParameter(eff, i));
    }
    const uint64_t payload_size = (uint64_t)n * sizeof(float);
    int ok = write_vst2_state_file(path, VSTPOC_VST2_STATE_KIND_PARAMS,
                                   (uint32_t)n, values, payload_size);
    free(values);
    if (!ok) return VSTPOC_STATE_STATUS_ERROR;
    if (out_size) *out_size = (uint64_t)sizeof(VstpocVst2StateHeader) + payload_size;
    LOG("state: saved VST2 parameter fallback (%d params)\n", n);
    return VSTPOC_STATE_STATUS_OK;
}

static void begin_vst2_state_restore(AEffect* eff) {
    if (!eff) return;
    eff->dispatcher(eff, effStopProcess, 0, 0, NULL, 0.0f);
    eff->dispatcher(eff, effMainsChanged, 0, 0, NULL, 0.0f);
}

static void end_vst2_state_restore(AEffect* eff) {
    if (!eff) return;
    eff->dispatcher(eff, effMainsChanged, 0, 1, NULL, 0.0f);
    eff->dispatcher(eff, effStartProcess, 0, 0, NULL, 0.0f);
}

static intptr_t set_vst2_chunk(AEffect* eff, int index,
                               const void* payload, uint64_t payload_size) {
    intptr_t ret = eff->dispatcher(eff, effSetChunk, index,
                                   (intptr_t)payload_size, (void*)payload, 0.0f);
    LOG("state: effSetChunk(%s, %llu bytes) -> %lld\n",
        vst2_chunk_name(index),
        (unsigned long long)payload_size,
        (long long)ret);
    return ret;
}

static int load_vst2_state_from_file(VstpocShared* shm, AEffect* eff, const char* path) {
    if (!eff || !path || !*path) return VSTPOC_STATE_STATUS_ERROR;
    FILE* f = fopen(path, "rb");
    if (!f) return VSTPOC_STATE_STATUS_ERROR;

    VstpocVst2StateHeader h;
    if (!read_exact(f, &h, sizeof(h))) {
        fclose(f);
        return VSTPOC_STATE_STATUS_ERROR;
    }
    if (memcmp(h.magic, VSTPOC_VST2_STATE_MAGIC, sizeof(h.magic)) != 0 ||
        h.version != VSTPOC_VST2_STATE_VERSION ||
        h.payload_size > VSTPOC_GUEST_STATE_MAX_BYTES) {
        fclose(f);
        return VSTPOC_STATE_STATUS_ERROR;
    }

    void* payload = NULL;
    if (h.payload_size > 0) {
        payload = malloc((size_t)h.payload_size);
        if (!payload || !read_exact(f, payload, (size_t)h.payload_size)) {
            if (payload) free(payload);
            fclose(f);
            return VSTPOC_STATE_STATUS_ERROR;
        }
    }
    fclose(f);

    int status = VSTPOC_STATE_STATUS_OK;
    if (h.kind == VSTPOC_VST2_STATE_KIND_BANK_CHUNK ||
        h.kind == VSTPOC_VST2_STATE_KIND_PROGRAM_CHUNK) {
        if (!payload || h.payload_size == 0 || h.payload_size > (uint64_t)INTPTR_MAX) {
            status = VSTPOC_STATE_STATUS_ERROR;
        } else {
            const int index = (h.kind == VSTPOC_VST2_STATE_KIND_BANK_CHUNK) ? 0 : 1;
            begin_vst2_state_restore(eff);
            set_vst2_chunk(eff, index, payload, h.payload_size);
            end_vst2_state_restore(eff);
            LOG("state: restored VST2 %s chunk (%llu bytes payload)\n",
                index == 0 ? "bank" : "program",
                (unsigned long long)h.payload_size);
        }
    } else if (h.kind == VSTPOC_VST2_STATE_KIND_CHUNKS) {
        if (!payload || h.payload_size < (uint64_t)sizeof(VstpocVst2ChunksPayloadHeader)) {
            status = VSTPOC_STATE_STATUS_ERROR;
        } else {
            VstpocVst2ChunksPayloadHeader ch;
            memcpy(&ch, payload, sizeof(ch));
            const uint64_t expected_size = (uint64_t)sizeof(ch) + ch.bank_size + ch.program_size;
            if (ch.bank_size > VSTPOC_GUEST_STATE_MAX_BYTES ||
                ch.program_size > VSTPOC_GUEST_STATE_MAX_BYTES ||
                expected_size != h.payload_size ||
                ch.bank_size > (uint64_t)INTPTR_MAX ||
                ch.program_size > (uint64_t)INTPTR_MAX) {
                status = VSTPOC_STATE_STATUS_ERROR;
            } else {
                const unsigned char* cursor = (const unsigned char*)payload + sizeof(ch);
                const unsigned char* bank = cursor;
                const unsigned char* program = bank + ch.bank_size;
                int applied = 0;

                begin_vst2_state_restore(eff);
                set_vst2_program_if_valid(eff, ch.current_program);
                if (ch.bank_size > 0) {
                    set_vst2_chunk(eff, 0, bank, ch.bank_size);
                    applied = 1;
                }
                set_vst2_program_if_valid(eff, ch.current_program);
                if (ch.program_size > 0) {
                    set_vst2_chunk(eff, 1, program, ch.program_size);
                    applied = 1;
                }
                set_vst2_program_if_valid(eff, ch.current_program);
                end_vst2_state_restore(eff);

                if (!applied) {
                    status = VSTPOC_STATE_STATUS_ERROR;
                } else {
                    LOG("state: restored VST2 chunks bank=%llu program=%llu currentProgram=%u\n",
                        (unsigned long long)ch.bank_size,
                        (unsigned long long)ch.program_size,
                        ch.current_program == VSTPOC_VST2_NO_PROGRAM ? 0xffffffffu : ch.current_program);
                }
            }
        }
    } else if (h.kind == VSTPOC_VST2_STATE_KIND_PARAMS) {
        if (!payload || h.payload_size < (uint64_t)h.param_count * sizeof(float)) {
            status = VSTPOC_STATE_STATUS_ERROR;
        } else {
            const int n = clamped_param_count(eff);
            const int count = (h.param_count < (uint32_t)n) ? (int)h.param_count : n;
            float* values = (float*)payload;
            for (int i = 0; i < count; i++) {
                eff->setParameter(eff, i, clamp_normalized_param(values[i]));
            }
            LOG("state: restored VST2 parameter fallback (%d params)\n", count);
        }
    } else {
        status = VSTPOC_STATE_STATUS_ERROR;
    }

    if (payload) free(payload);
    if (status == VSTPOC_STATE_STATUS_OK) publish_param_values(shm, eff);
    return status;
}

static void complete_state_request(VstpocShared* shm, uint32_t seq,
                                   uint32_t status, uint64_t size,
                                   const char* message) {
    if (!shm) return;
    __atomic_store_n(&shm->state_size, size, __ATOMIC_RELEASE);
    if (message && *message) {
        size_t n = strlen(message);
        if (n >= VSTPOC_STATE_MESSAGE_LEN) n = VSTPOC_STATE_MESSAGE_LEN - 1;
        memcpy(shm->state_message, message, n);
        shm->state_message[n] = '\0';
    } else {
        shm->state_message[0] = '\0';
    }
    __atomic_store_n(&shm->state_status, status, __ATOMIC_RELEASE);
    __atomic_store_n(&shm->state_response_seq, seq, __ATOMIC_RELEASE);
}

static void handle_state_request(VstpocShared* shm, AEffect* eff) {
    static uint32_t handled_seq = 0;
    if (!shm || !eff) return;

    const uint32_t seq = __atomic_load_n(&shm->state_request_seq, __ATOMIC_ACQUIRE);
    if (seq == 0 || seq == handled_seq) return;
    handled_seq = seq;

    char path[VSTPOC_STATE_PATH_LEN];
    memcpy(path, shm->state_path, sizeof(path));
    path[sizeof(path) - 1] = '\0';
    const uint32_t command = __atomic_load_n(&shm->state_command, __ATOMIC_ACQUIRE);
    uint64_t size = 0;
    uint32_t status = VSTPOC_STATE_STATUS_ERROR;
    const char* message = NULL;

    if (command == VSTPOC_STATE_CMD_SAVE) {
        status = (uint32_t)save_vst2_state_to_file(shm, eff, path, &size);
        if (status == VSTPOC_STATE_STATUS_UNSUPPORTED) message = "VST2 state unsupported";
        else if (status != VSTPOC_STATE_STATUS_OK) message = "VST2 state save failed";
    } else if (command == VSTPOC_STATE_CMD_LOAD) {
        status = (uint32_t)load_vst2_state_from_file(shm, eff, path);
        if (status != VSTPOC_STATE_STATUS_OK) message = "VST2 state load failed";
    } else {
        message = "unknown VST2 state command";
    }

    complete_state_request(shm, seq, status, size, message);
}

/* Load one VST2 plugin DLL into g_plugins[slot]. Extracted from main()'s
 * load loop so VSTPOC_LOAD_ON_EDITOR_THREAD can run it on the editor thread
 * (JUCE MessageManager binding — see the option-2 comment at the globals).
 * Returns 1 on success. Crash recovery relies on the single global
 * g_pluginmain_jmp, so callers must not run two loads concurrently. */
static int load_one_plugin(int slot, const char* dll_path) {
    const int kSampleRate = 48000;
    const int kBlockSize  = 512;

    LOG("loading plugin[%d]: %s\n", slot, dll_path);

    LOG("  pre-LoadLibraryA\n");
    HMODULE plugin = LoadLibraryA(dll_path);
    LOG("  post-LoadLibraryA plugin=%p\n", (void*)plugin);
    if (!plugin) {
        unsigned long e = (unsigned long)GetLastError();
        LOG("  LoadLibraryA(%s) failed: %lu\n", dll_path, e);
        switch (e) {
            case 193:  /* ERROR_BAD_EXE_FORMAT */
                RECORD_FAILURE("32-bit plugin — this build only supports 64-bit VSTs");
                break;
            case 126:  /* ERROR_MOD_NOT_FOUND */
                RECORD_FAILURE("LoadLibrary failed — a dependency DLL is missing (see wine log)");
                break;
            default:
                RECORD_FAILURE("LoadLibrary failed (err=%lu) — DLL or a dependency missing", e);
                break;
        }
        return 0;
    }

    VstPluginMainFn entry = (VstPluginMainFn)GetProcAddress(plugin, "VSTPluginMain");
    if (!entry) {
        entry = (VstPluginMainFn)GetProcAddress(plugin, "main");
    }
    if (!entry) {
        LOG("  VSTPluginMain not found in %s\n", dll_path);
        RECORD_FAILURE("VSTPluginMain not found — file is not a VST2 plugin");
        FreeLibrary(plugin);
        return 0;
    }

    /* Wrap VSTPluginMain in SEH so a plugin-side access violation
     * doesn't take down the whole vst_host process — see pluginmain_veh. */
    LOG("  pre-VSTPluginMain entry=%p\n", (void*)entry);
    AEffect* eff = NULL;
    g_pluginmain_fault_code = 0;
    g_pluginmain_fault_addr = 0;
    if (setjmp(g_pluginmain_jmp) == 0) {
        g_pluginmain_jmp_armed = 1;
        eff = entry(host_callback);
        g_pluginmain_jmp_armed = 0;
    } else {
        /* Returned via longjmp from pluginmain_veh — plugin crashed. */
        LOG("  VSTPluginMain raised exception code=0x%08lx at %p (plugin init failed)\n",
            g_pluginmain_fault_code, (void*)g_pluginmain_fault_addr);
        RECORD_FAILURE("Plugin VSTPluginMain crashed (code 0x%08lx at %p) — missing dependency or unsupported Win32 API",
                       g_pluginmain_fault_code, (void*)g_pluginmain_fault_addr);
        /* Leak the module — half-initialised plugin state can make
         * FreeLibrary crash. */
        return 0;
    }
    LOG("  post-VSTPluginMain eff=%p magic=%08x\n",
        (void*)eff, eff ? (unsigned)eff->magic : 0);
    if (!eff || eff->magic != kEffectMagic) {
        LOG("  VSTPluginMain returned invalid AEffect (magic=%08x)\n",
            eff ? (unsigned)eff->magic : 0);
        RECORD_FAILURE("Plugin init failed — VSTPluginMain returned null/invalid AEffect (check wine log for missing assemblies)");
        FreeLibrary(plugin);
        return 0;
    }
    LOG("  plugin[%d] loaded: numParams=%d numInputs=%d numOutputs=%d uniqueID=%08x\n",
        slot, eff->numParams, eff->numInputs, eff->numOutputs, (unsigned)eff->uniqueID);

    /* Standard VST2 init dance. */
    eff->dispatcher(eff, /*effOpen=*/0, 0, 0, NULL, 0.0f);
    eff->dispatcher(eff, /*effSetSampleRate=*/10, 0, 0, NULL, (float)kSampleRate);
    eff->dispatcher(eff, /*effSetBlockSize=*/11, 0, kBlockSize, NULL, 0.0f);

    g_plugins[slot].module = plugin;
    g_plugins[slot].eff    = eff;
    g_plugins[slot].path   = dll_path;
    return 1;
}

int main(int argc, char** argv) {
    /* Declare the process as Per-Monitor V2 DPI-aware BEFORE creating any
     * window or loading any plugin. Otherwise wine's GDI virtualizes the
     * client rect: AmpCraft's window is 1290x612 in X11 physical pixels
     * but wine's X11DRV_CreateWindowSurface allocates the GDI surface at
     * 896x640 (logical, 96 DPI). AmpCraft then BitBlts 896 wide pixels
     * into a 1290 wide X11 window, leaving the right 30% brown.
     * SetProcessDpiAwarenessContext at startup makes wine return physical
     * pixels from GetClientRect → surface matches X11 → no cropping.
     *
     * Resolved at runtime because mingw-w64's headers don't declare
     * SetProcessDpiAwarenessContext under our default Windows version
     * macro, but user32 exports it on Win10+. */
    {
        typedef int (WINAPI *PFN_SetProcessDpiAwarenessContext)(void*);
        HMODULE user32 = GetModuleHandleA("user32.dll");
        if (user32) {
            PFN_SetProcessDpiAwarenessContext pSet =
                (PFN_SetProcessDpiAwarenessContext)GetProcAddress(
                    user32, "SetProcessDpiAwarenessContext");
            if (pSet) {
                /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = ((HANDLE)-4) */
                int ret = pSet((void*)(intptr_t)-4);
                LOG("SetProcessDpiAwarenessContext(PER_MONITOR_V2) -> %d\n", ret);
            } else {
                /* Fallback: legacy SetProcessDPIAware (Vista+). */
                typedef int (WINAPI *PFN_SetProcessDPIAware)(void);
                PFN_SetProcessDPIAware pLegacy =
                    (PFN_SetProcessDPIAware)GetProcAddress(user32, "SetProcessDPIAware");
                if (pLegacy) {
                    int ret = pLegacy();
                    LOG("SetProcessDPIAware (legacy) -> %d\n", ret);
                }
            }
        }
    }

    /* Spawn rpcss.exe so the RPC service is reachable when plugins call
     * CoInitialize / CoCreateInstance during VSTPluginMain. Without this,
     * wine's ole32 logs 'Failed to open RpcSs service' and any plugin that
     * needs COM (Amplitube 5 spawns a worker thread that deadlocks on a
     * CS waiting for COM init to complete) hangs in VSTPluginMain. We
     * don't wait for rpcss to finish initializing — it runs as a sibling
     * process and ole32 will retry the connection internally. */
    {
        STARTUPINFOA si = { .cb = sizeof(si) };
        PROCESS_INFORMATION pi = {0};
        char cmd[] = "C:\\windows\\system32\\rpcss.exe";
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                           DETACHED_PROCESS, NULL, NULL, &si, &pi)) {
            LOG("spawned rpcss.exe pid=%lu\n", (unsigned long)pi.dwProcessId);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            LOG("CreateProcessA(rpcss.exe) failed: %lu\n",
                (unsigned long)GetLastError());
        }
    }

    /* Install vectored exception handler so a plugin crash in VSTPluginMain
     * unwinds to setjmp instead of taking down the whole vst_host process.
     * See pluginmain_veh above for why __try/__except isn't sufficient. */
    AddVectoredExceptionHandler(1 /* CALL_FIRST */, pluginmain_veh);

    if (argc < 3) {
        LOG("usage: vst_host.exe <shm_file> <plugin1.dll> [plugin2.dll ...]\n");
        return 2;
    }
    const char* shm_path = argv[1];
    const int requestedPlugins = argc - 2;
    if (requestedPlugins > VSTHOST_MAX_PLUGINS) {
        LOG("WARN: %d plugins requested but VSTHOST_MAX_PLUGINS=%d; truncating\n",
            requestedPlugins, VSTHOST_MAX_PLUGINS);
    }
    const int numPlugins = requestedPlugins < VSTHOST_MAX_PLUGINS
                                ? requestedPlugins : VSTHOST_MAX_PLUGINS;

    LOG("starting; shm=%s numPlugins=%d\n", shm_path, numPlugins);

    VstpocShared* shm = map_shared(shm_path);
    if (!shm) return 3;
    /* Make sure status starts pending — map_shared zeroes the region but be
     * explicit so a stale reader sees the expected sentinel. */
    write_status(shm, 0, NULL);

    /* Standard VST2 init params shared across all plugins. */
    const int kBlockSize  = 512;

    {
        const char* lt = getenv("VSTPOC_LOAD_ON_EDITOR_THREAD");
        g_load_on_editor_thread = (lt && *lt && *lt != '0');
    }
    g_shm = shm;  /* editor threads poll stop_flag during option-2 load */

    if (!g_load_on_editor_thread) {
        /* Default: load each plugin from argv on the main thread, compacting
         * successes into g_plugins[0..count). Skip plugins that fail — the
         * chain just drops that one rather than aborting the whole host. */
        for (int i = 0; i < numPlugins; i++) {
            if (load_one_plugin(g_pluginCount, argv[2 + i])) g_pluginCount++;
        }
    } else {
        /* Option-2 mode: fixed argv slots; each editor thread loads its own
         * plugin (JUCE MessageManager binding — see globals comment). Loads
         * are serialized: spawn, wait for the result, then spawn the next
         * (the VSTPluginMain crash-recovery jmp_buf is a single global).
         * Failed slots keep eff==NULL; every consumer below skips them. */
        LOG("VSTPOC_LOAD_ON_EDITOR_THREAD=1: loading plugins on editor threads\n");
        g_pluginCount = numPlugins;
        for (int i = 0; i < numPlugins; i++) {
            g_paths[i] = argv[2 + i];
            HANDLE th = CreateThread(NULL, 8 * 1024 * 1024,
                                     per_plugin_editor_thread,
                                     (LPVOID)(intptr_t)i,
                                     STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
            if (!th) {
                LOG("editor[%d] CreateThread failed: %lu\n",
                    i, (unsigned long)GetLastError());
                InterlockedExchange(&g_load_result[i], 2);
                continue;
            }
            CloseHandle(th);
            LOG("editor[%d] loader thread spawned\n", i);
            while (g_load_result[i] == 0) Sleep(5);
        }
    }

    int loadedCount = 0;
    for (int i = 0; i < g_pluginCount; i++) if (g_plugins[i].eff) loadedCount++;
    if (loadedCount == 0) {
        LOG("no plugins loaded; aborting\n");
        write_status(shm, 2,
            g_failure_set ? g_failure_msg
                          : "No plugins loaded (no specific error captured)");
        return 7;
    }
    LOG("loaded %d/%d plugins; chaining DSP in argv order\n", loadedCount, numPlugins);

    /* The "front" plugin is the one whose editor + params we expose to
     * Android in this commit. Later commits will extend the param ring
     * to address all plugins. (First LOADED slot — option-2 mode can leave
     * NULL holes for failed plugins.) */
    AEffect* eff = NULL;
    for (int i = 0; i < g_pluginCount; i++) {
        if (g_plugins[i].eff) { eff = g_plugins[i].eff; break; }
    }

    /* Probe: does this plugin advertise an editor? Don't call effEditGetRect
     * here — JUCE-based plugins bind their MessageManager to the FIRST
     * thread that touches their editor API. If main thread calls
     * effEditGetRect, JUCE pins the message thread to main; the per-plugin
     * editor thread's later effEditGetRect then deadlocks waiting for main
     * (which is in the audio loop, not pumping messages). The editor thread
     * does its own effEditGetRect right before effEditOpen — that's the
     * only thread that should touch editor APIs. */
    LOG("flags=0x%x hasEditor=%d\n", (unsigned)eff->flags,
        (eff->flags & effFlagsHasEditor) ? 1 : 0);

    /* effMainsChanged(1) + effStartProcess for ALL plugins in the chain. */
    for (int i = 0; i < g_pluginCount; i++) {
        AEffect* e = g_plugins[i].eff;
        if (!e) continue;
        e->dispatcher(e, /*effMainsChanged=*/12, 0, 1, NULL, 0.0f);
        e->dispatcher(e, /*effStartProcess=*/71, 0, 0, NULL, 0.0f);
    }

    /* publish_params calls eff->dispatcher (effGetParamName), which goes
     * into plugin code that may take the wine user32 global critical
     * section. The editor thread (next) takes that same CS during its
     * CreateWindow / effEditOpen. Doing it in this order means the main
     * thread is done with all plugin queries BEFORE the editor thread
     * possibly grabs (and holds) the CS for a long time.
     *
     * Multi-plugin note: only publishes the FIRST plugin's params for
     * now. Later commits extend the shared layout for per-plugin params. */
    publish_params(shm, eff);

    /* signal Android side */
    write_status(shm, 1, NULL);
    __sync_synchronize();
    shm->guest_ready = 1;

    /* Spawn one editor thread per loaded plugin. Each thread blocks on
     * the shared g_editor_open_index counter so only one
     * CreateWindowExA is in flight at a time (avoids the wine user32
     * deadlock that happens with concurrent CreateWindowExA). After all
     * editors have opened, each thread runs its own message loop in
     * parallel — independent enough that a hang in one editor doesn't
     * stall the others. */
    g_shm = shm;
    g_eff = eff;
    if (!g_load_on_editor_thread) {
        for (int p = 0; p < g_pluginCount; p++) {
            if (!g_plugins[p].eff) continue;
            /* JUCE-based plugins (BOD, etc) blow past the default ~1MB stack
             * during effEditOpen — observed 1856-byte overflow inside the
             * editor thread. Reserve 8 MB to be comfortably above the 2-4 MB
             * a typical JUCE editor uses. */
            HANDLE th = CreateThread(NULL, 8 * 1024 * 1024,
                                     per_plugin_editor_thread,
                                     (LPVOID)(intptr_t)p,
                                     STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
            if (th) {
                CloseHandle(th);
                LOG("editor[%d] thread spawned\n", p);
            } else {
                LOG("editor[%d] CreateThread failed: %lu\n",
                    p, (unsigned long)GetLastError());
            }
        }
    }
    /* Option-2 mode: the loader/editor threads are already running and parked
     * on this flag; release them now that params/status are published. */
    InterlockedExchange(&g_editor_go, 1);

    /* Audio buffers. Plugin gets de-interleaved float**; ring is interleaved
     * stereo. Use kBlockSize-sized chunks. */
    static float in_l [512], in_r [512];
    static float out_l[512], out_r[512];
    float* inputs[2]  = { in_l,  in_r  };
    float* outputs[2] = { out_l, out_r };

    /* Wait for the editor thread to finish effEditOpen before we start
     * calling processReplacing on the audio thread. processReplacing
     * goes into plugin DSP code that touches the wine user32 CS
     * (WagnerSharp's setParameter side-effects update GUI state). The
     * editor thread holds the same CS during CreateWindowExA. With both
     * threads contending — and our C++ X server having slower replies
     * than the Java one — wine's CS retry loop deadlocks. Sleep here
     * costs us a couple of buffer underruns but unblocks the editor. */
    /* Pump messages on main while waiting for the editor. JUCE-based
     * plugins (BOD, etc) bind their MessageManager to the thread that
     * loaded the DLL (i.e. main). The editor thread's dispatcher calls
     * then post messages to main and wait — if main isn't pumping, the
     * editor deadlocks. */
    LOG("audio thread: waiting for editor to finish opening (pumping msgs)\n");
    {
        int waited_ms = 0;
        while (!g_editor_open_done && !shm->stop_flag && waited_ms < 30000) {
            MSG msg;
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
            handle_state_request(shm, eff);
            Sleep(10);
            waited_ms += 10;
        }
        LOG("audio thread: editor_open_done=%d after %dms\n",
            (int)g_editor_open_done, waited_ms);
    }

    /* FEX JIT warm-up: call processReplacing a few times with silent input
     * so FEX-Emu translates each plugin's hot DSP path BEFORE the audio
     * thread starts feeding real samples. Without this, the first blocks
     * after Play hit single-shot ARM64→x86_64 translation cost on the
     * audio thread and starve the Oboe output ring.
     *
     * Defensive note: some VST2 plugins (WagnerSharp included) write back
     * to their INPUT buffers despite the spec forbidding it. With a fixed
     * warm-up input buffer, the plugin's own output residue then re-enters
     * via the next iteration's input and the internal filters integrate
     * the bleed until the plugin saturates at ~-8 dB. Re-zeroing the
     * input every iteration AND keeping the count small mitigates this.
     * 8 iters × 512 samples = ~85 ms of audio — still enough to exercise
     * the steady-state DSP path for JIT translation. */
    LOG("JIT warm-up: 8 silent processReplacing calls per plugin (%d plugins)\n",
        g_pluginCount);
    {
        static float warm_in_l[512], warm_in_r[512];
        static float warm_out_l[512], warm_out_r[512];
        float* w_in[2]  = { warm_in_l,  warm_in_r  };
        float* w_out[2] = { warm_out_l, warm_out_r };
        for (int iter = 0; iter < 8; iter++) {
            /* Defensive zero of input on every iter — see comment above. */
            for (int i = 0; i < kBlockSize; i++) {
                warm_in_l[i]  = 0.0f;
                warm_in_r[i]  = 0.0f;
            }
            float* cur_in[2]  = { w_in[0],  w_in[1]  };
            float* cur_out[2] = { w_out[0], w_out[1] };
            for (int p = 0; p < g_pluginCount; p++) {
                AEffect* pe = g_plugins[p].eff;
                if (!pe) continue;
                pe->processReplacing(pe, cur_in, cur_out, kBlockSize);
                float* tmp;
                tmp = cur_in[0]; cur_in[0] = cur_out[0]; cur_out[0] = tmp;
                tmp = cur_in[1]; cur_in[1] = cur_out[1]; cur_out[1] = tmp;
            }
        }
    }
    LOG("JIT warm-up complete\n");

    LOG("entering process loop\n");
    while (!shm->stop_flag) {
        /* Pump messages on every iteration. JUCE plugins post async work
         * from the editor thread to the main thread (where their
         * MessageManager lives) and stall waiting for it. */
        {
            MSG msg;
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
        }
        handle_state_request(shm, eff);
        /* Param draining moved to the editor thread to avoid a user32-CS
         * deadlock: WagnerSharp's setParameter touches its GUI state and
         * acquires the wine user32 critical section, which the editor
         * thread also holds during effEditOpen / CreateWindow. With both
         * threads contending, audio + editor lock each other up. The
         * editor thread now calls setParameter from its idle loop. */

        /* Pull mic input if active. WAIT for a full block; never zero-pad
         * mid-block. The mic Oboe burst is much smaller than kBlockSize, so
         * a "best-effort pull" would yield blocks like [signal..,0,0,..],
         * generating a ~93 Hz envelope that sounds like nasty digital
         * crackle even at low input levels. Pacing the loop on input
         * availability makes vst_host run at the mic rate naturally. */
        if (__atomic_load_n(&shm->mic_active, __ATOMIC_RELAXED)) {
            uint64_t inh = 0, int_ = 0, avail = 0;
            int waited_ms = 0;
            for (;;) {
                if (shm->stop_flag) goto teardown;
                inh   = __atomic_load_n(&shm->audio_in_head, __ATOMIC_ACQUIRE);
                int_  = __atomic_load_n(&shm->audio_in_tail, __ATOMIC_RELAXED);
                avail = inh - int_;
                if (avail >= kBlockSize) break;
                Sleep(1);
                if (++waited_ms > 100) break;  /* mic stalled — emit silence */
            }
            if (avail >= kBlockSize) {
                /* Cap latency: if backlog grew, drop oldest. */
                if (avail > (uint64_t)kBlockSize * 4) {
                    int_ = inh - (uint64_t)kBlockSize * 2;
                }
                for (int i = 0; i < kBlockSize; i++) {
                    uint64_t idx = (int_ + (uint64_t)i) & (VSTPOC_AUDIO_RING_FRAMES - 1);
                    in_l[i] = shm->audio_in[idx * 2 + 0];
                    in_r[i] = shm->audio_in[idx * 2 + 1];
                }
                __atomic_store_n(&shm->audio_in_tail,
                                 int_ + (uint64_t)kBlockSize, __ATOMIC_RELEASE);
            } else {
                for (int i = 0; i < kBlockSize; i++) { in_l[i] = 0; in_r[i] = 0; }
            }
        } else {
            for (int i = 0; i < kBlockSize; i++) { in_l[i] = 0; in_r[i] = 0; }
        }

        /* Chained DSP: feed input through each plugin in sequence,
         * ping-ponging between (in_l/r) and (out_l/r) as scratch
         * buffers. With N plugins, the chain looks like:
         *   in → plugin[0] → out
         *   out → plugin[1] → in
         *   in → plugin[2] → out
         *   ...
         * After N processReplacing calls, the final pair (in or out
         * depending on parity) is copied to out_l/out_r.
         *
         * processReplacing is allowed to write to its output buffers
         * but should NOT modify the input buffers — so ping-ponging
         * between two distinct pairs is safe. */
        {
            float* cur_in[2]  = { in_l,  in_r  };
            float* cur_out[2] = { out_l, out_r };
            for (int p = 0; p < g_pluginCount; p++) {
                AEffect* pe = g_plugins[p].eff;
                if (!pe) continue;
                pe->processReplacing(pe, cur_in, cur_out, kBlockSize);
                /* Swap: this plugin's output becomes next plugin's input. */
                float* tmp;
                tmp = cur_in[0]; cur_in[0] = cur_out[0]; cur_out[0] = tmp;
                tmp = cur_in[1]; cur_in[1] = cur_out[1]; cur_out[1] = tmp;
            }
            /* After the loop, cur_in[] points at the FINAL output (we
             * swapped after the last processReplacing). If that's not
             * out_l/out_r already, copy. */
            if (cur_in[0] != out_l) {
                for (int i = 0; i < kBlockSize; i++) {
                    out_l[i] = cur_in[0][i];
                    out_r[i] = cur_in[1][i];
                }
            }
        }

        /* push output, dropping samples if ring full */
        uint64_t ah = __atomic_load_n(&shm->audio_head, __ATOMIC_RELAXED);
        uint64_t at = __atomic_load_n(&shm->audio_tail, __ATOMIC_ACQUIRE);
        uint64_t space = VSTPOC_AUDIO_RING_FRAMES - (ah - at);
        uint64_t push = space < kBlockSize ? space : kBlockSize;
        for (uint64_t i = 0; i < push; i++) {
            uint64_t idx = (ah + i) & (VSTPOC_AUDIO_RING_FRAMES - 1);
            shm->audio[idx * 2 + 0] = out_l[i];
            shm->audio[idx * 2 + 1] = out_r[i];
        }
        __atomic_store_n(&shm->audio_head, ah + push, __ATOMIC_RELEASE);
        __atomic_add_fetch(&shm->guest_frames_produced, push, __ATOMIC_RELAXED);

        /* throttle: don't melt the CPU if the host isn't consuming */
        if (space < kBlockSize) Sleep(1);
    }

teardown:
    LOG("stop_flag set; shutting down (frames_produced=%llu)\n",
        (unsigned long long)shm->guest_frames_produced);
    /* Tear down each plugin in reverse order. */
    for (int i = g_pluginCount - 1; i >= 0; i--) {
        AEffect* e = g_plugins[i].eff;
        if (!e) continue;
        e->dispatcher(e, /*effStopProcess=*/72, 0, 0, NULL, 0.0f);
        e->dispatcher(e, /*effMainsChanged=*/12, 0, 0, NULL, 0.0f);
        e->dispatcher(e, /*effClose=*/1, 0, 0, NULL, 0.0f);
        FreeLibrary(g_plugins[i].module);
    }
    return 0;
}
