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

#include <jni.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
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
    WineHostProcess::Config cfg{
        .nativeLibDir       = jstr(jNativeLibDir),
        .cacheDir           = jstr(jCacheDir),
        .wineBinary         = jstr(jWineBinary),
        .wineserverBinary   = jstr(jWineserverBinary),
        .wineDllPath        = jstr(jWineDllPath),
        .winePrefix         = jstr(jPrefixPath),
        .primaryExe         = jstr(jExePath),
        .shmPath            = {},     // no IPC with the installer
        .pickerShmPath      = {},     // wine's builtin FileOpenDialog is fine
        .pluginPaths        = {},
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

}  // extern "C"
