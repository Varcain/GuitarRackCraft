#include "WineHostProcess.h"
#include "../util/log.h"

#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// vstpoc: the WM-storm throttle knobs (WINE_VSTPOC_COALESCE_POSTS /
// USER_STORM_BREAK / POST_GAP_MS) are LOAD-BEARING for JUCE plugin editors
// (TH-U/X50 WM_USER+123 storms) but a ~100x latency tax for Chromium/Electron
// managers — their message pump self-wakes by re-posting the same WM, which
// our coalescer mistakes for a storm (see feedback_wm_throttle_chromium_tax).
// The knobs are read ONCE by the wineserver, which is SHARED per prefix, so
// the choice can't be per-flow — only per-prefix-KIND. A v-prefix hosts a
// standalone plugin (throttles ON); an e-/installer-prefix hosts an Electron
// manager (+ any plugins installed into that environment) and runs throttles
// OFF. Residual: a storm-prone JUCE plugin living in an environment runs
// unthrottled — accepted (VSTPOC_LOAD_ON_EDITOR_THREAD covers the known cases).
bool vstpocIsPluginPrefix(const std::string& winePrefix) {
    size_t slash = winePrefix.find_last_of('/');
    const std::string base =
        (slash == std::string::npos) ? winePrefix : winePrefix.substr(slash + 1);
    return base.rfind("wineprefix_v", 0) == 0;
}

// PerformanceHint API (libandroid.so, API 33+). Resolved at runtime via
// dlsym since minSdk=27 — we can't link against the symbols directly.
// All three are no-ops on older Android (the create call returns null).
using APerformanceHintManagerHandle = void;
using APerformanceHintSessionHandle = void;
using GetManagerFn   = APerformanceHintManagerHandle* (*)(void);
using CreateSessionFn = APerformanceHintSessionHandle* (*)(
    APerformanceHintManagerHandle*, const int32_t*, size_t, int64_t);
using CloseSessionFn = void (*)(APerformanceHintSessionHandle*);
struct PerfHintApi {
    GetManagerFn    getManager   = nullptr;
    CreateSessionFn createSession = nullptr;
    CloseSessionFn  closeSession  = nullptr;
    bool resolved = false;
    bool available() const { return getManager && createSession; }
};
const PerfHintApi& perf_hint_api() {
    static PerfHintApi api = [] {
        PerfHintApi a;
        // libandroid is already linked; RTLD_DEFAULT searches the loaded
        // dependency graph and finds it without a second dlopen.
        a.getManager = reinterpret_cast<GetManagerFn>(
            ::dlsym(RTLD_DEFAULT, "APerformanceHint_getManager"));
        a.createSession = reinterpret_cast<CreateSessionFn>(
            ::dlsym(RTLD_DEFAULT, "APerformanceHint_createSession"));
        a.closeSession = reinterpret_cast<CloseSessionFn>(
            ::dlsym(RTLD_DEFAULT, "APerformanceHint_closeSession"));
        a.resolved = true;
        return a;
    }();
    return api;
}

// Compute the "big-core" cpu_set on this heterogeneous CPU. Heuristic:
// any core whose cpuinfo_max_freq is within 10% of the highest observed.
// On a typical Snapdragon 8 Gen 2 this yields the X3+A715 cluster
// (cores 5-7) and excludes the A510 efficiency cores. Falls back to
// "all cores" if /sys is unreadable. Cached after first call.
static const cpu_set_t& big_core_set() {
    static cpu_set_t cached;
    static bool computed = false;
    if (computed) return cached;
    CPU_ZERO(&cached);
    const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
    long max_freqs[64] = {0};
    long highest = 0;
    const int probe = (n > 0 && n < 64) ? (int)n : 64;
    for (int i = 0; i < probe; ++i) {
        char path[160];
        std::snprintf(path, sizeof(path),
            "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        FILE* f = std::fopen(path, "r");
        if (f) {
            long v = 0;
            if (std::fscanf(f, "%ld", &v) == 1 && v > 0) {
                max_freqs[i] = v;
                if (v > highest) highest = v;
            }
            std::fclose(f);
        }
    }
    if (highest <= 0) {
        for (int i = 0; i < probe; ++i) CPU_SET(i, &cached);
    } else {
        const long threshold = (highest * 9) / 10;
        for (int i = 0; i < probe; ++i)
            if (max_freqs[i] >= threshold) CPU_SET(i, &cached);
    }
    computed = true;
    return cached;
}

// Pin the calling thread to the big cores. Returns the bitmask we set,
// for logging (0 on failure).
unsigned long set_big_core_affinity() {
    const cpu_set_t& set = big_core_set();
    if (::sched_setaffinity(0, sizeof(set), &set) != 0) return 0;
    unsigned long mask = 0;
    for (int i = 0; i < 64; ++i) if (CPU_ISSET(i, &set)) mask |= (1UL << i);
    return mask;
}

static char asciiLower(char c) {
    return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}

static bool containsAsciiCaseInsensitive(const std::string& haystack, const char* needle) {
    const size_t needleLen = needle ? std::strlen(needle) : 0;
    if (!needleLen) return true;
    if (needleLen > haystack.size()) return false;
    for (size_t i = 0; i <= haystack.size() - needleLen; ++i) {
        size_t j = 0;
        for (; j < needleLen; ++j) {
            if (asciiLower(haystack[i + j]) != asciiLower(needle[j])) break;
        }
        if (j == needleLen) return true;
    }
    return false;
}

static bool vstpocNeedsScaledGuestStacks(const WineHostProcess::Config& cfg) {
    for (const auto& path : cfg.pluginPaths) {
        if (containsAsciiCaseInsensitive(path, "x50ii") ||
            containsAsciiCaseInsensitive(path, "tse x50") ||
            containsAsciiCaseInsensitive(path, "tse audio/x50")) {
            return true;
        }
    }
    return false;
}

static void vstpocApplyPluginEnvDefaults(const WineHostProcess::Config& cfg) {
    if (vstpocNeedsScaledGuestStacks(cfg)) {
        const char* existing = ::getenv("VSTPOC_STACK_PCT");
        if (!existing || !*existing) {
            /* X50II's JUCE callback thread overflows under FEX at PC
             * 0x7fffb99874 before later async UI updates can run. Patch 0035
             * scales the resolved guest stack reserve; 400 = 4x. */
            ::setenv("VSTPOC_STACK_PCT", "400", 1);
        }
    }
}

// vstpoc 2026-05-24 (Fix A.2): re-pin a SPECIFIC tid to the big-core set.
// Used by the watchdog every few seconds to defeat Android cgroup
// demotion of background-spawned threads. Per-tid mask is silently
// ignored if the tid no longer exists (process race ok).
static void pin_tid_to_big_cores(int tid) {
    const cpu_set_t& set = big_core_set();
    ::sched_setaffinity(tid, sizeof(set), &set);
}

// vstpoc 2026-05-24 (Fix C): dump kernel-side state for a stuck thread.
// Read /proc/<pid>/task/<tid>/wchan (current kernel wait function name),
// /proc/<pid>/task/<tid>/syscall (current syscall + first 6 args), and
// /proc/<pid>/task/<tid>/comm (thread name). Cheap, signal-safe, gives
// most of the diagnostic value of a userspace backtrace without any
// signal-handler risk. Lets us tell at a glance whether a "stuck" thread
// is on a futex (JUCE lock), epoll (X11 server I/O), io_getevents (perf
// hint), nanosleep (FEX backoff), or other — each implies a different
// root cause.
static void dump_stuck_thread_kernel_state(int pid, int tid) {
    auto read_first_line = [](const char* path, char* out, size_t n) -> bool {
        int fd = ::open(path, O_RDONLY);
        if (fd < 0) { out[0] = 0; return false; }
        ssize_t r = ::read(fd, out, n - 1);
        ::close(fd);
        if (r <= 0) { out[0] = 0; return false; }
        out[r] = 0;
        for (ssize_t i = 0; i < r; ++i) if (out[i] == '\n') { out[i] = 0; break; }
        return true;
    };
    char path[160], comm[64] = "?", wchan[128] = "?", syscall[256] = "?";
    std::snprintf(path, sizeof(path), "/proc/%d/task/%d/comm", pid, tid);
    read_first_line(path, comm, sizeof(comm));
    std::snprintf(path, sizeof(path), "/proc/%d/task/%d/wchan", pid, tid);
    read_first_line(path, wchan, sizeof(wchan));
    std::snprintf(path, sizeof(path), "/proc/%d/task/%d/syscall", pid, tid);
    read_first_line(path, syscall, sizeof(syscall));
    LOGE("vstpoc watchdog:   tid=%d comm='%s' wchan='%s' syscall='%s'",
         tid, comm, wchan, syscall);
}

}  // namespace

/* vstpoc 2026-05-25: cross-module signal for "ESC-on-touch-while-hung" UX
 * hack. The watchdog samples vst_host's total process CPU once per second
 * (sum of utime+stime delta across all tasks). When the process burns more
 * than ~90% of one core for >=3 consecutive seconds, the host is in a
 * pathological MessageManager/AsyncUpdater storm (typically TH-U popup
 * with thousands of preset items). X11NativeDisplay::injectTouch reads
 * this counter on ACTION_UP and, if > 0, synthesizes a VK_ESCAPE
 * keydown/keyup after the click so any modal popup gets a dismiss chance.
 *
 * Single global rather than per-process — at most one host runs at a time
 * in practice; if that changes, promote to a map keyed by pid. */
static std::atomic<int> g_vstpoc_hot_seconds{0};
extern "C" int vstpoc_get_hot_seconds() {
    return g_vstpoc_hot_seconds.load(std::memory_order_acquire);
}

/* vstpoc experiment hook (2026-06-02): file-driven env overrides so we can tune
 * wine/FEX env vars WITHOUT a rebuild. Drop KEY=VAL lines in <cache>/wine_env.txt
 * and relaunch the plugin. Used e.g. for VSTPOC_STACK_PCT (patch 0035 thread
 * stack scaling — BIAS FX 2's tid 00a0 FEX overflow). '#' lines skipped; absent
 * file = no-op. Applied LAST in each env block so it can override defaults. */
static void vstpocApplyEnvFile(const std::string& cacheDir) {
    std::ifstream f(cacheDir + "/wine_env.txt");
    std::string line;
    while (std::getline(f, line)) {
        size_t b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos || line[b] == '#') continue;
        size_t eq = line.find('=', b);
        if (eq == std::string::npos) continue;
        std::string k = line.substr(b, eq - b);
        std::string v = line.substr(eq + 1);
        size_t e = v.find_last_not_of(" \t\r\n");
        v.resize(e == std::string::npos ? 0 : e + 1);
        if (!k.empty()) ::setenv(k.c_str(), v.c_str(), 1);
    }
}

/* vstpoc: write the FEX Config.json = base cache entries + any KEY=VAL lines from
 * <appCache>/fex_hacks.txt as extra Config entries (e.g. "Multiblock=0",
 * "X87ReducedPrecision=0", "TSOEnabled=1", "O0=1"). Lets FEX Hacks/CPU knobs be
 * tried WITHOUT a rebuild while chasing the BIAS FX 2 gfx::SystemFonts re-entrant
 * recursion under FEX. fexRoot = "<appCache>/fex_aot"; '#' lines skipped. */
static void vstpocWriteFexConfig(const std::string& configFile, const std::string& fexRoot) {
    std::string appCache = fexRoot;
    auto pos = appCache.rfind("/fex_aot");
    if (pos != std::string::npos) appCache.resize(pos);
    std::string body =
        "    \"EnableCodeCachingWIP\": \"1\",\n"
        "    \"EnableLazyCodeCachingWIP\": \"1\",\n"
        "    \"EnableCodeCacheValidation\": \"1\"";
    std::ifstream hf(appCache + "/fex_hacks.txt");
    std::string line;
    while (std::getline(hf, line)) {
        size_t b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos || line[b] == '#') continue;
        size_t eq = line.find('=', b);
        if (eq == std::string::npos) continue;
        std::string k = line.substr(b, eq - b);
        std::string v = line.substr(eq + 1);
        size_t ke = k.find_last_not_of(" \t");
        k.resize(ke == std::string::npos ? 0 : ke + 1);
        size_t e = v.find_last_not_of(" \t\r\n");
        v.resize(e == std::string::npos ? 0 : e + 1);
        if (!k.empty()) body += ",\n    \"" + k + "\": \"" + v + "\"";
    }
    FILE* f = std::fopen(configFile.c_str(), "w");
    if (f) {
        std::fprintf(f, "{\n  \"Config\": {\n%s\n  }\n}\n", body.c_str());
        std::fclose(f);
    }
}

/* Set wine env vars in the current process. Used by both the vst_host
 * fork child (start()) and the wineboot fork child (bootServicesIfNeeded()).
 * Safe to call only after fork — mutates ::setenv. */
/* vstpoc Mesa-Zink (desktop GL over Vulkan/Turnip for GL plugin editors like
 * AmpliTube). Enables the desktop-GL path GLOBALLY via VSTPOC_EGL_LIBRARY (see
 * below) — every GL editor now goes through zink->Turnip instead of system
 * Adreno GLES. Adds the mesa + turnip dirs to LD_LIBRARY_PATH so mesa's deps
 * resolve (mesa-only libs incl. the stub libLLVM live in mesaDir; shared
 * libdrm/libxcb/libz in turnipDir; the app-built X11 libs in nativeLibDir win,
 * but they're the same Termux build). zink uses the system Vulkan loader by
 * default. */
static void vstpocSetMesaZinkEnv(const std::string& wineRoot, const std::string& nativeLibDir) {
    const std::string mesaDir   = wineRoot + "/mesa";
    const std::string turnipDir = wineRoot + "/turnip";
    const std::string ldpath = nativeLibDir + ":" + mesaDir + ":" + turnipDir;
    ::setenv("LD_LIBRARY_PATH", ldpath.c_str(), 1);
    /* Our Mesa is a STANDALONE (non-glvnd) build — win32u dlopens its libEGL.so
     * directly, so no glvnd __EGL_VENDOR_LIBRARY_FILENAMES is needed. */
    ::setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1);
    ::setenv("GALLIUM_DRIVER", "zink", 1);
    ::setenv("LIBGL_DRIVERS_PATH", (mesaDir + "/dri").c_str(), 1);
    /* Desktop-GL switch — now ENABLED GLOBALLY (was opt-in via wine_env.txt).
     * VSTPOC_EGL_LIBRARY makes win32u's egl_init dlopen our standalone Mesa
     * libEGL → desktop GL 4.6 via zink->Turnip for every GL plugin editor
     * (needed by JUCE/desktop-GLSL editors like AmpliTube). The two tuning
     * vars are required by the headless zink path and are inert unless our
     * libEGL is actually loaded (system Adreno EGL / DXVK ignore them):
     *   LIBGL_ALWAYS_SOFTWARE=1 forces Mesa's surfaceless software loader (the
     *     only route to zink with no DRM render-node on untrusted_app);
     *   VSTPOC_ZINK_FORCE_HW=1 stops that flag from making zink demand a CPU
     *     (lavapipe) device — keep the real HW (Turnip) pdev.
     * All three are setenv-overwrite so cache/wine_env.txt (applied later) can
     * still A/B them off per plugin during regression testing. */
    ::setenv("VSTPOC_EGL_LIBRARY", (mesaDir + "/libEGL_vstpoc.so").c_str(), 1);
    ::setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
    ::setenv("VSTPOC_ZINK_FORCE_HW", "1", 1);
}

void WineHostProcess::setupWineEnvChild(const Config& cfg) {
    /* See start() for the rationale behind each var; this is a verbatim
     * extraction so the two children share identical env. */
    ::setenv("LD_LIBRARY_PATH", cfg.nativeLibDir.c_str(), 1);
    ::setenv("WINEPREFIX",   cfg.winePrefix.c_str(),         1);
    ::setenv("WINELOADER",   cfg.wineBinary.c_str(),         1);
    ::setenv("WINESERVER",   cfg.wineserverBinary.c_str(),   1);
    ::setenv("WINEDLLPATH",  cfg.wineDllPath.c_str(),        1);
    {
        std::string wineRoot = cfg.wineBinary;
        auto pos = wineRoot.find_last_of('/');
        if (pos != std::string::npos) wineRoot.resize(pos);
        pos = wineRoot.find_last_of('/');
        if (pos != std::string::npos) wineRoot.resize(pos);
        const std::string ntdll = wineRoot + "/lib/wine/aarch64-unix/ntdll.so";
        ::setenv("WINELOADER_NTDLL", ntdll.c_str(), 1);
        const std::string dataDir = wineRoot + "/share/wine";
        ::setenv("WINEDATADIR", dataDir.c_str(), 1);
        const std::string binDir = wineRoot + "/bin";
        ::setenv("WINEBINDIR", binDir.c_str(), 1);
        const std::string dllDir = wineRoot + "/lib/wine";
        ::setenv("WINEDLLDIR", dllDir.c_str(), 1);
        /* vstpoc: Turnip (Mesa Adreno Vulkan, correct VK_EXT_robustness2
         * nullDescriptor) loaded as an Android-HAL driver via libadrenotools,
         * exactly like Winlator. win32u patch 0030 calls
         * adrenotools_open_libvulkan() which opens the Android libvulkan with
         * our custom HAL driver hooked into a linker namespace that satisfies
         * the driver's namespace-restricted system deps (libhardware/libcutils/
         * libnativewindow). HOOKDIR MUST equal nativeLibraryDir (it holds the
         * built hook .so shipped as jniLibs), DRIVERDIR is the extracted driver
         * dir (internal storage), DRIVERNAME is the HAL driver soname.
         * IMPORTANT: do NOT set VSTPOC_VULKAN_LOADER — patch 0024 keeps the
         * android_surface bridge active when it's unset, which is exactly what
         * the Android libvulkan needs (it has android_surface, not xlib). */
        /* Trailing slash is REQUIRED: adrenotools_open_libvulkan builds the
         * driver path as (customDriverDir + customDriverName) with no separator,
         * so without it the path becomes ".../turnipvulkan.ad07xx.so" and the
         * dlopen fails → silent fallback to system (Qualcomm) Vulkan, whose
         * broken VK_EXT_robustness2 nullDescriptor makes DXVK/D3D11 plugins
         * (AmpliTube) render black. */
        const std::string turnipDir = wineRoot + "/turnip/";
        ::setenv("VSTPOC_ADRENOTOOLS_HOOKDIR",   cfg.nativeLibDir.c_str(), 1);
        ::setenv("VSTPOC_ADRENOTOOLS_DRIVERDIR", turnipDir.c_str(),        1);
        ::setenv("VSTPOC_ADRENOTOOLS_DRIVERNAME", "vulkan.ad07xx.so",      1);
        /* Mesa/Turnip logs to logcat by default (invisible from the forked wine
         * process); redirect to a readable file + trace device init. */
        ::setenv("MESA_LOG_FILE", (cfg.cacheDir + "/turnip.log").c_str(), 1);
        ::setenv("TU_DEBUG", "startup", 1);
        /* Turnip shader cache → our writable cacheDir (its default path is
         * unwritable from here → recompiles every run otherwise). */
        const std::string mesaCache = cfg.cacheDir + "/mesa_shader_cache";
        ::mkdir(mesaCache.c_str(), 0700);
        ::setenv("MESA_SHADER_CACHE_DIR", mesaCache.c_str(), 1);
        ::setenv("XDG_CACHE_HOME", cfg.cacheDir.c_str(), 1);
        ::setenv("HOME", cfg.cacheDir.c_str(), 1);
        vstpocSetMesaZinkEnv(wineRoot, cfg.nativeLibDir);
    }
    ::setenv("HODLL64",      "libarm64ecfex.dll",             1);
    ::setenv("HODLL",        "libwow64fex.dll",               1);

    /* FEX-Emu code caching: redirect the cache/data/config dirs into our
     * app cacheDir (writable) and write a Config.json that enables the
     * WIP code-cache feature. Without these:
     *   - FEX picks up GetCacheDirectory() = LOCALAPPDATA/.../fex-emu/
     *     or falls through to "." (relative to wine's CWD) → cache
     *     ends up at unwritable paths, FEX silently skips persistence
     *     and pays the JIT-compile cost on every run.
     *   - FEX's non-path options (EnableCodeCachingWIP, SMCChecks,
     *     TSO flags) aren't readable from env vars — they live in
     *     Config.json under <config_dir>/Config.json.
     *
     * On this version of FEX the relevant config knob is the
     * EnableCodeCachingWIP (WIP = work-in-progress). The old
     * AOTIRCapture/Save/Load options have been replaced.
     *
     * Path layout: cacheDir/fex_aot/{config,data,cache}/. */
    {
        const std::string fexRoot = cfg.cacheDir + "/fex_aot";
        const std::string configDir = fexRoot + "/config/";
        const std::string dataDir   = fexRoot + "/data/";
        const std::string cacheDir  = fexRoot + "/cache/";
        ::mkdir(fexRoot.c_str(),   0700);
        ::mkdir(configDir.c_str(), 0700);
        ::mkdir(dataDir.c_str(),   0700);
        ::mkdir(cacheDir.c_str(),  0700);

        /* Write Config.json on first launch (idempotent — same content
         * each time). FEX reads it from <config_dir>/Config.json. */
        const std::string configFile = configDir + "Config.json";
        vstpocWriteFexConfig(configFile, fexRoot);
        ::setenv("FEX_APP_CONFIG_LOCATION", configDir.c_str(), 1);
        ::setenv("FEX_APP_DATA_LOCATION",   dataDir.c_str(),   1);
        ::setenv("FEX_APP_CACHE_LOCATION",  cacheDir.c_str(),  1);
    }
    /* FEX_SMCCHECKS: SMC (self-modifying code) detection mode. "full"
     * revalidates every JIT block before execution — ~3-5× perf cost
     * (memory: feedback_fex_smcchecks_full_perf). "mtrack" is the FEX
     * default (memory-tracking-based invalidation); much faster, correct
     * for code that doesn't legitimately rewrite itself mid-stream.
     *
     * Was set to "full" 2026-05-23 as a stale-cache experiment for TH-U
     * drag-drop NULL deref. That hypothesis didn't pan out (the crash
     * was a pre-existing TH-U bug, not FEX cache staleness) — so the
     * 3-5× tax was being paid for nothing. Reverted 2026-05-27 after
     * audio-breakup diagnosis with Helix Native showed the JIT cost was
     * eating real-time deadlines even at moderate CPU. */
    ::setenv("FEX_SMCCHECKS", "mtrack", 1);
    /* VST2 default (2026-06-10): load each plugin DLL + VSTPluginMain on its
     * editor thread so JUCE's MessageManager, window ownership and message
     * pump share ONE thread. With main-thread loading, every JUCE editor
     * interaction was a cross-thread send and X50II's menu wedged the host
     * in a self-feeding WM_USER storm (UI frozen, audio xruns). Single-thread
     * JUCE fixed menus end-to-end; X50II + LeCto verified on device.
     * vst3_host ignores this env. Override via wine_env.txt (=0) to A/B.
     * See vst_host.c per_plugin_editor_thread + memory
     * feedback_x50_stomp_popup_grab_dismiss. */
    ::setenv("VSTPOC_LOAD_ON_EDITOR_THREAD", "1", 1);
    /* Plugin-specific env defaults must be set before wine_env.txt so local
     * testing can still override or disable them without a rebuild. */
    vstpocApplyPluginEnvDefaults(cfg);
    /* vstpoc: file-driven env overrides (VSTPOC_STACK_PCT etc.) — see start()'s
     * inline copy. Both blocks per feedback_winehostprocess_dup_env. */
    vstpocApplyEnvFile(cfg.cacheDir);
    /* FEX TSO config: leave at defaults.
     *   FEX_HALFBARRIERTSOENABLED=1 (default) — half-barrier optimisation
     *     on; faster than the strict-barrier path.
     *   FEX_VECTORTSOENABLED=0 (default) — vector-load/store TSO off;
     *     faster, fine for plugins that don't rely on cross-thread SIMD
     *     memory ordering.
     *
     * Both were forced to slow modes on 2026-05-23 as a memory-ordering
     * hypothesis for TH-U's drag-drop NULL deref — gdb later confirmed
     * (feedback_thu_deep_deadlock) the bug was a JUCE-internal deadlock,
     * not a FEX TSO race. The slow settings were paying a real-time
     * deadline tax for nothing. Reverted 2026-05-27 after Helix Native
     * showed audio stuttering under the combined FEX overhead. */

    /* vstpoc: the wineserver cycle-detector band-aid is NOT used (its 11.9
     * port destabilized plugin loading). The robust fix is patch 021's
     * approach — don't send WM_SETFOCUS for child popups in
     * process_mouse_message — pursued in win32u instead. This env is inert
     * (no detector code present) but kept for clarity. */
    ::setenv("WINE_VSTPOC_NO_CYCLE_DETECT", "1", 1);
    /* vstpoc patch 028 (ported to 11.9): coalesce exact duplicate WM_USER+N
     * posts in the wineserver. TH-U's editor-create and X50's popup dismiss
     * can both be buried by a JUCE/FEX WM_USER+123 callback flood. Cap pending
     * posts per exact (hwnd,msg,wparam,lparam) at this threshold; distinct
     * callback pointers remain queued. Read once by wineserver at start —
     * wineserver MUST be killed for a change to take effect. */
    /* vstpoc patch 028 + storm breaker (0046) + post-gap (030): gate on prefix
     * KIND — plugin (v) prefixes keep them ON (JUCE storms); manager/Electron
     * (e/installer) prefixes turn them OFF (the Chromium ~100x msg-pump tax).
     * See vstpocIsPluginPrefix + feedback_wm_throttle_chromium_tax. Read once
     * by the shared-per-prefix wineserver — kill wineserver to apply. */
    const bool vstpocPluginPfx = vstpocIsPluginPrefix(cfg.winePrefix);
    ::setenv("WINE_VSTPOC_COALESCE_POSTS",   vstpocPluginPfx ? "1"   : "0", 1);
    ::setenv("WINE_VSTPOC_USER_STORM_BREAK", vstpocPluginPfx ? "500" : "0", 1);
    ::setenv("WINE_VSTPOC_POST_GAP_MS",      vstpocPluginPfx ? "50"  : "0", 1);
    /* vstpoc: registry open-handle cache (ntdll patch 0053). Heavy IK plugins
     * (AmpliTube 5) re-open the same key chain (HKLM\SOFTWARE\IK Multimedia\
     * AmpliTube 5) and re-query their license serial ~7000x/sec as a runtime
     * heartbeat; on Windows that is in-process + free, but under wine every
     * NtOpenKey/NtClose is a wineserver IPC round-trip, FEX-amplified -> a
     * constant UI-thread storm (~14000 write-syscalls/sec, idle). The cache
     * keeps recently-opened key handles alive across the app's close/reopen and
     * returns them without a server call (NtClose on a cached key handle is a
     * no-op); it eliminated ~93% of the registry server calls on AmpliTube
     * (~14000 -> ~1500/sec). Scoped to non-plugin (e/installer) prefixes where
     * the IK managers + their plugins live; the delicate v-prefix JUCE plugins
     * are left untouched (cache unvalidated there) and can opt in via
     * wine_env.txt (VSTPOC_REGCACHE=1). Read per process by ntdll; no wineserver
     * restart needed. See feedback_amplitube_registry_heartbeat. */
    ::setenv("VSTPOC_REGCACHE", vstpocPluginPfx ? "0" : "1", 1);
    /* vstpoc: GPU present (winex11 patch 0048). Ship the editor's rendered
     * AHardwareBuffer GPU->GPU to the X server over the AHB side-channel +
     * present fence, composited zero-copy, instead of CPU readback + full-frame
     * XPutImage over the socket. Removed ~31% of the CPU during BIAS knob-drag
     * (the VU meters stay fluid; does NOT change the knob fps, which is gated
     * upstream by the emulated DXVK/Turnip/libcef render pipeline). Only
     * Vulkan/DXVK editors use this path (GL editors use win32u readback, GDI use
     * XPutImage — both unaffected); graceful per-frame fallback to readback if
     * the channel/buffer fails. Scoped to non-plugin (e/installer) prefixes
     * where BIAS + the managers live (BIAS verified); v-prefix Vulkan editors
     * (TONEX/TH-U) stay on readback until render-verified. Opt out via
     * wine_env.txt (VSTPOC_AHB_PRESENT=0). See feedback_bias_knob_drag_gpu_latency. */
    ::setenv("VSTPOC_AHB_PRESENT", vstpocPluginPfx ? "0" : "1", 1);
    /* TH-U editor deadlock fix: drop win_data_mutex across the cross-thread send
     * in winex11 WM_STATE/_XEMBED PropertyNotify handlers. Default off in wine;
     * we enable it here. Benign for plugins that don't hit the deadlock. */
    ::setenv("WINE_VSTPOC_WM_STATE_UNLOCK", "1", 1);
    /* DXVK log level — "info" gives adapter/feature-level/init diagnostics
     * without the per-shader signature DUMP that "debug" emits. On AmpliTube
     * (hundreds of shaders) the debug dump floods the log to 70k+ lines AND
     * the dump path itself is a crash suspect on Turnip (less-tested code that
     * walks shader metadata). Bump back to "debug" only when chasing a D3D11
     * device-creation failure, not for shader-compile issues. */
    ::setenv("DXVK_LOG_LEVEL", "info", 1);
    /* DXVK Graphics Pipeline Libraries OFF. Turnip advertises
     * VK_EXT_graphics_pipeline_library (Qualcomm's driver does NOT), so on
     * Turnip DXVK takes the GPL shader-compile path that never ran on the old
     * Qualcomm path. That path faults under FEX — a compiler thread crashes
     * during shader compile and wine can't dispatch it ("NtRaiseException:
     * Exception frame is not in stack limits"), which regressed TONEX AFTER a
     * clean Turnip D3D11 device + setProcessing. Forcing the monolithic
     * pipeline path (the one TONEX already worked with on Qualcomm) sidesteps
     * it. DXVK_CONFIG = inline "key = value", ';'-separated (config.cpp:1497). */
    ::setenv("DXVK_CONFIG", "dxvk.enableGraphicsPipelineLibrary = False", 1);
    {
        char displayBuf[64];
        std::snprintf(displayBuf, sizeof(displayBuf),
                      "127.0.0.1:%d", cfg.displayNumber);
        ::setenv("DISPLAY", displayBuf, 1);
    }

    // Set Windows-style PATH/COMSPEC in the Linux env. Wine inherits the
    // Linux process env when constructing the Windows process environment
    // for the first wine subprocess; child wine processes spawned via
    // wineserver inherit from the parent. Without this, Electron apps that
    // call Node's spawnSync (IK Multimedia Product Manager, Native Access)
    // fail at startup with "spawnSync cmd.exe ENOENT" because libuv walks
    // PATH to find cmd.exe and the Linux PATH (/system/bin:/system/xbin:…)
    // has nothing Windows-style. Linux PATH isn't needed once wine has
    // exec'd — wineserver/wine use absolute paths via WINELOADER /
    // WINESERVER / WINEDLLPATH for everything else.
    ::setenv("PATH", "C:\\windows\\system32;C:\\windows;C:\\windows\\System32\\Wbem", 1);
    ::setenv("COMSPEC", "C:\\windows\\system32\\cmd.exe", 1);
    ::setenv("PATHEXT", ".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC", 1);
}

/* Run `wine wineboot.exe -i` once per WINEPREFIX to populate default
 * service registry entries and start services.exe (which then auto-starts
 * RpcSs, etc.). Plugins like X50II call CoMarshalInterface during
 * activation, which routes through RpcSs; without this bootstrap, wine's
 * combase logs `err:ole:start_rpcss Failed to open RpcSs service`,
 * dispatches RPC_S_SERVER_UNAVAILABLE, and the plugin silently bails
 * on the Register click. Sentinel file marks the prefix as booted so we
 * only pay the ~5s wineboot cost on the first plugin load per prefix.
 *
 * Returns true if the prefix is (or now is) booted; false on hard failure.
 * False is non-fatal — plugin launch proceeds and may still work for
 * plugins that don't use COM out-of-process. */
bool WineHostProcess::bootServicesIfNeeded() {
    const std::string sentinel = cfg_.winePrefix + "/.vstpoc_services_booted_v1";
    if (::access(sentinel.c_str(), F_OK) == 0) {
        return true;  /* fast path on second+ plugin launch */
    }
    LOGI("WineHostProcess: bootstrapping wine services in %s", cfg_.winePrefix.c_str());

    pid_t p = ::fork();
    if (p < 0) {
        LOGE("WineHostProcess: bootstrap fork failed: %s", std::strerror(errno));
        return false;
    }
    if (p == 0) {
        /* child: stdout/stderr to a log so we can diagnose wineboot failures. */
        const std::string bootLog = cfg_.cacheDir + "/wineboot.log";
        int lfd = ::open(bootLog.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (lfd >= 0) { ::dup2(lfd, 1); ::dup2(lfd, 2); if (lfd > 2) ::close(lfd); }
        setupWineEnvChild(cfg_);
        /* Minimal WINEDEBUG — wineboot is normally chatty under +all. */
        ::setenv("WINEDEBUG", "-all,err+all", 1);

        std::string wineBoot = cfg_.winePrefix +
            "/drive_c/windows/system32/wineboot.exe";
        std::vector<std::string> argv_owned = {
            cfg_.wineBinary, wineBoot, "-i"
        };
        std::vector<char*> argv;
        for (auto& s : argv_owned) argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);
        ::execv(cfg_.wineBinary.c_str(), argv.data());
        ::_exit(127);
    }

    /* parent: poll for child exit with a 60s timeout. */
    int status = 0;
    int ret = -1;
    for (int i = 0; i < 600; ++i) {  /* 600 * 100ms = 60s */
        ret = ::waitpid(p, &status, WNOHANG);
        if (ret == p) break;
        if (ret < 0) {
            LOGE("WineHostProcess: bootstrap waitpid failed: %s", std::strerror(errno));
            return false;
        }
        ::usleep(100 * 1000);
    }
    if (ret != p) {
        LOGE("WineHostProcess: bootstrap timed out after 60s, killing pid=%d", p);
        ::kill(p, SIGTERM);
        ::usleep(200 * 1000);
        ::kill(p, SIGKILL);
        ::waitpid(p, &status, 0);
        return false;
    }
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    LOGI("WineHostProcess: wineboot exited code=%d (status=0x%x)", code, status);
    if (code != 0) {
        LOGE("WineHostProcess: wineboot failed; will retry next plugin launch");
        return false;
    }

    /* vstpoc 2026-06-15: wineboot just wrote PROCESSOR_ARCHITECTURE to
     * HKLM\System\...\Session Manager\Environment from the UN-spoofed
     * SystemCpuInformation = "ARM64" on our ARM64EC build (wine patch 0013
     * only spoofs AMD64 for is_wow64() 32-bit callers — native wineboot is
     * excluded by design so 64-bit JUCE plugins still read the real ARM64 from
     * the SystemCpuInformation API for their render-backend choice). PACE's iLok
     * "License Support" PreInstallDriverCheck reads this registry value and
     * aborts ("Installs on Windows 7 require SHA256 support…") on anything but
     * AMD64 — proven by a desktop-wine differential (AMD64=pass, forced ARM64=
     * abort at the same custom action). Overwrite it to AMD64 here via the LIVE
     * wineserver wineboot just started (an offline system.reg edit would lose to
     * the in-memory copy). Only this Environment value changes; the
     * SystemCpuInformation API is untouched, so plugin rendering is unaffected.
     * See memory feedback_ilok_sha256_processor_arch. */
    {
        pid_t gp = ::fork();
        if (gp == 0) {
            int nfd = ::open("/dev/null", O_WRONLY);
            if (nfd >= 0) { ::dup2(nfd, 1); ::dup2(nfd, 2); if (nfd > 2) ::close(nfd); }
            setupWineEnvChild(cfg_);
            ::setenv("WINEDEBUG", "-all", 1);
            std::string regExe = cfg_.winePrefix + "/drive_c/windows/system32/reg.exe";
            std::vector<std::string> a = {
                cfg_.wineBinary, regExe, "add",
                "HKLM\\System\\CurrentControlSet\\Control\\Session Manager\\Environment",
                "/v", "PROCESSOR_ARCHITECTURE", "/t", "REG_SZ", "/d", "AMD64", "/f"
            };
            std::vector<char*> argv;
            for (auto& s : a) argv.push_back(const_cast<char*>(s.c_str()));
            argv.push_back(nullptr);
            ::execv(cfg_.wineBinary.c_str(), argv.data());
            ::_exit(127);
        }
        if (gp > 0) {
            int st = 0, r = -1;
            for (int i = 0; i < 150; ++i) {  /* up to 15s */
                r = ::waitpid(gp, &st, WNOHANG);
                if (r == gp) break;
                ::usleep(100 * 1000);
            }
            if (r != gp) { ::kill(gp, SIGKILL); ::waitpid(gp, &st, 0); }
            LOGI("WineHostProcess: set Session Manager PROCESSOR_ARCHITECTURE=AMD64 "
                 "(wineboot had written the host arch)");
        } else {
            LOGE("WineHostProcess: PROCESSOR_ARCHITECTURE override fork failed: %s",
                 std::strerror(errno));
        }
    }

    /* vstpoc 2026-05-29: explicitly launch rpcss.exe in THIS prefix's
     * wineserver session. wine registers RpcSs as demand-start, but combase's
     * on-demand start fails in our Bionic/FEX env — plugins that do
     * out-of-process COM (TH-U: editor + audio activation) then hit
     * RPC_S_SERVER_UNAVAILABLE because \\.\pipe\lrpc\irpcss never gets served.
     * Starting rpcss.exe directly here (same prefix => same wineserver =>
     * same pipe namespace as the plugin host launched right after) makes the
     * endpoint mapper available. Detached daemon; we wait ~1.5s for it to
     * create the pipe before returning so the host finds it. */
    {
        pid_t rp = ::fork();
        if (rp == 0) {
            ::setsid();  /* detach so it survives as a daemon */
            const std::string rpcLog = cfg_.cacheDir + "/rpcss.log";
            int lfd = ::open(rpcLog.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (lfd >= 0) { ::dup2(lfd, 1); ::dup2(lfd, 2); if (lfd > 2) ::close(lfd); }
            setupWineEnvChild(cfg_);
            ::setenv("WINEDEBUG", "-all,err+all", 1);
            std::string rpcss = cfg_.winePrefix +
                "/drive_c/windows/system32/rpcss.exe";
            std::vector<std::string> a = { cfg_.wineBinary, rpcss };
            std::vector<char*> argv;
            for (auto& s : a) argv.push_back(const_cast<char*>(s.c_str()));
            argv.push_back(nullptr);
            ::execv(cfg_.wineBinary.c_str(), argv.data());
            ::_exit(127);
        }
        if (rp > 0) {
            LOGI("WineHostProcess: launched rpcss.exe pid=%d; waiting 1.5s for irpcss pipe", rp);
            ::usleep(1500 * 1000);
        } else {
            LOGE("WineHostProcess: rpcss fork failed: %s", std::strerror(errno));
        }
    }

    /* Mark the prefix as booted. Non-fatal if the touch fails. */
    int sfd = ::open(sentinel.c_str(), O_WRONLY | O_CREAT, 0644);
    if (sfd >= 0) ::close(sfd);
    return true;
}

WineHostProcess::WineHostProcess(Config cfg) : cfg_(std::move(cfg)) {}

WineHostProcess::~WineHostProcess() {
    /* Stop watchdog first so it can't observe pid_ going dead and
     * spam a "stuck" warning during teardown. */
    stopWatchdog();
    if (perfHintSession_) {
        const auto& api = perf_hint_api();
        if (api.closeSession) {
            api.closeSession(perfHintSession_);
        }
        perfHintSession_ = nullptr;
    }
    if (pid_ > 0) {
        killHard();
        int status = 0;
        ::waitpid(pid_, &status, 0);
        pid_ = -1;
    }
}

bool WineHostProcess::start() {
    if (cfg_.wineBinary.empty() || cfg_.primaryExe.empty()) {
        LOGE("WineHostProcess: wineBinary or primaryExe missing");
        return false;
    }
    if (::access(cfg_.wineBinary.c_str(), X_OK) != 0) {
        LOGE("WineHostProcess: %s not executable (%s)",
             cfg_.wineBinary.c_str(), std::strerror(errno));
        return false;
    }
    /* vstpoc 2026-05-25: auto-rewrite primaryExe to vst3_host.exe when the
     * first plugin path ends in .vst3. The Kotlin side ships both binaries
     * via stageAsset; staying in C++ keeps the format detection in one
     * place (don't need parallel Kotlin logic). The vst3 host speaks the
     * same VstpocShared protocol so everything else here is unchanged. */
    if (!cfg_.pluginPaths.empty()) {
        const std::string& first = cfg_.pluginPaths.front();
        if (first.size() > 5 &&
            ::strcasecmp(first.c_str() + first.size() - 5, ".vst3") == 0) {
            std::string vst3Exe = cfg_.primaryExe;
            auto slash = vst3Exe.find_last_of('/');
            std::string dir = (slash == std::string::npos) ? "" : vst3Exe.substr(0, slash + 1);
            vst3Exe = dir + "vst3_host.exe";
            if (::access(vst3Exe.c_str(), R_OK) == 0) {
                LOGI("WineHostProcess: .vst3 plugin detected, switching primaryExe %s -> %s",
                     cfg_.primaryExe.c_str(), vst3Exe.c_str());
                cfg_.primaryExe = vst3Exe;
            } else {
                LOGE("WineHostProcess: .vst3 plugin needs %s but it's not staged",
                     vst3Exe.c_str());
            }
        }
    }

    /* Only validate primaryExe as a unix file when it IS a path. A bare program
     * name (e.g. "msiexec" for the .msi → `msiexec /i` installer route) has no
     * unix file — wine resolves it from the Windows PATH (C:\windows\system32) —
     * so the access() check would wrongly fail it. */
    if (cfg_.primaryExe.find('/') != std::string::npos &&
        ::access(cfg_.primaryExe.c_str(), R_OK) != 0) {
        LOGE("WineHostProcess: primaryExe %s not readable (%s)",
             cfg_.primaryExe.c_str(), std::strerror(errno));
        return false;
    }

    /* Boot wine services (RpcSs etc.) before launching the plugin host.
     * Idempotent — sentinel file in the prefix short-circuits on later
     * launches. Failure is logged but non-fatal; some plugins work
     * without COM out-of-process and we don't want to block them. */
    if (!bootServicesIfNeeded()) {
        LOGE("WineHostProcess: service bootstrap failed; continuing anyway. "
             "Plugins that need RpcSs (X50II offline activation, etc.) may fail.");
    }

    // Build argv: [wine, primaryExe, [shmPath], [plugin1, plugin2, …]].
    // shmPath is omitted entirely (not even an empty string) when the
    // primaryExe is something other than vst_host.exe (installer mode).
    // Otherwise vst_host.exe argv[1]=shm_path, argv[2..]=plugin paths.
    std::vector<std::string> argv_owned = {
        cfg_.wineBinary,
        cfg_.primaryExe,
    };
    if (!cfg_.shmPath.empty()) argv_owned.push_back(cfg_.shmPath);
    for (const auto& p : cfg_.pluginPaths) argv_owned.push_back(p);
    for (const auto& a : cfg_.extraArgs)   argv_owned.push_back(a);

    const std::string hostLog = cfg_.logSuffix.empty()
        ? (cfg_.cacheDir + "/vst_host.log")
        : (cfg_.cacheDir + "/vst_host_" + cfg_.logSuffix + ".log");

    pid_t p = ::fork();
    if (p < 0) {
        LOGE("WineHostProcess: fork failed: %s", std::strerror(errno));
        return false;
    }
    if (p == 0) {
        // --- child ---
        // Redirect stdout/stderr to a log file so plugin-load failures are
        // diagnosable after the fact. Truncated each start.
        int lfd = ::open(hostLog.c_str(),
                         O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (lfd >= 0) {
            ::dup2(lfd, 1);
            ::dup2(lfd, 2);
            if (lfd > 2) ::close(lfd);
        }

        // LD_LIBRARY_PATH: Bionic's dynamic linker auto-includes the app's
        // nativeLibraryDir, but only when the binary IS the app's main
        // process. For our fork+execve'd wine subprocess, Bionic falls
        // back to the system default search path which doesn't include
        // nativeLibraryDir. Explicitly add it so libX11.so / libxcb.so /
        // libandroid-support.so (Termux-built X11 client libs we ship
        // under their original SONAMEs) are found by winex11.so.
        ::setenv("LD_LIBRARY_PATH", cfg_.nativeLibDir.c_str(), 1);

        // Wine environment. With native arm64 wine running under Bionic
        // there is no proot and no box64; wine binary, server, and PE DLLs
        // all live at real on-device paths.
        ::setenv("WINEPREFIX",   cfg_.winePrefix.c_str(),         1);
        ::setenv("WINELOADER",   cfg_.wineBinary.c_str(),         1);
        ::setenv("WINESERVER",   cfg_.wineserverBinary.c_str(),   1);
        ::setenv("WINEDLLPATH",  cfg_.wineDllPath.c_str(),        1);
        // Override wine's "look for ntdll.so next to my binary" default. The
        // symlinked wine binary's realpath points to nativeLibraryDir where
        // every file is named libwine_NNNN.so — wine can't find ntdll.so
        // there. WINELOADER_NTDLL points it at the friendly path. See
        // patches/wine/001-loader-ntdll-env-override.patch.
        {
            // wineRoot is dirname(wineBinary).parent — derive aarch64-unix dir.
            // wineBinary = <wineRoot>/bin/wine ⇒ <wineRoot>/lib/wine/aarch64-unix/ntdll.so
            std::string wineRoot = cfg_.wineBinary;
            auto pos = wineRoot.find_last_of('/');               // strip "wine"
            if (pos != std::string::npos) wineRoot.resize(pos);
            pos = wineRoot.find_last_of('/');                    // strip "bin"
            if (pos != std::string::npos) wineRoot.resize(pos);
            const std::string ntdll = wineRoot + "/lib/wine/aarch64-unix/ntdll.so";
            ::setenv("WINELOADER_NTDLL", ntdll.c_str(), 1);
            // WINEDATADIR: wine looks here for NLS files (locale.nls, etc.)
            // and font metadata. Default derivation goes via APK install dir
            // which we can't write to. WineSetup extracts the NLS tarball
            // into <wineRoot>/share/wine on first run.
            const std::string dataDir = wineRoot + "/share/wine";
            ::setenv("WINEDATADIR", dataDir.c_str(), 1);
            // WINEBINDIR: wine's exec_wineserver tries <bin_dir>/wineserver
            // via posix_spawn. posix_spawn always returns success (fork ok),
            // so the function never falls through to the WINESERVER env var
            // — the child just exits with 127 from a failed exec, and wine
            // gives up. Setting bin_dir explicitly to our real install path
            // makes the first attempt succeed.
            const std::string binDir = wineRoot + "/bin";
            ::setenv("WINEBINDIR", binDir.c_str(), 1);
            // WINEDLLDIR: load_ntdll / load_apiset_dll / load_wow64_ntdll
            // open `<dll_dir>/aarch64-windows/<name>.dll` DIRECTLY (not via
            // WINEDLLPATH). Default dll_dir is ntdll_dir = nativeLibraryDir,
            // where we don't have an aarch64-windows subdir. Override.
            const std::string dllDir = wineRoot + "/lib/wine";
            ::setenv("WINEDLLDIR", dllDir.c_str(), 1);
            /* vstpoc: Turnip via libadrenotools (Android-HAL driver), like
             * Winlator. MUST be duplicated here — the actual vst_host fork uses
             * this inline env block, not setupWineEnvChild (see
             * feedback_winehostprocess_dup_env). win32u patch 0030 calls
             * adrenotools_open_libvulkan(HOOKDIR=nativeLibraryDir,
             * DRIVERDIR, DRIVERNAME). Do NOT set VSTPOC_VULKAN_LOADER — patch
             * 0024 keeps the android_surface bridge (correct for the Android
             * libvulkan adrenotools returns). See setupWineEnvChild for detail. */
            /* Trailing slash is REQUIRED: adrenotools_open_libvulkan builds the
         * driver path as (customDriverDir + customDriverName) with no separator,
         * so without it the path becomes ".../turnipvulkan.ad07xx.so" and the
         * dlopen fails → silent fallback to system (Qualcomm) Vulkan, whose
         * broken VK_EXT_robustness2 nullDescriptor makes DXVK/D3D11 plugins
         * (AmpliTube) render black. */
        const std::string turnipDir = wineRoot + "/turnip/";
            ::setenv("VSTPOC_ADRENOTOOLS_HOOKDIR",   cfg_.nativeLibDir.c_str(), 1);
            ::setenv("VSTPOC_ADRENOTOOLS_DRIVERDIR", turnipDir.c_str(),         1);
            ::setenv("VSTPOC_ADRENOTOOLS_DRIVERNAME", "vulkan.ad07xx.so",       1);
            ::setenv("MESA_LOG_FILE", (cfg_.cacheDir + "/turnip.log").c_str(), 1);
            ::setenv("TU_DEBUG", "startup", 1);
            /* Redirect Turnip's shader cache to our writable cacheDir (its
             * default path is unwritable here → recompiles every run). MUST be
             * in this inline block too. */
            const std::string mesaCache = cfg_.cacheDir + "/mesa_shader_cache";
            ::mkdir(mesaCache.c_str(), 0700);
            ::setenv("MESA_SHADER_CACHE_DIR", mesaCache.c_str(), 1);
            ::setenv("XDG_CACHE_HOME", cfg_.cacheDir.c_str(), 1);
            ::setenv("HOME", cfg_.cacheDir.c_str(), 1);
            vstpocSetMesaZinkEnv(wineRoot, cfg_.nativeLibDir);
        }
        // HODLL64 / HODLL select which PE DLL implements WoW64-style x86
        // emulation. libarm64ecfex.dll is loaded by wine arm64ec when it
        // sees x86_64 PE code; libwow64fex.dll for i386 PE.
        // See dlls/wow64/syscall.c::get_cpu_dll_name() in wine source.
        ::setenv("HODLL64",      "libarm64ecfex.dll",             1);
        ::setenv("HODLL",        "libwow64fex.dll",               1);

        /* FEX-Emu code caching — same setup as setupWineEnvChild. MUST be
         * duplicated here because the actual vst_host fork uses this
         * inline env block, not setupWineEnvChild (see
         * feedback_winehostprocess_dup_env). FEX picks up paths from
         * FEX_APP_{CONFIG,DATA,CACHE}_LOCATION env vars; everything else
         * (EnableCodeCachingWIP etc) goes via Config.json in the config
         * dir. */
        {
            const std::string fexRoot   = cfg_.cacheDir + "/fex_aot";
            const std::string configDir = fexRoot + "/config/";
            const std::string dataDir   = fexRoot + "/data/";
            const std::string cacheDir  = fexRoot + "/cache/";
            ::mkdir(fexRoot.c_str(),   0700);
            ::mkdir(configDir.c_str(), 0700);
            ::mkdir(dataDir.c_str(),   0700);
            ::mkdir(cacheDir.c_str(),  0700);
            const std::string configFile = configDir + "Config.json";
            vstpocWriteFexConfig(configFile, fexRoot);
            ::setenv("FEX_APP_CONFIG_LOCATION", configDir.c_str(), 1);
            ::setenv("FEX_APP_DATA_LOCATION",   dataDir.c_str(),   1);
            ::setenv("FEX_APP_CACHE_LOCATION",  cacheDir.c_str(),  1);
        }

        // Point wine at the in-process X11 server (TCP loopback on
        // 127.0.0.1:6000+N, served by our X11NativeDisplay #N). Use TCP
        // form so winex11 doesn't bother with the /tmp/.X11-unix socket.
        {
            char displayBuf[64];
            std::snprintf(displayBuf, sizeof(displayBuf),
                          "127.0.0.1:%d", cfg_.displayNumber);
            ::setenv("DISPLAY", displayBuf, 1);
        }
        /* Parent VST2 plugin editors under a chromeless top-level host window
         * (vst_host.c VSTPOC_HOST_FRAME=popup) instead of WS_CHILD-of-desktop,
         * so wine activates them natively and the WS_CHILD activation patches
         * (0037-0040) become unnecessary. Set BEFORE vstpocApplyEnvFile so it
         * can be overridden via cache/wine_env.txt for A/B testing
         * (e.g. VSTPOC_HOST_FRAME=0 to fall back to the desktop-parent path). */
        ::setenv("VSTPOC_HOST_FRAME", "popup", 1);
        /* VST2 default (2026-06-10): single-thread JUCE — load each plugin on
         * its editor thread (vst_host.c). Fixes the X50II stompbox-menu storm
         * (frozen UI + xruns); X50II + LeCto verified. Duplicated from
         * setupWineEnvChild per feedback_winehostprocess_dup_env — THIS is the
         * real vst_host launch path. wine_env.txt (=0) overrides for A/B. */
        ::setenv("VSTPOC_LOAD_ON_EDITOR_THREAD", "1", 1);
        /* Plugin-specific env defaults must be set before wine_env.txt so local
         * testing can still override or disable them without a rebuild. */
        vstpocApplyPluginEnvDefaults(cfg_);
        /* vstpoc: file-driven env overrides (e.g. VSTPOC_STACK_PCT for patch 0035
         * thread-stack scaling — BIAS FX 2's tid 00a0 FEX overflow). This is the
         * REAL vst_host launch path, so the override MUST be here (the other copy
         * in setupWineEnvChild only covers wineboot/rpcss). Applied late so it
         * wins over the defaults above. */
        vstpocApplyEnvFile(cfg_.cacheDir);
        /* DXVK debug logging — duplicated from setupWineEnvChild because
         * the actual vst_host launch uses this inline block. Per
         * feedback_winehostprocess_dup_env: env vars must be in BOTH or
         * they silently no-op on vst_host. "info" (not "debug") — see the
         * setupWineEnvChild copy for why the per-shader debug dump is off. */
        ::setenv("DXVK_LOG_LEVEL", "info", 1);
        /* DXVK GPL off on Turnip — see setupWineEnvChild for the rationale
         * (GPL shader-compile path faults under FEX, crashed TONEX). */
        ::setenv("DXVK_CONFIG", "dxvk.enableGraphicsPipelineLibrary = False", 1);

        // FEX-Emu / vstpoc tunables. These were originally added only to
        // setupWineEnvChild (the wineboot path) but the actual vst_host
        // launch uses this inline block instead — so they had no effect on
        // the host until duplicated here. See setupWineEnvChild for the
        // per-var rationale.
        //
        // FEX_SMCCHECKS=full was an experiment to mask FEX JIT-cache
        // staleness for TH-U drag-drop. It worked but cost ~3-5× perf
        // (full JIT block revalidation on every execute). Reverted to
        // default ("mtrack").
        // vstpoc 2026-05-25 (later 3): TH-U VST3 works cleanly via
        // vst3_host without any TSO hacks — the VST2 crashes we were
        // working around are VST2-format-specific (different code path
        // in TH-U). Rolling back strict TSO to defaults to recover the
        // ~10-30% SIMD perf we sacrificed. Re-enable here if a future
        // plugin needs strict ordering. Defaults: HALFBARRIER=1 (fast),
        // VECTORTSO=0 (fast).
        // ::setenv("FEX_HALFBARRIERTSOENABLED", "0", 1);
        // ::setenv("FEX_VECTORTSOENABLED",      "1", 1);

        /* Electron verbose logging — for diagnosing manager early-exit
         * issues. Electron writes JS console output to stderr when this
         * is set; uncaught native-module exceptions and renderer-process
         * crashes get traced too. Cheap, only emits on errors. */
        ::setenv("ELECTRON_ENABLE_LOGGING", "1", 1);
        ::setenv("ELECTRON_ENABLE_STACK_DUMPING", "1", 1);
        /* vstpoc 2026-05-25 (later 3): MULTIBLOCK=0 was tried for TH-U
         * VST2 drag-drop crash, didn't help. VST3 path works without
         * it. Reverting to default (true = multi-block JIT) for perf. */
        // ::setenv("FEX_MULTIBLOCK", "0", 1);
        // vstpoc 2026-05-25: tried FEX_SMCCHECKS=full on vst_host (had only
        // been on wineboot before). Result: audio completely broken + GUI
        // hang on first popup interaction. Default (mtrack) is the only
        // viable setting for real-time audio + interactive UI. Do not set.

        // vstpoc 2026-05-24: FEX JIT-to-perf-map for simpleperf symbol
        // resolution. Writes /data/local/tmp/perf-<pid>.map per process,
        // mapping host JIT addresses to guest function names. With this
        // enabled, simpleperf's "unknown" 94% becomes "TH-U-64.dll+0xXX"
        // etc., so we can identify which guest function is spinning.
        // Library naming uses PE module headers (works for exported
        // functions); global naming gives a JIT_<addr> fallback for
        // anonymous blocks. Cheap at runtime (writes a few KB per minute
        // to a perf.map file).
        ::setenv("FEX_GLOBALJITNAMING", "1", 1);
        ::setenv("FEX_LIBRARYJITNAMING", "1", 1);
        // vstpoc 2026-05-24: also BlockJITNaming so each translated
        // block carries its guest address (format `JIT_0x<guest>_<host>`).
        // Lets us map a hot host PC back to a TH-U.dll guest offset and
        // identify the specific function by disassembling the PE.
        ::setenv("FEX_BLOCKJITNAMING", "1", 1);
        // Override the FEX JIT perf-map path. Default
        // /data/local/tmp/perf-<pid>.map is unwritable to
        // untrusted_app on Android; redirect to our own cacheDir so the
        // file can actually be created. Pull via adb after profiling.
        ::setenv("FEX_PERFMAP_DIR", cfg_.cacheDir.c_str(), 1);
        // Cycle detector RE-ENABLED 2026-05-24: without it, popup-button
        // click hangs hard enough that our Kotlin teardown timeout
        // SIGTERMs the host (69607 audio underruns observed). The
        // click-outside-to-dismiss crash at TH-U+0x1802DF0C3 happens
        // regardless of cycle detector state (use-after-free is a TH-U
        // bug unrelated to focus-message synth). So accept the dismiss
        // crash and keep popup interaction responsive — net better UX.
        // vstpoc 2026-05-25 (later 4): NO_DESTROY_DRAIN was a VST2-era
        // workaround. Reverting now that VST3 path works; revisit if a
        // future VST2 plugin regresses.
        // ::setenv("WINE_VSTPOC_NO_DESTROY_DRAIN", "1", 1);

        // vstpoc 2026-05-24: TH-U dismiss livelock confirmed CPU-bound at
        // 80% on the MessageManager thread (tid=24191 R-state with utime
        // advancing). Sync-focus rollback (WINE_VSTPOC_SYNC_FOCUS=1) did
        // NOT help — confirms the bottleneck is not in cross-thread focus
        // routing but in JUCE's own callback storm post-dismiss. Revert
        // to the async (patch 027) default and enable patch 028 server-
        // side coalescing of exact duplicate WM_USER+N posts: when the same
        // target/message/wparam/lparam callback is already pending in a queue,
        // drop further copies. Distinct callback pointers still run.
        // vstpoc Phase 3 (2026-05-24): superseded by patch 031
        // (deferred-focus emulation of JUCE 8's mouseActivateFlags fix).
        // Disable patches 028/029/030 throttles since the new defer
        // addresses the same TH-U livelock at its source (focus-regain
        // reentrancy mid-WindowProc). Keep the wine tree code so
        // re-enabling is one env edit if a future plugin needs them.
        /* vstpoc 2026-05-25 (later 3): patches 028 (coalesce) and 030
         * (throttle) were added to break the WM_USER+123 storm that
         * happened AFTER TH-U VST2 popup-dismiss surgical recovery. TH-U
         * VST3 doesn't enter that storm at all (no popup-dismiss crash,
         * no recovery, no follow-on storm). Reverting both to default
         * off. Wine source patches remain for future use; re-enable
         * with env-var values >0. */
        /* vstpoc 2026-05-29: RE-ENABLED 028 (re-ported its queue.c code to
         * 11.9 — it had been reverted on the bump, only the env stayed). The
         * old "VST3 doesn't storm" assumption is wrong: TH-U's editor-create
         * IS a WM_USER+123 storm livelock (watchdog: 5 threads futex-blocked
         * on win_data_mutex, editor never finishes CreateWindowExA → "Loading
         * editor…"). Coalesce caps the post flood so the lock frees up. */
        /* vstpoc: gate the WM-storm throttles on prefix KIND — plugin (v)
         * prefixes ON (JUCE storms), manager/Electron (e/installer) prefixes
         * OFF (Chromium msg-pump tax). MUST match the setupWineEnvChild copy
         * above (the dual-env-block trap). See feedback_wm_throttle_chromium_tax. */
        const bool vstpocPluginPfx = vstpocIsPluginPrefix(cfg_.winePrefix);
        ::setenv("WINE_VSTPOC_COALESCE_POSTS",   vstpocPluginPfx ? "1"   : "0", 1);
        ::setenv("WINE_VSTPOC_USER_STORM_BREAK", vstpocPluginPfx ? "500" : "0", 1);
        ::setenv("WINE_VSTPOC_TIMER_GAP_MS",   "0", 1);  /* patch 029 off (code not ported) */
        ::setenv("WINE_VSTPOC_POST_GAP_MS",      vstpocPluginPfx ? "50"  : "0", 1);
        /* vstpoc: registry open-handle cache (ntdll patch 0053) — non-plugin
         * (e/installer) prefixes ON, v-prefix plugins OFF. MUST match the
         * setupWineEnvChild copy above (dual-env-block trap). Kills AmpliTube's
         * ~7000/sec registry-IPC heartbeat. See feedback_amplitube_registry_heartbeat. */
        ::setenv("VSTPOC_REGCACHE", vstpocPluginPfx ? "0" : "1", 1);
        /* vstpoc: GPU present (winex11 patch 0048) — e/installer prefixes ON,
         * v-prefix OFF. Ships the editor AHB GPU->GPU (zero-copy) vs CPU readback
         * + XPutImage. MUST match the setupWineEnvChild copy above (dual-block
         * trap). BIAS verified. See feedback_bias_knob_drag_gpu_latency. */
        ::setenv("VSTPOC_AHB_PRESENT", vstpocPluginPfx ? "0" : "1", 1);
        ::setenv("WINE_VSTPOC_WM_STATE_UNLOCK","1", 1);  /* TH-U editor: drop lock across cross-thread send */
        // vstpoc 2026-05-25 (later 4): patches 031 (defer-focus) and
        // 032 (fingerprint-filter) were added for VST2-era TH-U
        // workarounds. Reverting both — VST3 path doesn't hit the
        // codepaths these patch. Wine source patches remain available;
        // re-enable by setting these vars to "1".
        // ::setenv("WINE_VSTPOC_DEFER_FOCUS_WHEN_BUSY", "1", 1);
        // ::setenv("WINE_VSTPOC_FINGERPRINT_FILTER", "1", 1);

        // Native file picker: tell our patched comdlg32 where the
        // picker-channel mmap file lives. Wine sees Linux paths as Z:\...
        // so we pass the *absolute Linux path* here; comdlg32 prepends
        // "\\??\\Z:\\" if needed before CreateFile-ing it. Empty cfg
        // disables the hijack; comdlg32 then falls through to wine's
        // builtin dialog.
        if (!cfg_.pickerShmPath.empty()) {
            ::setenv("VSTPOC_PICKER_PATH", cfg_.pickerShmPath.c_str(), 1);
        }

        // Performance: silence WINEDEBUG except crash-style errors.
        // The trace channels (+server,+module,+loaddll,+file,+winex11,
        // …) were essential for diagnosing wine-launch + CS-deadlock
        // bugs earlier in this branch, but they fire on every syscall
        // and module load — convolution plugins like LeCab call-out
        // a lot and starved the Oboe audio thread (~hundreds of
        // underruns/sec). Keep "err+all" so SIGSEGV / SIGSYS / etc
        // still surface in vst_host_pN.log; set WINEDEBUG explicitly
        // back to the trace cocktail when debugging.
        //
        // The installer flow IS one of those debugging cases — DLL
        // search paths during wow64 init are what we're investigating
        // for task #140. logSuffix=="installer" triggers extra channels
        // just for this role; the plugin role keeps the lean default
        // so audio doesn't underrun.
        if (cfg_.logSuffix == "installer") {
            // +ver captures every OS-version query — cheap (init-only) and
            // useful for diagnosing version-check failures. +reg would also
            // help but explodes log size (every registry hit), so leave it
            // off; flip on temporarily when version-spoof tuning is needed.
            ::setenv("WINEDEBUG",
                     "-all,err+all,trace+loaddll,trace+module,trace+ver", 1);
        } else {
            /* Build-time toggle for submenu / X11 driver tracing. Enable
             * temporarily when diagnosing menu-related bugs; flip back to
             * 0 for shipping builds since these channels can cause Oboe
             * underruns during heavy interaction. */
            /* vstpoc 2026-05-24: verbose tracing DISABLED.
             *
             * Why this matters: simpleperf profile of a hung TH-U
             * showed the MessageManager thread spending 53% of CPU
             * inside NtUserPeekMessage, of which 16%+ was the trace
             * pipeline (__wine_dbg_header → snprintf → __sfvwrite →
             * write to log fd). Add FEX's per-instruction overhead on
             * the snprintf/sfvwrite hot path and the trace pipeline
             * became the single largest CPU sink during JUCE's
             * dismiss-callback flood. Vst_host log grew to 700+ MB.
             *
             * Earlier comment (now obsolete): "trace re-enabled. Disabling
             * dropped patch 025's phase 1 wake success from 99.4% to 17%."
             * That reasoning depended on cycle detector being the focus-
             * deadlock mitigation. Patch 027 made cross-thread focus
             * sends async (fire-and-forget), so the cycle detector is
             * largely vestigial; we don't need tracing as accidental
             * backoff anymore.
             *
             * Keep `err+all` (any wine ERR log surfaces) and `trace+seh`
             * (full register dump on exception — cheap until an actual
             * SEH event fires; needed to diagnose plugin crashes). Drop
             * all the hot channels (+msg, +win, +sync, +event, +file,
             * +mouse, +cursor, +x11drv, …) — each of these adds N trace
             * writes per *every* wine syscall on the message hot path. */
            /* Re-enabled 2026-05-24 after test confirmed disabling
             * regressed TH-U from "3 dismisses to repro" to "1 dismiss
             * reliably hangs". The tracing-as-implicit-backoff is real:
             * the per-call overhead slows JUCE's PeekMessage polling
             * enough that the receiver thread actually drains its
             * backlog. Disabling it removed that backoff and tightened
             * the busy-poll loop into immediate livelock. Until we have
             * a deliberate backoff (sched_yield after N consecutive
             * empty peeks, or similar), keep tracing on as the only
             * mitigation that empirically works. */
            constexpr bool kVerboseTraceEnabled = false;
            if (kVerboseTraceEnabled) {
                /* Diagnostic mode: re-enable when investigating a new
                 * message-flow / X11 / wineserver bug. Expect 700MB+
                 * vst_host logs and visible UI lag. */
                ::setenv("WINEDEBUG", "-all,err+all,trace+seh,trace+menu,trace+x11drv,trace+event,trace+msg,trace+win,trace+key,trace+sync,trace+wininet,trace+winsock,trace+mouse,trace+cursor,trace+iphlpapi,trace+netapi32,trace+reg,trace+file", 1);
            } else {
                /* vstpoc 2026-06-01: DROPPED trace+seh. The old comment claimed
                 * it's "cheap until an SEH event fires" — true for normal plugins
                 * (rare exceptions), CATASTROPHIC for AmpliTube, whose IK::ATK
                 * init throws ~47k C++ exceptions across 4 threads as normal
                 * control-flow (property-bag probing). With trace+seh every throw
                 * runs the full SEH trace path (register dump + format + write
                 * under wine's debug-log lock) AND FEX holds its internal unwind
                 * CS longer → the threads deadlock on FEX's exception lock
                 * (0x…E2E478 in libarm64ecfex.dll) before init completes →
                 * editor renderer never created → black screen. Our own VEH
                 * (pluginmain_veh) still logs throw types + AV backtraces, so we
                 * lose no crash diagnostics. Re-enable trace+seh only when
                 * debugging a specific non-AmpliTube crash. */
                ::setenv("WINEDEBUG", "-all,err+all", 1);
            }
        }

        // Make the wine process attachable by lldb-server. Without
        // PR_SET_DUMPABLE=1, Android's selinux + dumpable check denies
        // ptrace even from the same UID. Required for gdb/lldb debugging
        // of JUCE state-machine deadlocks (task #150).
        if (::prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) != 0) {
            std::fprintf(stderr, "[launcher] prctl(PR_SET_DUMPABLE, 1) failed: %s\n",
                std::strerror(errno));
        }

        // Pin to the big-core cluster so the audio worker isn't migrated to
        // an A510 mid-callback.
        unsigned long mask = set_big_core_affinity();
        if (mask) {
            std::fprintf(stderr, "[launcher] big-core mask=0x%lx\n", mask);
        } else {
            std::fprintf(stderr, "[launcher] sched_setaffinity failed: %s\n",
                std::strerror(errno));
        }

        /* vstpoc: re-apply file env overrides LAST (after the WINEDEBUG/FEX/DXVK
         * setenvs above) so wine_env.txt can also override WINEDEBUG — e.g.
         * "WINEDEBUG=+gdi,+tid" to trace the BIAS FX 2 tid 00a0 runaway recursion
         * (creates a GDI object per level). The earlier call (post-DISPLAY) covers
         * vars read before this point; this one wins for everything. */
        vstpocApplyEnvFile(cfg_.cacheDir);

        // vstpoc 2026-05-24 (Fix A.1 — priority-inversion mitigation):
        // Reverted from nice=-10 to nice=0. Bionic does NOT implement
        // PTHREAD_PRIO_INHERIT despite defining the constant, so high-prio
        // wine threads can starve waiting on a JUCE worker holding a JUCE
        // mutex (e.g. MessageManagerLock during popup dismiss) — observed
        // as 30–53s watchdog stuck reports on TH-U dismiss. Audio thread
        // priority is now handled by the PerformanceHint API (createSession
        // below); no need for raw process nice.
        if (::setpriority(PRIO_PROCESS, 0, 0) != 0) {
            std::fprintf(stderr, "[launcher] setpriority(0) failed: %s\n",
                std::strerror(errno));
        } else {
            std::fprintf(stderr, "[launcher] nice=0 set (was -10 — see Fix A.1)\n");
        }

        // chdir to primaryExe's parent so Windows apps that look for
        // sibling resource files via CWD-relative paths can find them.
        // Qt apps use applicationDirPath() (exe-relative, not CWD-relative)
        // but many non-Qt apps — installers, license-checkers, IK Multimedia
        // Product Manager — call LoadLibraryA("foo.dll") or fopen("config.ini")
        // expecting CWD to be the exe's directory. Without this the child
        // inherits the app process's working dir which is something like
        // /data/user/0/com.varcain.guitarrackcraft (or /), no resources there.
        if (!cfg_.primaryExe.empty()) {
            auto slash = cfg_.primaryExe.find_last_of('/');
            if (slash != std::string::npos) {
                std::string dir = cfg_.primaryExe.substr(0, slash);
                if (::chdir(dir.c_str()) != 0) {
                    std::fprintf(stderr, "[launcher] chdir(%s) failed: %s\n",
                        dir.c_str(), std::strerror(errno));
                }
            }
        }

        // execve the wine binary directly.
        std::vector<char*> argv;
        argv.reserve(argv_owned.size() + 1);
        for (auto& s : argv_owned) argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);
        ::execv(cfg_.wineBinary.c_str(), argv.data());
        ::_exit(127);
    }

    // --- parent ---
    pid_ = p;
    hostLog_ = hostLog;
    LOGI("WineHostProcess: launched pid=%d (primary=%s, %zu plugins, log=%s)",
         pid_, cfg_.primaryExe.c_str(), cfg_.pluginPaths.size(),
         hostLog_.c_str());

    // Tell the kernel scheduler we expect this thread to do real-time work
    // on a fixed cadence. PerformanceHint is the Android-blessed alternative
    // to SCHED_FIFO (which untrusted_app can't use). The session targets the
    // wine subprocess's main TID — for a freshly fork+execve'd process that
    // equals its PID. Target work duration: 6 ms per 10.67 ms audio block
    // (kBlockSize=512 @ 48 kHz). API 33+; on older Android the dlsym lookup
    // returns null and the session is silently skipped.
    const auto& api = perf_hint_api();
    if (api.available()) {
        APerformanceHintManagerHandle* mgr = api.getManager();
        if (mgr) {
            int32_t tids[1] = { static_cast<int32_t>(pid_) };
            const int64_t targetNs = 6 * 1000 * 1000;  // 6 ms
            perfHintSession_ = api.createSession(mgr, tids, 1, targetNs);
            if (perfHintSession_) {
                LOGI("PerformanceHint: session created for tid=%d target=%lldns",
                     (int)pid_, (long long)targetNs);
            } else {
                LOGW("PerformanceHint: createSession returned null (likely <API 33 vendor build)");
            }
        }
    } else {
        LOGI("PerformanceHint: API not available (older Android)");
    }

    /* vstpoc 2026-05-24: kick off per-thread liveness watchdog so the
     * next time a wine thread "stalls" we can tell from the logs
     * whether it's CPU-bound (FEX JIT loop) or kernel-blocked. */
    startWatchdog();

    return true;
}

bool WineHostProcess::waitFor(int timeoutMs) {
    if (pid_ <= 0) return true;
    const int slice_ms = 10;
    int waited = 0;
    while (waited < timeoutMs) {
        int status = 0;
        pid_t r = ::waitpid(pid_, &status, WNOHANG);
        if (r == pid_) {
            LOGI("WineHostProcess: pid=%d exited (status=%d)", pid_, status);
            pid_ = -1;
            return true;
        }
        if (r < 0 && errno == ECHILD) {
            pid_ = -1;
            return true;
        }
        struct timespec ts = { 0, slice_ms * 1000000L };
        ::nanosleep(&ts, nullptr);
        waited += slice_ms;
    }
    return false;
}

void WineHostProcess::killHard() {
    if (pid_ <= 0) return;
    stopWatchdog();
    LOGW("WineHostProcess: sending SIGTERM to pid=%d", pid_);
    ::kill(pid_, SIGTERM);
    if (waitFor(800)) return;
    LOGW("WineHostProcess: SIGTERM ignored, sending SIGKILL");
    ::kill(pid_, SIGKILL);
    waitFor(500);
}

/* vstpoc watchdog: per-thread CPU-time/state poller for diagnosing the
 * "wine thread stuck" symptom. Reads /proc/<pid>/task/<tid>/stat for
 * every task in the host process every 1 s; if a task's utime+stime
 * stays flat for ≥ 2 consecutive polls AND its state is R (runnable
 * — i.e. genuinely stuck in CPU-bound code without progressing) or D
 * (uninterruptible wait), logs a warning. Distinguishes "blocked on
 * futex" (state=S, fine, expected) from "looping in FEX JIT" (state=R,
 * total time not advancing — suspicious). Free / no perf cost when no
 * thread is stuck. */
void WineHostProcess::startWatchdog() {
    if (pid_ <= 0) return;
    watchdogRunning_ = true;
    watchdogThread_ = std::thread([this, pid = pid_]() {
        struct TS {
            long lastTotal = -1;
            int  stuckPolls = 0;
            char lastState = '?';
            bool pinned = false;   /* big-core affinity already applied? */
        };
        std::unordered_map<int, TS> states;
        int pollCount = 0;
        /* vstpoc 2026-05-25: ESC-on-touch hot detection. Track cumulative
         * process CPU each poll (sum of all tasks' utime+stime). A jiffy
         * is 10 ms on Bionic, so 100 jiffies/s = 100% of one core. We
         * declare the host "hot" when delta >= 90 jiffies/s (≈ pegging
         * one core) for two consecutive polls — that's our signal of the
         * MessageManager/AsyncUpdater storm. Reset to zero the moment
         * the rate drops back to sane levels. */
        long lastProcessTotal = -1;
        int  hotStreak        = 0;

        while (watchdogRunning_.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!watchdogRunning_.load()) break;
            pollCount++;

            char taskDir[64];
            std::snprintf(taskDir, sizeof(taskDir), "/proc/%d/task", pid);
            DIR* d = ::opendir(taskDir);
            if (!d) {
                /* Process gone — exit cleanly. */
                if (errno == ENOENT) break;
                continue;
            }

            std::unordered_set<int> seenTids;
            while (struct dirent* ent = ::readdir(d)) {
                int tid = std::atoi(ent->d_name);
                if (tid <= 0) continue;
                seenTids.insert(tid);

                /* vstpoc 2026-05-24 (Fix A.2): pin each tid to big cores
                 * on first sight, then re-pin every 5 s. Defeats Android
                 * cgroup demotion of background-spawned threads (which
                 * causes 30-50 s stalls under TH-U popup-dismiss load). */
                auto& sRef = states[tid];
                if (!sRef.pinned || (pollCount % 5) == 0) {
                    pin_tid_to_big_cores(tid);
                    sRef.pinned = true;
                }

                char statPath[128];
                std::snprintf(statPath, sizeof(statPath),
                              "/proc/%d/task/%d/stat", pid, tid);
                int sf = ::open(statPath, O_RDONLY);
                if (sf < 0) continue;
                char buf[512];
                ssize_t n = ::read(sf, buf, sizeof(buf) - 1);
                ::close(sf);
                if (n <= 0) continue;
                buf[n] = 0;

                /* /proc/[pid]/task/[tid]/stat format:
                 *   pid (comm) state ppid pgrp ... utime(14) stime(15) ...
                 * comm may contain spaces and parens, so find last ')'. */
                char* commEnd = std::strrchr(buf, ')');
                if (!commEnd || !commEnd[1] || !commEnd[2]) continue;
                char* p = commEnd + 2;            /* skip ") " */
                char state = *p;
                /* Advance past 11 more whitespace-separated fields to reach utime. */
                int skipped = 1;
                while (*p && skipped < 12) {
                    while (*p && *p != ' ') p++;
                    while (*p == ' ')       p++;
                    skipped++;
                }
                long utime = std::atol(p);
                while (*p && *p != ' ') p++;
                while (*p == ' ')       p++;
                long stime = std::atol(p);
                long total = utime + stime;

                auto& s = states[tid];
                if (s.lastTotal == -1) {
                    s.lastTotal = total;
                    s.lastState = state;
                    continue;
                }
                if (total == s.lastTotal) {
                    s.stuckPolls++;
                    /* R/D states stuck even for 2 s is a strong CPU-bound /
                     * uninterruptible-wait signal: warn early. */
                    if (s.stuckPolls >= 2 && (state == 'R' || state == 'D')) {
                        LOGE("vstpoc watchdog: pid=%d tid=%d state=%c stuck=%ds "
                             "utime+stime=%ld (no CPU progress — possible "
                             "JIT livelock or kernel wait)",
                             pid, tid, state, s.stuckPolls, total);
                        dump_stuck_thread_kernel_state(pid, tid);
                    }
                    /* vstpoc 2026-05-24: also warn on state='S' (sleeping
                     * on syscall) when stuck > 5 s — this is what
                     * priority-inversion / futex-starvation looks like
                     * during TH-U popup dismiss. Logged at WARN not ERROR
                     * because parked threads are common, but a 5+ s park
                     * with the process otherwise idle is a stall worth
                     * flagging. */
                    else if (s.stuckPolls == 5 && state == 'S') {
                        LOGW("vstpoc watchdog: pid=%d tid=%d state=S stuck=%ds "
                             "(futex-blocked — possible JUCE lock starvation)",
                             pid, tid, s.stuckPolls);
                        dump_stuck_thread_kernel_state(pid, tid);
                    }
                } else {
                    if (s.stuckPolls >= 2) {
                        LOGI("vstpoc watchdog: pid=%d tid=%d recovered after %ds",
                             pid, tid, s.stuckPolls);
                    }
                    s.stuckPolls = 0;
                    s.lastTotal = total;
                    s.lastState = state;
                }
            }
            ::closedir(d);

            /* Drop dead tids from the state map so they don't leak. */
            for (auto it = states.begin(); it != states.end();) {
                if (seenTids.find(it->first) == seenTids.end()) it = states.erase(it);
                else                                            ++it;
            }

            /* vstpoc 2026-05-25: process-level CPU sample from /proc/<pid>/stat
             * (utime + stime — already aggregated across threads). 90 jiffies
             * delta per second ≈ pegging one core; 3 s of that is our
             * "MessageManager storm" trigger. */
            char psPath[64];
            std::snprintf(psPath, sizeof(psPath), "/proc/%d/stat", pid);
            int psf = ::open(psPath, O_RDONLY);
            if (psf >= 0) {
                char buf[512];
                ssize_t n = ::read(psf, buf, sizeof(buf) - 1);
                ::close(psf);
                if (n > 0) {
                    buf[n] = 0;
                    char* commEnd = std::strrchr(buf, ')');
                    if (commEnd && commEnd[1] && commEnd[2]) {
                        char* p = commEnd + 2;
                        int skipped = 1;
                        while (*p && skipped < 12) {
                            while (*p && *p != ' ') p++;
                            while (*p == ' ')       p++;
                            skipped++;
                        }
                        long utime = std::atol(p);
                        while (*p && *p != ' ') p++;
                        while (*p == ' ')       p++;
                        long stime = std::atol(p);
                        long pTotal = utime + stime;
                        if (lastProcessTotal >= 0) {
                            long delta = pTotal - lastProcessTotal;
                            if (delta >= 90) {
                                if (++hotStreak >= 3) {
                                    int prev = g_vstpoc_hot_seconds.exchange(
                                        hotStreak, std::memory_order_release);
                                    if (prev == 0) {
                                        LOGW("vstpoc watchdog: host CPU HOT "
                                             "(delta=%ld jiffies/s, streak=%ds) — "
                                             "ESC-on-touch dismiss armed",
                                             delta, hotStreak);
                                    }
                                }
                            } else {
                                if (hotStreak >= 3) {
                                    LOGI("vstpoc watchdog: host CPU cooled "
                                         "(delta=%ld jiffies/s) — disarming "
                                         "ESC-on-touch after %ds streak",
                                         delta, hotStreak);
                                }
                                hotStreak = 0;
                                g_vstpoc_hot_seconds.store(0, std::memory_order_release);
                            }
                        }
                        lastProcessTotal = pTotal;
                    }
                }
            }
        }
        LOGI("vstpoc watchdog: thread exiting (pid=%d)", pid);
        g_vstpoc_hot_seconds.store(0, std::memory_order_release);
    });
}

void WineHostProcess::stopWatchdog() {
    watchdogRunning_ = false;
    if (watchdogThread_.joinable()) watchdogThread_.join();
}
