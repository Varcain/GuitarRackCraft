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

/* Set wine env vars in the current process. Used by both the vst_host
 * fork child (start()) and the wineboot fork child (bootServicesIfNeeded()).
 * Safe to call only after fork — mutates ::setenv. */
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
    }
    ::setenv("HODLL64",      "libarm64ecfex.dll",             1);
    ::setenv("HODLL",        "libwow64fex.dll",               1);
    /* vstpoc experiment 2026-05-23: force FEX-Emu to fully revalidate every
     * JIT block before execution. If TH-U's drag-drop +2-byte mid-instruction
     * landing is from stale translation-cache entries (FEX issue #5328 type
     * mechanism), this masks it. Default is "mtrack" (memory-tracking-based
     * invalidation); "full" is slower but ironclad. Drop back to default if
     * the experiment is inconclusive. */
    ::setenv("FEX_SMCCHECKS", "full", 1);
    /* vstpoc experiment 2026-05-23 (hypothesis 1 — FEX memory ordering):
     * Half-barrier TSO optimization can leave aligned loadstores
     * non-atomic in rare cases (per FEX config docs). Force strict
     * barriers to test whether TH-U's drag-drop NULL deref is a
     * stale-read race that FEX's TSO emulation isn't catching. */
    ::setenv("FEX_HALFBARRIERTSOENABLED", "0", 1);
    ::setenv("FEX_VECTORTSOENABLED", "1", 1);

    /* vstpoc patch 025: cycle detector synthesizes 0-replies for stuck
     * focus messages. Helps popup-button-click on JUCE-7, but causes
     * TH-U crashes at +0x1802DF0C3 on click-outside-to-dismiss (synth
     * fires while WindowProc is still mid-state-setup → null deref).
     * Disabled per user 2026-05-23: prefer hangs over crashes. Click
     * outside / popup interaction may freeze briefly while TH-U's slow
     * WindowProcs complete naturally, but state stays consistent. */
    ::setenv("WINE_VSTPOC_NO_CYCLE_DETECT", "1", 1);
    {
        char displayBuf[64];
        std::snprintf(displayBuf, sizeof(displayBuf),
                      "127.0.0.1:%d", cfg.displayNumber);
        ::setenv("DISPLAY", displayBuf, 1);
    }
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

    if (::access(cfg_.primaryExe.c_str(), R_OK) != 0) {
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
        }
        // HODLL64 / HODLL select which PE DLL implements WoW64-style x86
        // emulation. libarm64ecfex.dll is loaded by wine arm64ec when it
        // sees x86_64 PE code; libwow64fex.dll for i386 PE.
        // See dlls/wow64/syscall.c::get_cpu_dll_name() in wine source.
        ::setenv("HODLL64",      "libarm64ecfex.dll",             1);
        ::setenv("HODLL",        "libwow64fex.dll",               1);

        // Point wine at the in-process X11 server (TCP loopback on
        // 127.0.0.1:6000+N, served by our X11NativeDisplay #N). Use TCP
        // form so winex11 doesn't bother with the /tmp/.X11-unix socket.
        {
            char displayBuf[64];
            std::snprintf(displayBuf, sizeof(displayBuf),
                          "127.0.0.1:%d", cfg_.displayNumber);
            ::setenv("DISPLAY", displayBuf, 1);
        }

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
        // side coalescing of WM_USER+N posts: when >= 200 same-target
        // same-msg posts already pending in a queue, drop further ones.
        // Lossy (drops JUCE async callbacks) but breaks the runaway.
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
        ::setenv("WINE_VSTPOC_COALESCE_POSTS", "0", 1);  /* patch 028 off */
        ::setenv("WINE_VSTPOC_TIMER_GAP_MS",   "0", 1);  /* patch 029 off */
        ::setenv("WINE_VSTPOC_POST_GAP_MS",    "0", 1);  /* patch 030 off */
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
            ::setenv("WINEDEBUG", "-all,err+all,trace+loaddll,trace+module", 1);
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
            constexpr bool kVerboseTraceEnabled = true;
            if (kVerboseTraceEnabled) {
                /* Diagnostic mode: re-enable when investigating a new
                 * message-flow / X11 / wineserver bug. Expect 700MB+
                 * vst_host logs and visible UI lag. */
                ::setenv("WINEDEBUG", "-all,err+all,trace+seh,trace+menu,trace+x11drv,trace+event,trace+msg,trace+win,trace+key,trace+sync,trace+wininet,trace+winsock,trace+mouse,trace+cursor,trace+iphlpapi,trace+netapi32,trace+reg,trace+file", 1);
            } else {
                ::setenv("WINEDEBUG", "-all,err+all,trace+seh", 1);
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
