// JNI surface for vsthost_lib's non-X11 runtime bindings.
//
// What this file exposes (everything called from Kotlin via NativeBridge):
//   - nativeInspectPluginExports: classify a PE file as VST2/VST3/x86/x64
//     without loading it. Used by the import flow to validate plugins.
//   - nativeRunWineboot: synchronous `wineboot.exe --update` against a
//     prefix. Runs once per prefix so wine.inf's RegisterDllsSection
//     fires (drag-drop / WinRT marshalling depends on it).
//
// X11 bindings live in jni_x11.cpp. The IPlugin bridge (VstFactory /
// WineVstPlugin) lives in vst/ and is linked into :app via prefab — no
// JNI in this path; :app calls into it as native C++.

#include "launcher/WineHostProcess.h"
#include "util/PeExports.h"
#include "util/log.h"

extern "C" {
#include "../../../external/shared_layout.h"
}

#include <jni.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <string>
#include <fstream>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
// Singleton for the running installer wine subprocess. Only one installer
// at a time — the lock prevents concurrent start/wait/kill from racing,
// not the installer itself from ANR'ing the audio path (it runs in its
// own wine child process and never touches Oboe).
std::mutex installer_mutex;
std::unique_ptr<WineHostProcess> g_installer;
}  // namespace

extern "C" {

JNIEXPORT jint JNICALL
Java_com_varcain_vsthost_NativeBridge_nativeInspectPluginExports(
    JNIEnv* env, jobject /*thiz*/, jstring jPath) {
    if (!jPath) return 0;
    const char* s = env->GetStringUTFChars(jPath, nullptr);
    if (!s) return 0;
    std::string path(s);
    env->ReleaseStringUTFChars(jPath, s);
    vstpoc::pe::ExportInfo info = vstpoc::pe::inspect(path);
    jint flags = 0;
    if (info.valid)             flags |= 1 << 0;
    if (info.isDll)             flags |= 1 << 1;
    if (info.is64Bit)           flags |= 1 << 2;
    if (info.hasVstPluginMain)  flags |= 1 << 3;
    if (info.hasVst3Factory)    flags |= 1 << 4;
    return flags;
}

JNIEXPORT jint JNICALL
Java_com_varcain_vsthost_NativeBridge_nativeRunWineboot(
    JNIEnv* env, jobject /*thiz*/,
    jstring jPrefixPath,
    jstring jWineBinary, jstring jWineserverBinary, jstring jWineDllPath,
    jstring jNativeLibDir, jstring jCacheDir,
    jint timeoutSec) {
    auto jstr = [&](jstring s) -> std::string {
        if (!s) return {};
        const char* c = env->GetStringUTFChars(s, nullptr);
        if (!c) return {};
        std::string r(c);
        env->ReleaseStringUTFChars(s, c);
        return r;
    };
    const std::string prefixPath = jstr(jPrefixPath);
    const std::string wineBinary = jstr(jWineBinary);
    const std::string wineserverBinary = jstr(jWineserverBinary);
    const std::string wineDllPath = jstr(jWineDllPath);
    const std::string nativeLibDir = jstr(jNativeLibDir);
    const std::string cacheDir = jstr(jCacheDir);

    if (wineDllPath.empty() || wineBinary.empty()) {
        LOGE("nativeRunWineboot: missing wine paths");
        return -1;
    }
    const std::string winebootExe = wineDllPath + "/wineboot.exe";
    if (::access(winebootExe.c_str(), R_OK) != 0) {
        LOGE("nativeRunWineboot: wineboot.exe not readable at %s", winebootExe.c_str());
        return -1;
    }

    WineHostProcess::Config cfg{
        .nativeLibDir       = nativeLibDir,
        .cacheDir           = cacheDir,
        .wineBinary         = wineBinary,
        .wineserverBinary   = wineserverBinary,
        .wineDllPath        = wineDllPath,
        .winePrefix         = prefixPath,
        .primaryExe         = winebootExe,
        .shmPath            = {},
        .pickerShmPath      = {},
        .pluginPaths        = {},
        .extraArgs          = { std::string("--update") },
        .displayNumber      = 0,  // headless
        .logSuffix          = "wineboot",
    };
    WineHostProcess proc(std::move(cfg));
    if (!proc.start()) {
        LOGE("nativeRunWineboot: failed to start");
        return -1;
    }
    pid_t pid = proc.pid();
    LOGI("nativeRunWineboot: launched pid=%d (timeout=%ds)", (int)pid, (int)timeoutSec);

    // Roll our own waitpid loop so we can return wineboot's actual exit code —
    // WineHostProcess::waitFor() reaps internally and discards it.
    const int timeoutMs = (timeoutSec > 0 ? timeoutSec : 120) * 1000;
    const int sliceMs = 25;
    int waited = 0;
    while (waited < timeoutMs) {
        int status = 0;
        pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            int exitCode = WIFEXITED(status) ? WEXITSTATUS(status)
                         : WIFSIGNALED(status) ? (128 + WTERMSIG(status))
                         : -1;
            LOGI("nativeRunWineboot: exited code=%d (status=0x%x)", exitCode, status);
            return static_cast<jint>(exitCode);
        }
        if (r < 0 && errno != EINTR) {
            LOGE("nativeRunWineboot: waitpid failed (%s)", std::strerror(errno));
            return -1;
        }
        struct timespec ts = { 0, sliceMs * 1000000L };
        ::nanosleep(&ts, nullptr);
        waited += sliceMs;
    }
    LOGE("nativeRunWineboot: timeout after %d ms, killing pid=%d", timeoutMs, (int)pid);
    proc.killHard();
    return -2;
}

// --- Installer wine subprocess --------------------------------------------
// VstInstallerScreen drives these from Kotlin. Spawn wine against a clone
// of the base wineprefix; the user interacts with the installer wizard
// via the embedded X11 SurfaceView. After installer exits, the discovery
// phase walks the prefix for new VST DLLs.

JNIEXPORT jint JNICALL
Java_com_varcain_vsthost_NativeBridge_nativeStartInstaller(
    JNIEnv* env, jobject /*thiz*/,
    jstring jExePath, jstring jPrefixPath, jint displayNumber,
    jstring jWineBinary, jstring jWineserverBinary, jstring jWineDllPath,
    jstring jNativeLibDir, jstring jCacheDir) {
    std::lock_guard<std::mutex> lock(installer_mutex);
    if (g_installer && g_installer->isRunning()) {
        LOGE("nativeStartInstaller: an installer is already running (pid=%d)",
             (int)g_installer->pid());
        return -1;
    }
    auto jstr = [&](jstring s) -> std::string {
        if (!s) return {};
        const char* c = env->GetStringUTFChars(s, nullptr);
        if (!c) return {};
        std::string r(c);
        env->ReleaseStringUTFChars(s, c);
        return r;
    };
    // vstpoc: a .msi is not a PE — `wine foo.msi` can't exec it. Route MSI
    // packages through `msiexec /i` (the way an MSI is meant to be installed).
    // This also lets us bypass broken EXE bootstrappers (e.g. Advanced
    // Installer's opaque "Function failed") by running the extracted MSI
    // directly. The package path must be a DOS path for msiexec's parser —
    // wine maps the unix root to Z:\ (the installer exes already load as
    // Z:\data\user\0\...), so prefix Z: and flip the slashes.
    std::string exePath = jstr(jExePath);
    std::string primaryExe = exePath;
    std::vector<std::string> extraArgs;
    {
        std::string ext = exePath.size() >= 4 ? exePath.substr(exePath.size() - 4) : "";
        for (char& c : ext) c = static_cast<char>(::tolower((unsigned char)c));
        if (ext == ".msi") {
            std::string dos = "Z:" + exePath;
            for (char& c : dos) if (c == '/') c = '\\';
            // Verbose MSI log (/l*vx) → <cache>/msi_install.log, so an aborted
            // install sequence (LaunchCondition, custom action, etc.) is fully
            // diagnosable — `msiexec /i` alone gives no reason on the wine log.
            std::string logDos = "Z:" + jstr(jCacheDir) + "/msi_install.log";
            for (char& c : logDos) if (c == '/') c = '\\';
            primaryExe = "msiexec";
            // Full UI: the wizard renders correctly once the installer framebuffer
            // is large enough (see INSTALLER_SCREEN_W/H) — the earlier "black/
            // invisible buttons" was GDI-surface clipping at 640x480, not a theme
            // bug (the silent /q levels are ignored by Advanced Installer's custom
            // UI anyway). AI_BOOTSTRAPPER=1 is the property its EXE normally passes.
            extraArgs  = { "/i", dos, "AI_BOOTSTRAPPER=1", "/l*vx", logDos };
            LOGI("nativeStartInstaller: .msi → msiexec /i %s (log %s)", dos.c_str(), logDos.c_str());
        }
        else {
            /* Optional extra command-line args for direct .exe launches, one
             * per line in <cache>/exe_args.txt — same no-rebuild spirit as
             * wine_env.txt. Needed e.g. for Electron-based vendor managers
             * (IK Product Manager) where '--disable-gpu' forces Chromium's
             * software compositor when the GPU present path yields nothing.
             * '#' lines skipped; absent file = no-op. */
            std::ifstream af(jstr(jCacheDir) + "/exe_args.txt");
            std::string line;
            while (std::getline(af, line)) {
                size_t b = line.find_first_not_of(" \t\r\n");
                if (b == std::string::npos || line[b] == '#') continue;
                size_t e = line.find_last_not_of(" \t\r\n");
                std::string arg = line.substr(b, e - b + 1);
                if (!arg.empty()) {
                    extraArgs.push_back(arg);
                    LOGI("nativeStartInstaller: extra exe arg: %s", arg.c_str());
                }
            }
        }
    }
    WineHostProcess::Config cfg{
        .nativeLibDir       = jstr(jNativeLibDir),
        .cacheDir           = jstr(jCacheDir),
        .wineBinary         = jstr(jWineBinary),
        .wineserverBinary   = jstr(jWineserverBinary),
        .wineDllPath        = jstr(jWineDllPath),
        .winePrefix         = jstr(jPrefixPath),
        .primaryExe         = primaryExe,
        .shmPath            = {},     // no IPC with the installer
        .pickerShmPath      = {},     // wine's builtin FileOpenDialog is fine
        .pluginPaths        = {},
        .extraArgs          = extraArgs,
        .displayNumber      = static_cast<int>(displayNumber),
        .logSuffix          = "installer",  // → cache/vst_host_installer.log
    };
    auto proc = std::make_unique<WineHostProcess>(std::move(cfg));
    if (!proc->start()) {
        LOGE("nativeStartInstaller: WineHostProcess::start failed");
        return -1;
    }
    pid_t pid = proc->pid();
    g_installer = std::move(proc);
    LOGI("nativeStartInstaller: launched pid=%d", (int)pid);
    return static_cast<jint>(pid);
}

// Non-blocking wait. Returns:
//   -1  no installer registered / pid mismatch / waitpid reports ECHILD
//   -2  still running, caller should retry
//   >=0 installer exited with this code (or 128+signo if killed)
JNIEXPORT jint JNICALL
Java_com_varcain_vsthost_NativeBridge_nativeWaitInstaller(
    JNIEnv* /*env*/, jobject /*thiz*/, jint pid) {
    std::lock_guard<std::mutex> lock(installer_mutex);
    if (!g_installer || g_installer->pid() != pid) return -1;
    int status = 0;
    pid_t r = ::waitpid(g_installer->pid(), &status, WNOHANG);
    if (r == 0) return -2;
    if (r < 0) {
        if (errno == ECHILD) {
            g_installer.reset();
            return -1;
        }
        return -2;
    }
    int exitCode = WIFEXITED(status) ? WEXITSTATUS(status)
                 : WIFSIGNALED(status) ? (128 + WTERMSIG(status))
                 : -1;
    LOGI("nativeWaitInstaller: pid=%d exited code=%d (status=0x%x)",
         pid, exitCode, status);
    g_installer.reset();
    return static_cast<jint>(exitCode);
}

JNIEXPORT void JNICALL
Java_com_varcain_vsthost_NativeBridge_nativeKillInstaller(
    JNIEnv* /*env*/, jobject /*thiz*/, jint pid) {
    std::lock_guard<std::mutex> lock(installer_mutex);
    if (!g_installer || g_installer->pid() != pid) return;
    LOGW("nativeKillInstaller: terminating pid=%d", pid);
    g_installer->killHard();
    g_installer.reset();
}

// Read the health/diagnostics fields out of a plugin's VstpocShared mmap
// file WITHOUT going through the live SharedRing (which is owned by the
// :app-side WineVstPlugin and not reachable from here). The shm file at
// <filesDir>/tmp/vst_shm_v<uuid>.dat persists after the wine subprocess
// exits, so this works both live (running plugin) and post-mortem
// (crashed/closed plugin) — the debugging-infra use case.
//
// Returns a long[] of the health fields in a fixed order, or null if the
// file can't be read. The Kotlin side (NativeBridge.getPluginHealth)
// unpacks it into a PluginHealth data class. Returning long[] avoids
// having to construct a Java object across JNI for what is just a bag of
// scalars.
//
// Index layout (keep in sync with NativeBridge.PluginHealth):
//   [0] diagnostic_layout_v   (0 = legacy guest, fields below meaningless)
//   [1] dxvk_init_status
//   [2] d3d11_device_status
//   [3] render_api_used
//   [4] last_memory_alloc_failed_size
//   [5] last_memory_alloc_failed_types
//   [6] last_memory_alloc_failed_count
//   [7] paint_request_count
//   [8] wm_paint_count
//   [9] veh_patterns_hit_bitmask
//   [10] wm_user_storm_per_second
//   [11] load_status     (existing field, handy alongside)
//   [12] guest_ready     (existing field)
JNIEXPORT jlongArray JNICALL
Java_com_varcain_vsthost_NativeBridge_nativeReadPluginHealth(
    JNIEnv* env, jobject /*thiz*/, jstring jShmPath) {
    if (!jShmPath) return nullptr;
    const char* c = env->GetStringUTFChars(jShmPath, nullptr);
    if (!c) return nullptr;
    std::string path(c);
    env->ReleaseStringUTFChars(jShmPath, c);

    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        // Not an error worth logging loudly — a never-activated plugin
        // simply has no shm file yet.
        return nullptr;
    }
    // mmap read-only. The struct is large (audio rings); we only need the
    // trailing health fields, but mapping the whole thing is simplest and
    // the kernel only faults in the pages we touch.
    void* p = ::mmap(nullptr, sizeof(VstpocShared), PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) {
        LOGW("nativeReadPluginHealth: mmap(%s) failed: %s",
             path.c_str(), std::strerror(errno));
        return nullptr;
    }
    const VstpocShared* s = static_cast<const VstpocShared*>(p);

    jlong vals[13];
    vals[0]  = static_cast<jlong>(s->diagnostic_layout_v);
    vals[1]  = static_cast<jlong>(s->dxvk_init_status);
    vals[2]  = static_cast<jlong>(s->d3d11_device_status);
    vals[3]  = static_cast<jlong>(s->render_api_used);
    vals[4]  = static_cast<jlong>(s->last_memory_alloc_failed_size);
    vals[5]  = static_cast<jlong>(s->last_memory_alloc_failed_types);
    vals[6]  = static_cast<jlong>(s->last_memory_alloc_failed_count);
    vals[7]  = static_cast<jlong>(s->paint_request_count);
    vals[8]  = static_cast<jlong>(s->wm_paint_count);
    vals[9]  = static_cast<jlong>(s->veh_patterns_hit_bitmask);
    vals[10] = static_cast<jlong>(s->wm_user_storm_per_second);
    vals[11] = static_cast<jlong>(s->load_status);
    vals[12] = static_cast<jlong>(s->guest_ready);

    ::munmap(p, sizeof(VstpocShared));

    jlongArray arr = env->NewLongArray(13);
    if (!arr) return nullptr;
    env->SetLongArrayRegion(arr, 0, 13, vals);
    return arr;
}

}  // extern "C"
