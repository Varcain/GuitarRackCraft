#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <sys/types.h>

// Long-running wine-based guest. fork() + execve() of our cross-compiled
// native arm64 wine binary (under Bionic, no proot, no box64). Wine loads
// libarm64ecfex.dll / libwow64fex.dll in-process via HODLL64/HODLL env vars
// to emulate x86_64 / x86 PE code. The launched process is
// `<wineBinary> <primaryExe> [<shmPath>] [<plugin1> <plugin2> …]`.
//
// Two roles today:
//   1. Plugin host (primaryExe = vst_host.exe, shmPath set, pluginPaths non-empty).
//      Communicates with the Android host via the VstpocShared mmap layout.
//   2. Installer (primaryExe = some installer.exe, shmPath empty, pluginPaths empty).
//      No IPC; the wine GUI is rendered via DISPLAY into the Android X server
//      and the subprocess exits when the user closes the installer wizard.
class WineHostProcess {
public:
    struct Config {
        // App-side paths (used by the parent fork to validate things exist).
        std::string nativeLibDir;       // /data/app/.../lib/arm64
        std::string cacheDir;           // <filesDir>/../cache (for vst_host_*.log)

        // Wine install paths.
        std::string wineBinary;         // <filesDir>/wine/bin/wine  (symlink → libwine_fNNN.so)
        std::string wineserverBinary;   // <filesDir>/wine/bin/wineserver
        std::string wineDllPath;        // <filesDir>/wine/lib/wine/aarch64-windows
        std::string winePrefix;         // <filesDir>/wineprefix (or a template/clone)

        // Absolute Unix paths to the wine-side executables and rings.
        // Wine maps `/` to `Z:` so these become Z:\... paths inside wine.
        // primaryExe is the wine argv[1] — typically vst_host.exe for the
        // plugin role or an installer .exe for the installer role.
        std::string primaryExe;         // e.g. <filesDir>/tmp/vst_host.exe (or installer.exe)
        std::string shmPath;            // VstpocShared mmap (empty = no IPC; installer mode)
        std::string pickerShmPath;      // e.g. <filesDir>/tmp/vst_picker_p0.dat (empty = no native picker)
        // Plugins to load + chain. argv order = signal flow order. Empty
        // when primaryExe is anything other than vst_host.exe.
        std::vector<std::string> pluginPaths;
        // Extra arguments appended verbatim after primaryExe + shmPath +
        // pluginPaths. Used to pass flags through to the wine subprocess
        // (e.g. `--update` for wineboot, or any future trailing CLI args).
        std::vector<std::string> extraArgs;

        // X11 display number this wine process should connect to. The
        // in-process X server listens on TCP 127.0.0.1:(6000+displayNumber).
        // Used to spawn separate wine instances rendering to separate
        // SurfaceViews — one per plugin in the multi-process chain.
        int displayNumber = 0;
        // Unique log filename suffix so multiple WineHostProcess instances
        // don't trample each other's vst_host.log. Default empty = single
        // instance writes to vst_host.log; set to e.g. "p1" for vst_host_p1.log.
        std::string logSuffix;
    };

    explicit WineHostProcess(Config cfg);
    ~WineHostProcess();

    WineHostProcess(const WineHostProcess&) = delete;
    WineHostProcess& operator=(const WineHostProcess&) = delete;

    bool start();
    /** Run the one-time per-prefix service bootstrap without launching
     *  primaryExe. Used by import flows to move first-rack-add setup cost
     *  out of the live rack path. */
    bool bootstrapServices();
    bool waitFor(int timeoutMs);
    void killHard();
    bool isRunning() const { return pid_ > 0; }
    pid_t pid() const { return pid_; }
    const std::string& logPath() const { return hostLog_; }

private:
    // Set wine env vars (WINEPREFIX, WINELOADER, WINEDLLPATH, …) in the
    // current process. Shared by start()'s vst_host fork-child and by
    // bootServicesIfNeeded()'s wineboot fork-child. Inherits cfg by ref;
    // safe to call only AFTER fork (mutates process env).
    static void setupWineEnvChild(const Config& cfg);

    // One-time per-prefix bootstrap of wine services (RpcSs etc.). Without
    // this, COM out-of-process marshalling fails with
    // RPC_S_SERVER_UNAVAILABLE — X50II's offline-activation flow hits
    // CoMarshalInterface and silently bails when the SCM doesn't know
    // about RpcSs. wineboot.exe -i populates default service registry
    // entries and starts services.exe, which then auto-starts RpcSs.
    // Sentinel file in the prefix marks it as booted so subsequent plugin
    // launches skip the (~5s) bootstrap.
    bool bootServicesIfNeeded();

    // vstpoc 2026-05-24: per-thread liveness watchdog. Polls each
    // task's stat file under /proc/<pid_>/task/ every 1 s; logs any
    // tid whose utime+stime has been flat for >=2 polls. Distinguishes
    // CPU-bound livelock (state=R, time not advancing) from blocked
    // syscall (state=S, normal idle) from uninterruptible wait
    // (state=D, suspicious). Vital diagnostic for the popup deadlock.
    void startWatchdog();
    void stopWatchdog();

    Config cfg_;
    pid_t pid_ = -1;
    std::string hostLog_;
    // Opaque handle for APerformanceHint_createSession (libandroid, API 33+).
    // Stored as void* because we dlsym the API rather than link directly —
    // minSdk is 27 so we can't reference the symbols at build time.
    void* perfHintSession_ = nullptr;
    std::atomic<bool> watchdogRunning_{false};
    std::thread watchdogThread_;
};
