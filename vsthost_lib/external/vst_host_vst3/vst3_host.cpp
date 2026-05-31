/*
 * vst3_host.exe — Windows PE that runs under wine inside Android, loads
 * a VST3 plugin via Steinberg's SDK helpers, and pumps audio through it
 * via the shared-memory ring the launcher allocates.
 *
 * argv layout (kept compatible with vst_host.exe):
 *   argv[1]   = shared-memory file path (Windows-side, already created)
 *   argv[2..] = plugin path(s) (Windows-side). First arg = .vst3 bundle.
 *
 * The launcher (WineHostProcess.cpp) picks vst3_host vs vst_host based on
 * the plugin file extension. From the launcher's perspective, both speak
 * the same VstpocShared protocol so audio + status + parameter ring are
 * format-agnostic.
 *
 * This is a MINIMAL VST3 host — single plugin, stereo in/out, no MIDI, no
 * GUI yet (GUI lands in next iteration). Editor view comes from
 * IEditController::createView("editor") + IPlugView::attached(hwnd,"HWND")
 * which we wire to the same DesktopWindow path vst_host.c uses.
 */

/* shared_layout.h is C11 — uses _Alignas. Shim it to C++'s alignas so g++
 * compiles the struct identically. (Same alignment, just different keyword.) */
#ifdef __cplusplus
extern "C" {
#define _Alignas(x) alignas(x)
#include "shared_layout.h"
#undef _Alignas
}
#else
#include "shared_layout.h"
#endif

/* Wine + Windows headers (we're a Windows PE running under wine). */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <atomic>

/* Steinberg VST3 SDK */
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vsttypes.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/common/memorystream.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

/* Logging — same pattern as vst_host.c: prefix-then-flush so we get
 * partial lines even if the host crashes mid-message. */
#define LOG(...) do { fprintf(stderr, "[vst3_host] " __VA_ARGS__); fflush(stderr); } while (0)

/* Shared-memory pointer for the VEH to use during crash logging. */
static volatile VstpocShared* g_shm = NULL;

/* ----- VEH pattern catalog ------------------------------------------------
 *
 * Surgical-skip patterns for known crash sites in commercial plugins.
 *
 * Adding a new pattern: append one entry to g_veh_patterns[]. No code
 * changes required. See feedback_plugin_debug_infrastructure.md for the
 * step-by-step recipe.
 *
 * The patterns are tried in declaration order. The FIRST match wins.
 * Patterns with `exact_pc != 0` only fire when the fault PC matches
 * exactly (used for plugins with no/predictable ASLR like TH-U). Patterns
 * with `exact_pc == 0` are byte-pattern matches against the bytes at PC
 * (used for ASLR-randomised plugins like TONEX).
 *
 * If NO pattern matches, the VEH falls back to two policies:
 *   - WRITE-to-NULL  → log diagnostic + ExitThread (host stays alive)
 *   - READ-from-NULL → log diagnostic + ExitThread
 *
 * Both fallbacks emit the same byte-dump + register-dump format so a new
 * crash site can be added to the catalog with one struct literal.
 *
 * Memory cross-references:
 *   [[feedback_thu_veh_surgical_recovery]]
 *   [[feedback_tonex_vst3_editor_stall]] (the "2026-05-28 ~21:00" entry
 *     for why TONEX needs rax→zero-scratch not just rax=0)
 *   [[feedback_thu_popup_fix_landed]]
 * ------------------------------------------------------------------------ */

namespace {

/* Static zero buffer for RaxToZeroScratch recovery (TONEX). 256-byte
 * aligned, 4KB long — covers any struct offset the downstream code is
 * likely to walk. Lives in .bss; no per-call allocation. */
alignas(256) static unsigned char g_zero_scratch[4096] = {};

enum class Recovery : unsigned char {
    /* Skip a fixed number of bytes (Rip += skip_bytes). Combine with
     * zero_regs_mask to clear destination regs (matches TH-U +0x2A05AF
     * pattern that loaded rax = [rax]). */
    Skip,
    /* Jump to an exact address (set_rip). Used for TH-U where the skip
     * needs to land past a specific safe-resume point, not just N bytes
     * ahead. */
    JumpTo,
    /* TONEX-specific: like Skip, but also (1) memset 16 bytes at
     * [rsi + memset_rsi_offset] to emulate the original movups store the
     * skip would otherwise lose, and (2) point Rax at the static zero
     * scratch buffer so downstream native ARM64 wine code that derefs Rax
     * gets a real zero page instead of a NULL crash. */
    RaxToZeroScratch,
};

struct VehPattern {
    const char *name;
    /* ExceptionInformation[0]: 0 = read, 1 = write, 2 = either */
    unsigned char access_kind;
    /* Pattern only fires when fault addr < this. 0x1000 catches the
     * "NULL + small struct offset" common case. */
    USHORT fault_addr_max;
    /* If nonzero, fault PC must equal this. */
    DWORD64 exact_pc;
    /* Bytes at PC must match for byte_count bytes. 0 = skip check. */
    unsigned char byte_count;
    unsigned char bytes[16];
    /* If nonzero, Rax must equal this on entry (catches the common
     * "rax=0 is the trigger" case). Use 0xFFFFFFFFFFFFFFFFULL to mean
     * "don't check Rax". */
    DWORD64 rax_must_be;
    Recovery recovery;
    /* Recovery::Skip / RaxToZeroScratch: bytes to advance Rip by. */
    unsigned char skip_bytes;
    /* Recovery::JumpTo: target Rip. */
    DWORD64 set_rip;
    /* Bit 0=rax, 1=xmm0, 2=xmm1, 8=r14. OR'd registers get zeroed
     * in the ContextRecord before resume. */
    USHORT zero_regs_mask;
    /* RaxToZeroScratch: byte offset from Rsi to memset(0); count of
     * bytes to memset. 0/0 = skip the memset. */
    USHORT memset_rsi_offset;
    USHORT memset_rsi_count;
    /* Live hit counter — atomic since the VEH can be entered from any
     * plugin thread. mutable so the catalog can be declared const. */
    mutable std::atomic<unsigned> hit_count;
};

/* Recovery::Skip+R14 zero is the TH-U +0x2DF0C3 popup-vector-race fix.
 * Recovery::JumpTo is functionally identical to Skip when skip lands
 * inside the same basic block, but we keep both for clarity.
 *
 * Note: zero_regs_mask uses bit 8 for R14 since r0..r3 are bits 0..3 in
 * our scheme; we reserve bits 0..7 for rax..r9 and shift R14 up. */
static constexpr unsigned ZR_RAX   = 1u << 0;
static constexpr unsigned ZR_XMM0  = 1u << 1;
static constexpr unsigned ZR_XMM1  = 1u << 2;
static constexpr unsigned ZR_R14   = 1u << 8;

static VehPattern g_veh_patterns[] = {
    /* TH-U +0x2DF0C3: popup-dismiss vector race. Hard-coded address (no
     * ASLR for TH-U since DllOverrides force a fixed module base). */
    {
        "thu-popup-dismiss",
        /* read */ 0,
        /* fault_addr_max */ 0x1000,
        /* exact_pc */ 0x1802DF0C3ULL,
        /* byte_count */ 4,
        /* bytes */ { 0x4c, 0x8b, 0x34, 0x07 },
        /* rax_must_be */ 0xFFFFFFFFFFFFFFFFULL,
        Recovery::JumpTo,
        /* skip_bytes */ 0,
        /* set_rip */ 0x1802DF0E3ULL,
        /* zero_regs_mask */ ZR_R14,
        /* memset_rsi_* */ 0, 0,
        /* hit_count */ {0},
    },
    /* TH-U +0x2A05AF: effect-removal NULL get_item. */
    {
        "thu-effect-removal-null",
        /* read */ 0,
        /* fault_addr_max */ 0x1000,
        /* exact_pc */ 0x1802A05AFULL,
        /* byte_count */ 3,
        /* bytes */ { 0x48, 0x8b, 0x10 },
        /* rax_must_be */ 0xFFFFFFFFFFFFFFFFULL,
        Recovery::JumpTo,
        /* skip_bytes */ 0,
        /* set_rip */ 0x1802A05B8ULL,
        /* zero_regs_mask */ ZR_RAX,
        /* memset_rsi_* */ 0, 0,
        /* hit_count */ {0},
    },
    /* TONEX setProcessing-worker NULL controller. ASLR'd, so we match
     * the 16-byte pattern. Recovery emulates a zero-init struct:
     *   movaps xmm0, [rax+0x20]   → xmm0 = 0
     *   movaps xmm1, [rax+0x30]   → xmm1 = 0
     *   mov    rax, [rax+0x40]    → rax  = &g_zero_scratch (NOT 0; see
     *                                       feedback_tonex_vst3_editor_stall
     *                                       "2026-05-28 ~21:00" entry)
     *   movups [rsi+0x70], xmm1   → memset(rsi+0x70, 0, 16) (so wine
     *                                ntdll ARM64 STUR loop at module
     *                                +FE7784 reads valid zeros, not
     *                                stale stack)
     */
    {
        "tonex-null-controller",
        /* read */ 0,
        /* fault_addr_max */ 0x1000,
        /* exact_pc */ 0,    /* ASLR, match by bytes only */
        /* byte_count */ 16,
        /* bytes */ {
            0x0f, 0x28, 0x40, 0x20,
            0x0f, 0x28, 0x48, 0x30,
            0x48, 0x8b, 0x40, 0x40,
            0x0f, 0x11, 0x4e, 0x70,
        },
        /* rax_must_be */ 0,
        Recovery::RaxToZeroScratch,
        /* skip_bytes */ 16,
        /* set_rip */ 0,
        /* zero_regs_mask */ ZR_XMM0 | ZR_XMM1,
        /* memset_rsi_offset */ 0x70,
        /* memset_rsi_count */ 16,
        /* hit_count */ {0},
    },
};

/* Helper: VirtualQuery + state checks for safe memory access. */
static bool addr_is_committed(const void *addr)
{
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi)
        && mbi.State == MEM_COMMIT
        && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}

static bool addr_is_writable(const void *addr)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

/* Apply zero_regs_mask to ContextRecord. */
static void zero_regs(PCONTEXT ctx, USHORT mask)
{
    if (mask & ZR_RAX)  ctx->Rax = 0;
    if (mask & ZR_XMM0) { ctx->Xmm0.Low = 0; ctx->Xmm0.High = 0; }
    if (mask & ZR_XMM1) { ctx->Xmm1.Low = 0; ctx->Xmm1.High = 0; }
    if (mask & ZR_R14)  ctx->R14 = 0;
}

/* Try to match `info` against a single catalog entry. Return true if
 * matched + recovery applied (caller returns EXCEPTION_CONTINUE_EXECUTION). */
static bool try_pattern(PEXCEPTION_POINTERS info, const VehPattern &p)
{
    /* access_kind: 0=read, 1=write, 2=either */
    if (p.access_kind != 2) {
        if (info->ExceptionRecord->ExceptionInformation[0] != p.access_kind)
            return false;
    }
    if (info->ExceptionRecord->ExceptionInformation[1] >= p.fault_addr_max)
        return false;
    if (p.exact_pc != 0
        && info->ExceptionRecord->ExceptionAddress != (PVOID)p.exact_pc)
        return false;
    if (p.rax_must_be != 0xFFFFFFFFFFFFFFFFULL
        && info->ContextRecord->Rax != p.rax_must_be)
        return false;
    if (p.byte_count > 0) {
        const void *pc = info->ExceptionRecord->ExceptionAddress;
        if (!addr_is_committed(pc)) return false;
        if (memcmp(pc, p.bytes, p.byte_count) != 0) return false;
    }

    /* Match — apply recovery. */
    PCONTEXT ctx = info->ContextRecord;
    zero_regs(ctx, p.zero_regs_mask);

    switch (p.recovery) {
        case Recovery::Skip:
            ctx->Rip += p.skip_bytes;
            break;
        case Recovery::JumpTo:
            ctx->Rip = p.set_rip;
            break;
        case Recovery::RaxToZeroScratch: {
            /* Emulate the lost write at [rsi + memset_rsi_offset] as zeros
             * (the NULL-controller field xmm1 would have been zero). This
             * is the proven TONEX recovery. NOTE: a sink-pointer variant
             * was tried for AmpliTube (whose downstream crashes at native
             * ARM64 wine +FE7784) and did NOT help — AmpliTube's NULL
             * pointer does not originate from this slot, and the +FE7784
             * fault is unrecoverable from our x86-shaped VEH anyway. Kept
             * the simple zero memset to protect TONEX's working path. */
            if (p.memset_rsi_count > 0 && ctx->Rsi != 0) {
                void *dst = (char*)ctx->Rsi + p.memset_rsi_offset;
                if (addr_is_writable(dst))
                    memset(dst, 0, p.memset_rsi_count);
            }
            ctx->Rax = (DWORD64)&g_zero_scratch[0];
            ctx->Rip += p.skip_bytes;
            break;
        }
    }

    /* Record + log. Use atomic OR for hit_count; cheap, lock-free. */
    unsigned prior = p.hit_count.fetch_add(1, std::memory_order_relaxed);
    LOG("VEH: surgical skip %s (pc=%p, hit#%u)\n",
        p.name, info->ExceptionRecord->ExceptionAddress, prior + 1);

    /* Mirror the hit into VstpocShared so the Android side can read
     * which surgical-skips fired without parsing the wine log. Bit N =
     * pattern N in g_veh_patterns. Done here (not in caller) so the
     * mapping stays adjacent to the table. */
    if (g_shm != nullptr) {
        size_t idx = static_cast<size_t>(&p - &g_veh_patterns[0]);
        if (idx < 64) {
            uint64_t bit = (uint64_t)1 << idx;
            /* Lock-free OR on the shared-mem field. The host reads it
             * with relaxed semantics — no race on missing bits matters,
             * the worst case is a momentarily-stale view. */
            __atomic_or_fetch(
                (uint64_t*)&g_shm->veh_patterns_hit_bitmask,
                bit, __ATOMIC_RELAXED);
        }
    }
    return true;
}

/* Diagnostic fallback for an unmatched AV. Logs bytes + regs so the
 * crash can be added to the catalog with one struct literal. Returns
 * after ExitThread (does not return to caller in practice). */
[[noreturn]] static void log_and_exit_thread(PEXCEPTION_POINTERS info,
                                              const char *kind)
{
    const unsigned char *pc =
        (const unsigned char *)info->ExceptionRecord->ExceptionAddress;
    unsigned char bytes[16] = {0};
    if (addr_is_committed(pc))
        for (int i = 0; i < 16; i++) bytes[i] = pc[i];

    LOG("VEH: %s AV pc=%p fault=0x%llx (terminating thread)\n",
        kind, info->ExceptionRecord->ExceptionAddress,
        (unsigned long long)info->ExceptionRecord->ExceptionInformation[1]);
    LOG("VEH: bytes@pc: "
        "%02x %02x %02x %02x %02x %02x %02x %02x "
        "%02x %02x %02x %02x %02x %02x %02x %02x\n",
        bytes[0],  bytes[1],  bytes[2],  bytes[3],
        bytes[4],  bytes[5],  bytes[6],  bytes[7],
        bytes[8],  bytes[9],  bytes[10], bytes[11],
        bytes[12], bytes[13], bytes[14], bytes[15]);
    LOG("VEH: rax=%llx rcx=%llx rdx=%llx rbx=%llx "
        "rsi=%llx rdi=%llx r8=%llx r9=%llx\n",
        (unsigned long long)info->ContextRecord->Rax,
        (unsigned long long)info->ContextRecord->Rcx,
        (unsigned long long)info->ContextRecord->Rdx,
        (unsigned long long)info->ContextRecord->Rbx,
        (unsigned long long)info->ContextRecord->Rsi,
        (unsigned long long)info->ContextRecord->Rdi,
        (unsigned long long)info->ContextRecord->R8,
        (unsigned long long)info->ContextRecord->R9);
    ExitThread(0xDEADBEEF);
}

} /* anonymous namespace */

static LONG WINAPI pluginmain_veh(PEXCEPTION_POINTERS info)
{
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION
        || info->ExceptionRecord->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;

    const ULONG_PTR access = info->ExceptionRecord->ExceptionInformation[0];
    const ULONG_PTR fault  = info->ExceptionRecord->ExceptionInformation[1];

    /* Only act on NULL-ish faults. Real wild-pointer writes higher up
     * fall through to wine's default handler. */
    if (fault >= 0x1000) return EXCEPTION_CONTINUE_SEARCH;

    /* Try the catalog. First-match wins. */
    for (const auto &p : g_veh_patterns) {
        if (try_pattern(info, p))
            return EXCEPTION_CONTINUE_EXECUTION;
    }

    /* No pattern matched. Log diagnostic + kill thread. */
    log_and_exit_thread(info,
                         (access == 0) ? "READ-from-NULL" : "WRITE-to-NULL");
}

/* ----- Shared-memory mapping (identical to vst_host.c map_shared) ---- */
static VstpocShared* map_shared(const char* path)
{
    HANDLE fh = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        LOG("CreateFileA(%s) failed: %lu\n", path, (unsigned long)GetLastError());
        return NULL;
    }
    DWORD lo = (DWORD)(sizeof(VstpocShared) & 0xffffffff);
    DWORD hi = (DWORD)(sizeof(VstpocShared) >> 32);
    HANDLE mh = CreateFileMappingA(fh, NULL, PAGE_READWRITE, hi, lo, NULL);
    if (!mh) {
        LOG("CreateFileMappingA failed: %lu\n", (unsigned long)GetLastError());
        CloseHandle(fh);
        return NULL;
    }
    VstpocShared* shm = (VstpocShared*)MapViewOfFile(mh, FILE_MAP_ALL_ACCESS, 0, 0,
                                                     sizeof(VstpocShared));
    CloseHandle(mh);
    CloseHandle(fh);
    return shm;
}

static void write_status(VstpocShared* shm, int code, const char* msg)
{
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

/* ---- Minimal IPlugFrame ----------------------------------------------
 * JUCE-based VST3 plugins (and many others) require the host to install
 * an IPlugFrame BEFORE calling attached(); the plugin queries it during
 * attach to request the host's permission to resize / etc. Without it,
 * attached() can deadlock waiting for the frame to be set.
 *
 * We implement the minimum: queryInterface + refcount + resizeView. The
 * resizeView callback just acknowledges the request and tells the view to
 * apply the size — for our use case there's no "host window" to resize,
 * the plugin owns its drawing area inside the wine desktop window. */
class MinimalPlugFrame : public IPlugFrame
{
public:
    MinimalPlugFrame() = default;
    virtual ~MinimalPlugFrame() = default;

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) SMTG_OVERRIDE
    {
        if (FUnknownPrivate::iidEqual(iid, IPlugFrame::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid))
        {
            addRef();
            *obj = static_cast<IPlugFrame*>(this);
            return kResultTrue;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE
    {
        return ++refCount_;
    }
    uint32 PLUGIN_API release() SMTG_OVERRIDE
    {
        uint32 c = --refCount_;
        if (c == 0) delete this;
        return c;
    }

    /* The plugin uses this to request its parent (us) to resize. We
     * acknowledge and tell the view to apply the new size. */
    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* newSize) SMTG_OVERRIDE
    {
        if (view && newSize) {
            view->onSize(newSize);
            /* Publish new size to SHM so Android can resize its surface. */
            if (g_shm) {
                g_shm->editor_width  = newSize->right - newSize->left;
                g_shm->editor_height = newSize->bottom - newSize->top;
                __sync_synchronize();
            }
        }
        return kResultTrue;
    }

private:
    std::atomic<uint32> refCount_{1};
};

/* ---- Minimal IComponentHandler --------------------------------------
 * The SDK's HostApplication implements ONLY IHostApplication, NOT
 * IComponentHandler — so without this, editController->setComponentHandler
 * is never called and the controller has a NULL handler. Many JUCE-based
 * plugins store the handler at initialize/setComponentState time and
 * dereference it during setProcessing; a NULL handler then surfaces as a
 * NULL-controller-ish crash in the setProcessing worker (the
 * "tonex-null-controller" VEH pattern — shared by TONEX and AmpliTube).
 *
 * All callbacks are no-ops returning kResultOk: we don't surface plugin
 * parameter automation back to a DAW (there's no DAW — the audio path is
 * the shm ring), we just need a non-NULL, well-formed handler so the
 * plugin's controller initialises fully. Stack-allocated for the life of
 * main(), so refcounting is a no-op (returns a sentinel). */
class HostComponentHandler : public IComponentHandler, public IComponentHandler2 {
public:
    /* --- IComponentHandler --- */
    tresult PLUGIN_API beginEdit (ParamID) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API performEdit (ParamID, ParamValue) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API endEdit (ParamID) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API restartComponent (int32) SMTG_OVERRIDE { return kResultOk; }

    /* --- IComponentHandler2 (the interface TH-U needs) ---
     * vstpoc 2026-05-29: confirmed via denied-IID logging that TH-U queries
     * the handler for IComponentHandler2 (iid b3b440f0-...) exactly once at
     * init. Our minimal host denied it (kNoInterface); TH-U stored the NULL
     * and later dereferenced it during processing -> READ-from-NULL AV at
     * TH-U+0x2ab5f5 (`mov rax,[rbx]`, rbx=0) -> VEH terminates the thread ->
     * no audio. A real DAW host ALWAYS provides IComponentHandler2, so the
     * correct fix is to provide it. IComponentHandler2 is a SIBLING of
     * IComponentHandler under FUnknown (NOT a subclass) — hence multiple
     * inheritance, with queryInterface below disambiguating the two FUnknown
     * base subobjects. No-op callbacks returning kResultOk are valid host
     * behavior (we have no project to mark dirty / no editor-open flow). */
    tresult PLUGIN_API setDirty (TBool /*state*/) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API requestOpenEditor (FIDString /*name*/) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API startGroupEdit () SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API finishGroupEdit () SMTG_OVERRIDE { return kResultOk; }

    /* --- FUnknown (single set of overriders for both base subobjects) --- */
    tresult PLUGIN_API queryInterface (const TUID iid, void** obj) SMTG_OVERRIDE {
        if (FUnknownPrivate::iidEqual(iid, FUnknown::iid)
         || FUnknownPrivate::iidEqual(iid, IComponentHandler::iid)) {
            *obj = static_cast<IComponentHandler*>(this);
            return kResultOk;
        }
        if (FUnknownPrivate::iidEqual(iid, IComponentHandler2::iid)) {
            *obj = static_cast<IComponentHandler2*>(this);
            return kResultOk;
        }
        /* Keep logging any host-interface IID we still don't provide, so a
         * future plugin that needs a different one surfaces it the same way
         * TH-U's IComponentHandler2 did. */
        const unsigned char* b = (const unsigned char*)iid;
        LOG("HostComponentHandler::queryInterface DENIED iid="
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
            b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
        *obj = nullptr;
        return kNoInterface;
    }
    /* Stack object — never actually freed via refcount. Return a high
     * sentinel so a stray release() doesn't think it hit zero. */
    uint32 PLUGIN_API addRef () SMTG_OVERRIDE { return 1000; }
    uint32 PLUGIN_API release () SMTG_OVERRIDE { return 1000; }
};

/* ---- Editor thread state --------------------------------------------
 * VST3 editor lives on the main thread, but to keep parity with vst_host.c
 * (which spawns CreateWindowExA on a dedicated thread for wine UI ordering
 * reasons) we run the editor message loop on its own thread too. The view
 * itself is created on the main thread, then attached() is called on the
 * editor thread which owns the message pump. */
struct EditorThreadCtx {
    IPlugView*  view;
    HWND        parent;
    int32       width;
    int32       height;
};

/* Minimal WndProc — VST3 plugins create their own child window inside the
 * HWND we pass to attached(); they handle paints/input there. We just need
 * a valid frame for them to embed into.
 *
 * Explicitly return 0 for WM_CREATE/WM_NCCREATE so wine doesn't interpret
 * a non-zero DefWindowProc return as "abort window creation" — observed
 * with TONEX where CreateWindowExA was returning ERROR_INVALID_WINDOW_HANDLE
 * because the window was being destroyed immediately after WM_CREATE.
 */
static LRESULT CALLBACK editor_host_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_NCCREATE: return TRUE;   /* must return TRUE to continue creation */
        case WM_CREATE:   return 0;      /* 0 = success, -1 = abort */
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static DWORD WINAPI editor_thread_proc(LPVOID arg)
{
    EditorThreadCtx* ctx = (EditorThreadCtx*)arg;

    /* Force wine to initialize this thread's UI state (window station +
     * desktop + display driver attachment). Without this, the first
     * CreateWindowExA on the editor thread can fail with
     * ERROR_INVALID_WINDOW_HANDLE (1400, "nodrv_CreateWindow") because
     * the X11 connection isn't established until SOMETHING in this
     * thread touches the message system. PeekMessageA returns immediately
     * but as a side effect runs the thread's user32/win32u init. */
    MSG dummy_msg;
    PeekMessageA(&dummy_msg, NULL, 0, 0, PM_NOREMOVE);
    /* Also touch GetDesktopWindow — forces explorer.exe/desktop window
     * acquisition for threads that need it. */
    HWND desktop = GetDesktopWindow();
    LOG("editor: GetDesktopWindow = %p (must be non-NULL for CreateWindowExA)\n",
        (void*)desktop);

    /* Register a window class for our editor host frame. VST3 plugins (TAL,
     * JUCE-based, etc.) expect a real HWND with a WndProc backing it — not
     * the global desktop window. The sample editorhost does exactly this. */
    WNDCLASSA wc{};
    wc.lpfnWndProc   = editor_host_wndproc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "VstpocVst3EditorHost";
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    RegisterClassA(&wc);

    /* WS_POPUP = no title bar, no border. The plugin's IPlugView creates
     * its own child window inside ours and draws all the chrome it wants.
     * Showing the Win32 caption/sysmenu around the plugin GUI is just
     * visual noise on Android. */
    HWND parent = CreateWindowExA(
        0,
        "VstpocVst3EditorHost",
        "vstpoc-vst3-editor",
        WS_POPUP,
        0, 0, ctx->width, ctx->height,
        NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!parent) {
        LOG("editor: CreateWindowExA failed: %lu\n", (unsigned long)GetLastError());
        return 1;
    }
    LOG("editor: created host hwnd=%p (size %dx%d)\n",
        parent, ctx->width, ctx->height);

    /* Show the host window — under wine on Android this maps to a real
     * X11 window our X11NativeDisplay attaches to its render surface. */
    ShowWindow(parent, SW_SHOW);
    UpdateWindow(parent);

    /* Publish editor size BEFORE attached(). attached() can block for many
     * seconds while the plugin loads its assets (AmpCraft scans Kazrog IR
     * directories — observed 5–10s). Writing editor_width here gives the
     * Android-side VstInlineEditor the dimensions it needs to mount the
     * SurfaceView immediately; the plugin then renders into the already-
     * attached surface once attached() completes. */
    if (g_shm) {
        g_shm->editor_width  = (int32_t)ctx->width;
        g_shm->editor_height = (int32_t)ctx->height;
        __sync_synchronize();
    }

    LOG("editor: calling attached(hwnd=%p, HWND)\n", parent);
    tresult ar = ctx->view->attached((void*)parent, kPlatformTypeHWND);
    LOG("editor: attached result=0x%x\n", (unsigned)ar);

    if (ar != kResultTrue && ar != kResultOk) {
        LOG("editor: attach failed, destroying host window\n");
        DestroyWindow(parent);
        return 1;
    }

    /* Record which graphics APIs the plugin actually loaded, now that the
     * editor has been attached (DLLs are resolved by this point). This
     * feeds the black-screen detector: a plugin that loaded none of these
     * AND storms WM_USER+123 without painting is stuck. Checked once. */
    if (g_shm) {
        uint32_t apis = 0;
        if (GetModuleHandleA("d3d11.dll"))   apis |= 1u << 0;
        if (GetModuleHandleA("d3d9.dll"))    apis |= 1u << 1;
        if (GetModuleHandleA("opengl32.dll"))apis |= 1u << 2;
        /* gdi32 is loaded passively by combase etc., so its presence is a
         * weak signal — only flag it if NO accelerated API loaded (i.e.
         * the plugin is GDI-only). */
        if (apis == 0 && GetModuleHandleA("gdi32.dll")) apis |= 1u << 3;
        if (apis == 0) apis |= 1u << 4;  /* none observed */
        g_shm->render_api_used = apis;
        __sync_synchronize();
    }

    LOG("editor: entering message pump\n");
    /* Standard wine message pump — JUCE/Qt/etc. all dispatch their input
     * through here so the editor stays interactive. */
    MSG msg;
    /* Health instrumentation: count WM_USER+123 (JUCE async-update storm
     * signature) and WM_PAINT in a rolling 1-second window so the Android
     * side can detect a stuck editor without parsing the wine log. */
    uint64_t wm_user_window = 0;
    uint64_t wm_paint_total = 0;
    DWORD window_start = GetTickCount();
    while (!(g_shm && g_shm->stop_flag)) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_PAINT) {
                wm_paint_total++;
                if (g_shm) g_shm->wm_paint_count = wm_paint_total;
            } else if (msg.message == (WM_USER + 123)) {
                wm_user_window++;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        /* Flush the rolling rate once per second. */
        DWORD now = GetTickCount();
        if (now - window_start >= 1000) {
            if (g_shm) {
                g_shm->wm_user_storm_per_second = (uint32_t)wm_user_window;
                __sync_synchronize();
            }
            wm_user_window = 0;
            window_start = now;
        }
        Sleep(5);
    }

    LOG("editor: closing view\n");
    ctx->view->removed();
    DestroyWindow(parent);
    return 0;
}

/* ----- VST3 plumbing ---- */

/* Find the first class with category "Audio Module Class" in the factory's
 * class list. That's the plugin's audio processor. */
static bool find_audio_class(IPluginFactory* factory, FUID& outCid, std::string& outName)
{
    int32 nClasses = factory->countClasses();
    LOG("factory has %d classes\n", (int)nClasses);

    for (int32 i = 0; i < nClasses; i++) {
        PClassInfo info;
        if (factory->getClassInfo(i, &info) != kResultOk) continue;
        LOG("  class %d: category=%s name=%s\n", (int)i, info.category, info.name);

        if (strcmp(info.category, kVstAudioEffectClass) == 0) {
            outCid = FUID::fromTUID(info.cid);
            outName = info.name;
            return true;
        }
    }
    return false;
}

/* Activate the first stereo audio bus in each direction. VST3 plugins can
 * have many buses; we wire the primary input + output to our 2-channel ring. */
static void activate_main_buses(IComponent* comp)
{
    int32 nIn  = comp->getBusCount(kAudio, kInput);
    int32 nOut = comp->getBusCount(kAudio, kOutput);
    LOG("buses: audio in=%d out=%d\n", (int)nIn, (int)nOut);
    if (nIn  > 0) comp->activateBus(kAudio, kInput,  0, true);
    if (nOut > 0) comp->activateBus(kAudio, kOutput, 0, true);
}

/* Process one block of stereo audio. Uses the existing ring layout —
 * deinterleave from `audio_in`, deinterleave/reinterleave through VST3
 * (which wants per-channel buffers), interleave back into `audio`. */
static void process_block(IAudioProcessor* processor,
                          const float* in_l, const float* in_r,
                          float* out_l, float* out_r,
                          int32 nFrames)
{
    /* Per-bus buffers. VST3 wants ChannelBuffers32::channelBuffers32 to
     * point at per-channel float* arrays. */
    AudioBusBuffers inBus;
    AudioBusBuffers outBus;
    float* inChans[2]  = { (float*)in_l,  (float*)in_r  };
    float* outChans[2] = { out_l, out_r };

    inBus.numChannels       = 2;
    inBus.silenceFlags      = 0;
    inBus.channelBuffers32  = inChans;

    outBus.numChannels      = 2;
    outBus.silenceFlags     = 0;
    outBus.channelBuffers32 = outChans;

    /* Empty parameter and event lists — fine for initial testing, we
     * route DAW parameter changes via the existing setParameter path
     * once we wire it up in the audio loop below. */
    ParameterChanges noParams;
    EventList        noEvents;

    ProcessData data;
    data.processMode          = kRealtime;
    data.symbolicSampleSize   = kSample32;
    data.numSamples           = nFrames;
    data.numInputs            = 1;
    data.numOutputs           = 1;
    data.inputs               = &inBus;
    data.outputs              = &outBus;
    data.inputParameterChanges  = &noParams;
    data.outputParameterChanges = nullptr;
    data.inputEvents          = &noEvents;
    data.outputEvents         = nullptr;
    data.processContext       = nullptr;

    processor->process(data);
}

/* ----- Main entry point ----------------------------------------------*/
int main(int argc, char** argv)
{
    LOG("vst3_host starting (argc=%d)\n", argc);

    if (argc < 3) {
        LOG("usage: vst3_host.exe <shm_path> <plugin.vst3> [plugin.vst3 ...]\n");
        return 1;
    }

    /* Wire up SHM first so errors are visible to the launcher even if
     * the plugin never loads. */
    g_shm = map_shared(argv[1]);
    if (!g_shm) {
        LOG("shared-memory mapping failed; running detached\n");
        /* Continue anyway — for diagnostics-only runs. */
    } else {
        /* Announce that this guest build writes the health fields, so the
         * Android side knows reads of dxvk_init_status/render_api_used/etc.
         * are meaningful (0 = legacy guest that never wrote them). */
        g_shm->diagnostic_layout_v = 1;
        __sync_synchronize();
    }
    write_status((VstpocShared*)g_shm, 0, "loading VST3");

    /* VEH lives for the whole process so per-thread crashes get caught. */
    AddVectoredExceptionHandler(/*CALL_FIRST*/1, pluginmain_veh);

    /* Load the (first) plugin. Multi-plugin chain support comes later. */
    const char* pluginPath = argv[2];
    LOG("loading: %s\n", pluginPath);

    std::string err;
    VST3::Hosting::Module::Ptr module = VST3::Hosting::Module::create(pluginPath, err);
    if (!module) {
        LOG("module load failed: %s\n", err.c_str());
        write_status((VstpocShared*)g_shm, 2, err.c_str());
        return 2;
    }
    LOG("module loaded: %s\n", module->getName().c_str());

    auto factory = module->getFactory();
    IPluginFactory* pf = factory.get();

    FUID classId;
    std::string className;
    if (!find_audio_class(pf, classId, className)) {
        LOG("no audio class found in factory\n");
        write_status((VstpocShared*)g_shm, 2, "no audio class in VST3");
        return 3;
    }
    LOG("audio class: %s\n", className.c_str());

    /* Instantiate IComponent. */
    IComponent* component = nullptr;
    if (pf->createInstance(classId, IComponent::iid, (void**)&component) != kResultOk
        || !component) {
        LOG("createInstance(IComponent) failed\n");
        write_status((VstpocShared*)g_shm, 2, "createInstance failed");
        return 4;
    }
    LOG("component created\n");

    /* Minimal host context. SDK's HostApplication provides IHostApplication
     * + IComponentHandler defaults — enough to satisfy plugins that don't
     * use advanced features. */
    HostApplication host;
    if (component->initialize(&host) != kResultOk) {
        LOG("component initialize failed\n");
        write_status((VstpocShared*)g_shm, 2, "component init failed");
        component->release();
        return 5;
    }
    LOG("component initialized\n");

    /* IAudioProcessor — same object usually implements both interfaces. */
    FUnknownPtr<IAudioProcessor> processor(component);
    if (!processor) {
        LOG("component does not implement IAudioProcessor\n");
        write_status((VstpocShared*)g_shm, 2, "no IAudioProcessor");
        component->terminate();
        component->release();
        return 6;
    }

    /* IEditController — may be the same object (single-component design)
     * or a separate class identified by the component. */
    IEditController* editController = nullptr;
    TUID controllerCID;
    if (component->getControllerClassId(controllerCID) == kResultOk) {
        FUID ccid = FUID::fromTUID(controllerCID);
        if (pf->createInstance(ccid, IEditController::iid,
                               (void**)&editController) == kResultOk && editController) {
            LOG("edit controller instantiated separately\n");
            if (editController->initialize(&host) != kResultOk) {
                LOG("edit controller initialize failed (continuing without GUI)\n");
                editController->release();
                editController = nullptr;
            }
        }
    }
    if (!editController) {
        /* Try same-object pattern: component itself implements IEditController. */
        component->queryInterface(IEditController::iid, (void**)&editController);
        if (editController) LOG("edit controller is same object as component\n");
    }

    /* Standard VST3 host init steps that were MISSING — JUCE plugins
     * (AmpliTube 5 confirmed) require them before the controller is fully
     * valid, otherwise a NULL handler / un-synced controller state
     * surfaces as the "tonex-null-controller" crash in setProcessing.
     *
     * 1. setComponentHandler: give the controller a non-NULL host
     *    callback object (SDK's HostApplication does NOT implement
     *    IComponentHandler, despite the old comment claiming so).
     * 2. setComponentState: transfer the component's serialized state to
     *    the controller so its parameter model matches the processor.
     *    Done via a MemoryStream round-trip (component->getState →
     *    rewind → controller->setComponentState). */
    static HostComponentHandler s_componentHandler;
    if (editController) {
        tresult sch = editController->setComponentHandler(&s_componentHandler);
        LOG("setComponentHandler returned 0x%x\n", (unsigned)sch);

        MemoryStream stateStream;
        tresult gs = component->getState(&stateStream);
        if (gs == kResultOk) {
            int64 ignored = 0;
            stateStream.seek(0, IBStream::kIBSeekSet, &ignored);
            tresult scs = editController->setComponentState(&stateStream);
            LOG("component->getState -> controller->setComponentState returned 0x%x "
                "(state %lld bytes)\n", (unsigned)scs, (long long)stateStream.getSize());
        } else {
            LOG("component->getState returned 0x%x (skipping setComponentState)\n",
                (unsigned)gs);
        }
    }

    /* Connect Component <-> EditController via IConnectionPoint. JUCE-based
     * plugins (and many others) require this BEFORE createView, because the
     * controller's editor implementation queries component state through
     * the connection. Without it, createView returns NULL silently. */
    if (editController && editController != (IEditController*)component) {
        IConnectionPoint* compCP = nullptr;
        IConnectionPoint* ctrlCP = nullptr;
        component->queryInterface(IConnectionPoint::iid, (void**)&compCP);
        editController->queryInterface(IConnectionPoint::iid, (void**)&ctrlCP);
        if (compCP && ctrlCP) {
            tresult c1 = compCP->connect(ctrlCP);
            tresult c2 = ctrlCP->connect(compCP);
            LOG("component<->controller connection results: comp->ctrl=0x%x ctrl->comp=0x%x\n",
                (unsigned)c1, (unsigned)c2);
        } else {
            LOG("one or both of (component, controller) doesn't implement "
                "IConnectionPoint (compCP=%p ctrlCP=%p) — skipping connect\n",
                (void*)compCP, (void*)ctrlCP);
        }
        if (compCP) compCP->release();
        if (ctrlCP) ctrlCP->release();
    }

    /* Tell the plugin our channel layout BEFORE setActive. Without this
     * the plugin uses its default arrangement — Helix Native defaults to
     * mono input, so feeding it 2 channels via process() produces broken
     * output (no xruns reported because the audio thread still meets its
     * deadlines, but the samples themselves are garbage). 1 stereo in +
     * 1 stereo out matches our shm ring layout. */
    {
        SpeakerArrangement inSA  = SpeakerArr::kStereo;
        SpeakerArrangement outSA = SpeakerArr::kStereo;
        tresult arrRes = processor->setBusArrangements(&inSA, 1, &outSA, 1);
        LOG("setBusArrangements(stereo,stereo) returned 0x%x\n", (unsigned)arrRes);
    }

    /* Audio setup. Use the same 48k/512 the launcher uses by default. */
    ProcessSetup setup;
    setup.processMode        = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = 512;
    setup.sampleRate         = 48000.0;
    if (processor->setupProcessing(setup) != kResultOk) {
        LOG("setupProcessing failed\n");
    }

    activate_main_buses(component);

    /* Set component active = preparation for real-time. */
    if (component->setActive(true) != kResultOk) {
        LOG("setActive(true) failed\n");
    }
    {
        tresult procRes = processor->setProcessing(true);
        LOG("setProcessing(true) returned 0x%x (kResultOk=0)\n", (unsigned)procRes);
    }

    /* Spawn editor thread if we have a controller. */
    HANDLE editorThread = NULL;
    EditorThreadCtx editorCtx{};
    IPlugView* view = nullptr;
    if (editController) {
        view = editController->createView(ViewType::kEditor);
        LOG("createView(\"editor\") returned view=%p\n", (void*)view);
        if (view) {
            tresult hwndOk = view->isPlatformTypeSupported(kPlatformTypeHWND);
            LOG("isPlatformTypeSupported(HWND) returned 0x%x (kResultTrue=0, kResultFalse=1, kNotImplemented=0x80000001)\n",
                (unsigned)hwndOk);
            /* Be permissive: accept both kResultOk (0) and kResultTrue
             * (same value, alias) — and even try attached() if HWND check
             * was kNotImplemented, since some plugins return NotImpl but
             * still accept HWND attach. The actual attach() result tells
             * us if it really worked. */
            bool tryHwnd = (hwndOk == kResultTrue || hwndOk == kResultOk
                            || hwndOk == kNotImplemented);
            if (tryHwnd) {
                /* Install an IPlugFrame BEFORE attached(). JUCE expects
                 * this — without it, attached() hangs because the plugin
                 * tries to resolve the host's resize-policy via the frame. */
                MinimalPlugFrame* frame = new MinimalPlugFrame();
                tresult fRes = view->setFrame(frame);
                LOG("setFrame returned 0x%x\n", (unsigned)fRes);

                ViewRect rect{};
                tresult szRes = view->getSize(&rect);
                LOG("getSize returned 0x%x rect=(%d,%d)-(%d,%d)\n",
                    (unsigned)szRes, (int)rect.left, (int)rect.top,
                    (int)rect.right, (int)rect.bottom);
                editorCtx.view   = view;
                editorCtx.width  = (szRes == kResultOk && rect.right > rect.left)
                                    ? rect.right - rect.left : 800;
                editorCtx.height = (szRes == kResultOk && rect.bottom > rect.top)
                                    ? rect.bottom - rect.top : 600;
                LOG("editor view size: %dx%d (defaulted if getSize failed)\n",
                    (int)editorCtx.width, (int)editorCtx.height);
                editorThread = CreateThread(NULL, 0, editor_thread_proc,
                                            &editorCtx, 0, NULL);
            } else {
                LOG("HWND not supported (result=0x%x); skipping editor\n", (unsigned)hwndOk);
                view->release();
                view = nullptr;
            }
        } else {
            LOG("createView returned NULL; plugin has no editor\n");
        }
    } else {
        LOG("no edit controller; running headless (audio only)\n");
    }

    /* Signal ready to launcher. */
    write_status((VstpocShared*)g_shm, 1, "ready");
    if (g_shm) {
        __sync_synchronize();
        g_shm->guest_ready = 1;
        __sync_synchronize();
    }
    LOG("ready — entering audio loop\n");

    /* Audio loop: consume input ring, process, produce output ring.
     * Also pumps messages on this thread — VST3 plugins (TAL, JUCE, etc.)
     * often expect SOMEONE to be dispatching messages during attached()
     * so wine-internal window operations can complete. Editor thread is
     * blocked in attached() so it can't pump for itself. */
    const int blockFrames = 512;
    float in_l[blockFrames], in_r[blockFrames];
    float out_l[blockFrames], out_r[blockFrames];
    MSG audio_msg;
    /* Audio-loop timing stats: how often the loop spun waiting for input,
     * how often the output ring was full, how long process() took. Logged
     * every ~5 seconds so we can diagnose stuttering despite "no xruns". */
    uint64_t stats_blocks = 0;
    uint64_t stats_wait_loops = 0;
    uint64_t stats_out_full = 0;
    uint64_t stats_partial_pushes = 0;
    uint64_t stats_dropped_frames = 0;
    uint64_t stats_process_us_total = 0;
    uint64_t stats_process_us_max = 0;
    DWORD stats_last_log = GetTickCount();
    while (g_shm && !g_shm->stop_flag) {
        /* Drain wine-internal messages targeted at the main thread (system
         * windows, COM messages, etc.). Without this, plugins that send
         * synchronous wine API calls during attached() can wait forever. */
        while (PeekMessageA(&audio_msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&audio_msg);
            DispatchMessageA(&audio_msg);
        }
        /* Pull a block from the input ring (mic). Producer = launcher. */
        uint64_t ih = __atomic_load_n(&g_shm->audio_in_head, __ATOMIC_ACQUIRE);
        uint64_t it = __atomic_load_n(&g_shm->audio_in_tail, __ATOMIC_RELAXED);
        uint64_t available = ih - it;
        if (available < (uint64_t)blockFrames) {
            ++stats_wait_loops;
            Sleep(1);
            continue;
        }
        /* Cap latency. Same logic as vst_host.c: if the launcher has
         * produced more than 4 blocks of input that we haven't consumed
         * yet, fast-forward the tail to leave 2 blocks of headroom. Without
         * this, any one-time drift at startup (FEX JIT warmup, wineserver
         * boot, plugin's first-process() doing setup work) becomes
         * permanent input-side latency. VST3 hadn't had this cap; that's
         * why AmpCraft VST3 felt noticeably more delayed than AmpCraft VST2
         * even though both run through the same shm ring with the same
         * block size. */
        if (available > (uint64_t)blockFrames * 4) {
            it = ih - (uint64_t)blockFrames * 2;
        }
        for (int i = 0; i < blockFrames; i++) {
            uint64_t idx = (it + i) & (VSTPOC_AUDIO_RING_FRAMES - 1);
            in_l[i] = g_shm->audio_in[idx * VSTPOC_CHANNELS + 0];
            in_r[i] = g_shm->audio_in[idx * VSTPOC_CHANNELS + 1];
        }
        __atomic_store_n(&g_shm->audio_in_tail, it + blockFrames, __ATOMIC_RELEASE);

        DWORD t0 = GetTickCount();
        process_block(processor, in_l, in_r, out_l, out_r, blockFrames);
        DWORD t1 = GetTickCount();
        uint64_t proc_us = (uint64_t)(t1 - t0) * 1000;  /* ms*1000 ≈ us */
        stats_process_us_total += proc_us;
        if (proc_us > stats_process_us_max) stats_process_us_max = proc_us;

        /* Push block to output ring (speaker). Consumer = launcher.
         * Match vst_host.c's behavior: partial-push whatever fits and drop
         * the remainder, rather than dropping the whole block. Coarse
         * whole-block drops add audible glitches; partial pushes only lose
         * the tail samples that wouldn't have made it anyway. */
        uint64_t oh = __atomic_load_n(&g_shm->audio_head, __ATOMIC_RELAXED);
        uint64_t ot = __atomic_load_n(&g_shm->audio_tail, __ATOMIC_ACQUIRE);
        uint64_t space = (uint64_t)VSTPOC_AUDIO_RING_FRAMES - (oh - ot);
        uint64_t push  = space < (uint64_t)blockFrames ? space : (uint64_t)blockFrames;
        for (uint64_t i = 0; i < push; i++) {
            uint64_t idx = (oh + i) & (VSTPOC_AUDIO_RING_FRAMES - 1);
            g_shm->audio[idx * VSTPOC_CHANNELS + 0] = out_l[i];
            g_shm->audio[idx * VSTPOC_CHANNELS + 1] = out_r[i];
        }
        __atomic_store_n(&g_shm->audio_head, oh + push, __ATOMIC_RELEASE);
        if (g_shm) g_shm->guest_frames_produced += push;
        ++stats_blocks;
        if (push < (uint64_t)blockFrames) {
            ++stats_partial_pushes;
            stats_dropped_frames += (uint64_t)blockFrames - push;
        }
        /* If the host isn't consuming, throttle so we don't spin on a full
         * ring (matches vst_host.c line 1009). */
        if (space < (uint64_t)blockFrames) { ++stats_out_full; Sleep(1); }

        DWORD now = GetTickCount();
        if (now - stats_last_log >= 5000) {
            LOG("audio stats: blocks=%llu wait_loops=%llu out_full=%llu "
                "partial=%llu dropped_frames=%llu proc_us avg=%llu max=%llu\n",
                (unsigned long long)stats_blocks,
                (unsigned long long)stats_wait_loops,
                (unsigned long long)stats_out_full,
                (unsigned long long)stats_partial_pushes,
                (unsigned long long)stats_dropped_frames,
                (unsigned long long)(stats_blocks ? stats_process_us_total / stats_blocks : 0),
                (unsigned long long)stats_process_us_max);
            stats_blocks = stats_wait_loops = stats_out_full = 0;
            stats_partial_pushes = stats_dropped_frames = 0;
            stats_process_us_total = 0;
            stats_process_us_max = 0;
            stats_last_log = now;
        }
    }

    LOG("stop_flag set; shutting down\n");

    /* Teardown — strict reverse of init order to satisfy VST3 lifecycle. */
    if (editorThread) {
        WaitForSingleObject(editorThread, 5000);
        CloseHandle(editorThread);
    }
    if (view) view->release();
    processor->setProcessing(false);
    component->setActive(false);
    if (editController) {
        editController->terminate();
        editController->release();
    }
    component->terminate();
    component->release();
    /* module is shared_ptr — destructor unloads the DLL */

    LOG("shutdown clean\n");
    return 0;
}
