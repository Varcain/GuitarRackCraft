/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of Guitar RackCraft.
 *
 * Guitar RackCraft is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Guitar RackCraft is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Guitar RackCraft. If not, see <https://www.gnu.org/licenses/>.
 */

#include <jni.h>
#include <android/log.h>
#include <atomic>
#include <csignal>
#include <cstring>
#include <signal.h>
#include <dlfcn.h>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <pthread.h>
#include <unistd.h>
#include <vector>

#include "../engine/AudioEngine.h"
#include "../plugin/PluginUIGuard.h"
#include "../engine/OfflineProcessor.h"
#include "../plugin/PluginRegistry.h"
#include "../plugin/IPlugin.h"
#include "../plugin/IPluginFactory.h"
#include "../plugin/lv2/LV2PluginFactory.h"
#include "../plugin/StateSerializer.h"
#include "../plugin/PluginUIManager.h"
#include "../x11/X11NativeDisplay.h"
#include "../x11/X11Worker.h"
#include "../x11/DisplayState.h"
#include "../utils/ThreadUtils.h"

#define LOG_TAG "NativeBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using namespace guitarrackcraft;

struct NativeContext {
    std::unique_ptr<AudioEngine> audioEngine;
    std::unique_ptr<PluginRegistry> pluginRegistry;
    std::unique_ptr<OfflineProcessor> offlineProcessor;
    std::unique_ptr<PluginUIManager> pluginUIManager;
    std::string lv2Path;
    std::string nativeLibDir;
    std::string filesDir;
    std::string x11LibsDir;
    std::string pluginLibDir;  // PAD-extracted plugin .so (playstore flavor)
};

static NativeContext* g_ctx = nullptr;

static NativeContext* ensureCtx() {
    if (!g_ctx) g_ctx = new NativeContext();
    return g_ctx;
}

// Cached JNI class/method references (initialized lazily)
static struct {
    jclass pluginInfoClass = nullptr;
    jmethodID pluginInfoCtor = nullptr;
    jfieldID piId = nullptr;
    jfieldID piName = nullptr;
    jfieldID piFormat = nullptr;
    jfieldID piPorts = nullptr;
    jfieldID piModguiBasePath = nullptr;
    jfieldID piModguiIconTemplate = nullptr;
    jfieldID piHasX11Ui = nullptr;
    jfieldID piX11UiBinaryPath = nullptr;
    jfieldID piX11UiUri = nullptr;

    jclass portInfoClass = nullptr;
    jmethodID portInfoCtor = nullptr;

    jclass scalePointClass = nullptr;
    jmethodID scalePointCtor = nullptr;

    jclass arrayListClass = nullptr;
    jmethodID arrayListCtor = nullptr;
    jmethodID arrayListAdd = nullptr;
} g_jni;

static bool ensureJniCache(JNIEnv* env) {
    if (g_jni.pluginInfoClass) return true;

    auto cache = [&](const char* name) -> jclass {
        jclass local = env->FindClass(name);
        return local ? (jclass)env->NewGlobalRef(local) : nullptr;
    };

    g_jni.arrayListClass = cache("java/util/ArrayList");
    if (!g_jni.arrayListClass) return false;
    g_jni.arrayListCtor = env->GetMethodID(g_jni.arrayListClass, "<init>", "(I)V");
    g_jni.arrayListAdd = env->GetMethodID(g_jni.arrayListClass, "add", "(Ljava/lang/Object;)Z");

    g_jni.scalePointClass = cache("com/varcain/guitarrackcraft/engine/ScalePoint");
    if (g_jni.scalePointClass)
        g_jni.scalePointCtor = env->GetMethodID(g_jni.scalePointClass, "<init>", "(Ljava/lang/String;F)V");

    g_jni.portInfoClass = cache("com/varcain/guitarrackcraft/engine/PortInfo");
    if (!g_jni.portInfoClass) return false;
    g_jni.portInfoCtor = env->GetMethodID(g_jni.portInfoClass, "<init>",
        "(ILjava/lang/String;Ljava/lang/String;ZZZZFFFLjava/util/List;)V");

    g_jni.pluginInfoClass = cache("com/varcain/guitarrackcraft/engine/PluginInfo");
    if (!g_jni.pluginInfoClass) return false;
    g_jni.pluginInfoCtor = env->GetMethodID(g_jni.pluginInfoClass, "<init>", "()V");
    g_jni.piId = env->GetFieldID(g_jni.pluginInfoClass, "id", "Ljava/lang/String;");
    g_jni.piName = env->GetFieldID(g_jni.pluginInfoClass, "name", "Ljava/lang/String;");
    g_jni.piFormat = env->GetFieldID(g_jni.pluginInfoClass, "format", "Ljava/lang/String;");
    g_jni.piPorts = env->GetFieldID(g_jni.pluginInfoClass, "ports", "Ljava/util/List;");
    g_jni.piModguiBasePath = env->GetFieldID(g_jni.pluginInfoClass, "modguiBasePath", "Ljava/lang/String;");
    g_jni.piModguiIconTemplate = env->GetFieldID(g_jni.pluginInfoClass, "modguiIconTemplate", "Ljava/lang/String;");
    g_jni.piHasX11Ui = env->GetFieldID(g_jni.pluginInfoClass, "hasX11Ui", "Z");
    g_jni.piX11UiBinaryPath = env->GetFieldID(g_jni.pluginInfoClass, "x11UiBinaryPath", "Ljava/lang/String;");
    g_jni.piX11UiUri = env->GetFieldID(g_jni.pluginInfoClass, "x11UiUri", "Ljava/lang/String;");

    return true;
}

namespace guitarrackcraft {

static DisplayState::Phase getDisplayPhase(int displayNumber) {
    std::lock_guard<std::mutex> lock(displayStateMutex());
    auto it = displayStates().find(displayNumber);
    if (it != displayStates().end()) {
        return it->second.phase;
    }
    return DisplayState::Phase::None;
}

// Update display phase
static void setDisplayPhase(int displayNumber, DisplayState::Phase phase) {
    std::lock_guard<std::mutex> lock(displayStateMutex());
    displayStates()[displayNumber].phase = phase;
    LOGI("Display %d phase -> %d", displayNumber, static_cast<int>(phase));
}

}  // namespace guitarrackcraft

// Minimal SIGABRT handler: log to stderr (async-signal-safe) then re-raise so tombstone is still generated.
static void sigabrt_handler(int signum) {
    (void)signum;
    const char msg[] = "GuitarRackCraft: SIGABRT received (check tombstone for backtrace)\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
}

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)vm;
    (void)reserved;
    struct sigaction sa = {};
    sa.sa_handler = sigabrt_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGABRT, &sa, nullptr);
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeSetLv2Path(JNIEnv* env, jobject thiz, jstring path) {
    if (!path) {
        ensureCtx()->lv2Path.clear();
        return;
    }
    const char* pathStr = env->GetStringUTFChars(path, nullptr);
    if (pathStr) {
        ensureCtx()->lv2Path = pathStr;
        env->ReleaseStringUTFChars(path, pathStr);
        LOGI("LV2 path set: %s", g_ctx->lv2Path.c_str());
    }
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeSetNativeLibDir(JNIEnv* env, jobject thiz, jstring path) {
    if (!path) {
        ensureCtx()->nativeLibDir.clear();
        return;
    }
    const char* pathStr = env->GetStringUTFChars(path, nullptr);
    if (pathStr) {
        ensureCtx()->nativeLibDir = pathStr;
        env->ReleaseStringUTFChars(path, pathStr);
        LOGI("Native lib dir set: %s", g_ctx->nativeLibDir.c_str());
    }
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeSetFilesDir(JNIEnv* env, jobject thiz, jstring path) {
    if (!path) {
        ensureCtx()->filesDir.clear();
        return;
    }
    const char* pathStr = env->GetStringUTFChars(path, nullptr);
    if (pathStr) {
        ensureCtx()->filesDir = pathStr;
        env->ReleaseStringUTFChars(path, pathStr);
        LOGI("Files dir set: %s", g_ctx->filesDir.c_str());
    }
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeSetX11LibsDir(JNIEnv* env, jobject thiz, jstring path) {
    if (!path) {
        ensureCtx()->x11LibsDir.clear();
        return;
    }
    const char* pathStr = env->GetStringUTFChars(path, nullptr);
    if (pathStr) {
        ensureCtx()->x11LibsDir = pathStr;
        env->ReleaseStringUTFChars(path, pathStr);
        LOGI("X11 scratch dir set: %s", g_ctx->x11LibsDir.c_str());
        /* Preload X11 libs from nativeLibDir with RTLD_GLOBAL so plugin UI .so
         * can resolve DT_NEEDED (libxcb.so, libX11.so, etc.).
         * build.sh renames versioned files (libxcb.so.1 -> libxcb.so) so they're
         * extracted to nativeLibDir. Try unversioned first, then versioned as fallback. */
        std::string dir = g_ctx->nativeLibDir;
        if (!dir.empty() && dir.back() != '/') dir += '/';
        const char* libs[][2] = {
            {"libXau.so",  "libXau.so.6"},
            {"libxcb.so",  "libxcb.so.1"},
            {"libX11.so",  "libX11.so.6"},
            {nullptr, nullptr}
        };
        for (int i = 0; libs[i][0]; ++i) {
            std::string full = dir + libs[i][0];
            void* h = dlopen(full.c_str(), RTLD_NOW | RTLD_GLOBAL);
            if (!h) {
                full = dir + libs[i][1];
                h = dlopen(full.c_str(), RTLD_NOW | RTLD_GLOBAL);
            }
            LOGI("X11 preload %s from nativeLibDir: %s", libs[i][0], h ? "ok" : "skip");
        }
    }
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeSetPluginLibDir(JNIEnv* env, jobject thiz, jstring path) {
    if (!path) {
        ensureCtx()->pluginLibDir.clear();
        return;
    }
    const char* pathStr = env->GetStringUTFChars(path, nullptr);
    if (pathStr) {
        ensureCtx()->pluginLibDir = pathStr;
        env->ReleaseStringUTFChars(path, pathStr);
        LOGI("Plugin lib dir set: %s", g_ctx->pluginLibDir.c_str());
    }
}

JNIEXPORT jboolean JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeInit(JNIEnv* env, jobject thiz) {
    LOGI("Initializing native engine");

    // Promote libc++_shared.so to RTLD_GLOBAL so that LV2 plugin .so files
    // (which depend on it) can resolve the dependency when dlopen'd by lilv
    void* cxxLib = dlopen("libc++_shared.so", RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD);
    if (!cxxLib) {
        cxxLib = dlopen("libc++_shared.so", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!cxxLib) {
        LOGE("Warning: could not promote libc++_shared.so to global: %s", dlerror());
    }

    // NOTE: Do NOT call XInitThreads(). All Display* access is single-threaded via
    // the pluginUI thread in X11NativeDisplay. XInitThreads enables XCB sequence
    // tracking assertions that have known bugs in the Xlib-XCB bridge.

    // Create plugin registry
    ensureCtx();
    g_ctx->pluginRegistry = std::make_unique<PluginRegistry>();

    // Register LV2 factory (pass path from nativeSetLv2Path for extracted Guitarix/assets)
    auto lv2Factory = std::make_unique<LV2PluginFactory>(g_ctx->lv2Path, g_ctx->nativeLibDir, g_ctx->filesDir, g_ctx->pluginLibDir);
    g_ctx->pluginRegistry->registerFactory(std::move(lv2Factory));

    // Initialize all factories
    if (!g_ctx->pluginRegistry->initializeAll()) {
        LOGE("Failed to initialize plugin factories");
        return JNI_FALSE;
    }

    // Create audio engine
    g_ctx->audioEngine = std::make_unique<AudioEngine>();

    // Create plugin UI manager
    g_ctx->pluginUIManager = std::make_unique<guitarrackcraft::PluginUIManager>();
    g_ctx->pluginUIManager->setChain(&g_ctx->audioEngine->getChain());

    // Create offline processor (uses same chain as audio engine)
    g_ctx->offlineProcessor = std::make_unique<OfflineProcessor>(g_ctx->audioEngine->getChain());

    // Start the X11Worker thread for single-threaded X11 operations
    // This prevents xcb_xlib_threads_sequence_lost crashes by ensuring
    // all X11 calls (plugin instantiate, idle, cleanup) happen on one thread
    getX11Worker().start();
    LOGI("X11Worker started for single-threaded X11 operations");

    LOGI("Native engine initialized");
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeStartEngine(JNIEnv* env, jobject thiz, jfloat sampleRate, jint inputDeviceId, jint outputDeviceId, jint bufferFrames) {
    if (!g_ctx->audioEngine) {
        LOGE("Audio engine not initialized");
        return JNI_FALSE;
    }

    return g_ctx->audioEngine->start(static_cast<float>(sampleRate),
                                     static_cast<int32_t>(inputDeviceId),
                                     static_cast<int32_t>(outputDeviceId),
                                     static_cast<int32_t>(bufferFrames)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeStopEngine(JNIEnv* env, jobject thiz) {
    LOGI("nativeStopEngine CALLED tid=%ld (Java requested stop; will call closeStreams from this thread)", getTid());
    if (g_ctx->audioEngine) {
        g_ctx->audioEngine->stop();
    }
    LOGI("nativeStopEngine RETURNED tid=%ld", getTid());
}

JNIEXPORT jboolean JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeIsEngineRunning(JNIEnv* env, jobject thiz) {
    return g_ctx->audioEngine && g_ctx->audioEngine->isRunning() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloat JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeGetSampleRate(JNIEnv* env, jobject thiz) {
    if (!g_ctx->audioEngine) {
        return 0.0f;
    }
    return g_ctx->audioEngine->getSampleRate();
}

JNIEXPORT jint JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeGetBufferFrameCount(JNIEnv* env, jobject thiz) {
    if (!g_ctx->audioEngine) {
        return 0;
    }
    return static_cast<jint>(g_ctx->audioEngine->getCallbackFrameCount());
}

JNIEXPORT jintArray JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeGetStreamInfo(JNIEnv* env, jobject thiz) {
    // Returns [isAAudio, inputExclusive, outputExclusive, inputLowLatency, outputLowLatency, outputMMap, outputCallback, framesPerBurst]
    jint arr[8] = {};
    if (g_ctx->audioEngine && g_ctx->audioEngine->isRunning()) {
        auto info = g_ctx->audioEngine->getStreamInfo();
        arr[0] = info.isAAudio ? 1 : 0;
        arr[1] = info.inputExclusive ? 1 : 0;
        arr[2] = info.outputExclusive ? 1 : 0;
        arr[3] = info.inputLowLatency ? 1 : 0;
        arr[4] = info.outputLowLatency ? 1 : 0;
        arr[5] = info.outputMMap ? 1 : 0;
        arr[6] = info.outputCallback ? 1 : 0;
        arr[7] = info.framesPerBurst;
    }
    jintArray result = env->NewIntArray(8);
    if (result) {
        env->SetIntArrayRegion(result, 0, 8, arr);
    }
    return result;
}

JNIEXPORT jdouble JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeGetLatencyMs(JNIEnv* env, jobject thiz) {
    if (!g_ctx->audioEngine) {
        return 0.0;
    }
    return g_ctx->audioEngine->getLatencyMs();
}

JNIEXPORT jfloat JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeGetInputLevel(JNIEnv* env, jobject thiz) {
    if (!g_ctx->audioEngine) {
        return 0.0f;
    }
    return g_ctx->audioEngine->getInputLevel();
}

JNIEXPORT jfloat JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeGetOutputLevel(JNIEnv* env, jobject thiz) {
    if (!g_ctx->audioEngine) {
        return 0.0f;
    }
    return g_ctx->audioEngine->getOutputLevel();
}

JNIEXPORT jfloat JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeGetCpuLoad(JNIEnv* env, jobject thiz) {
    if (!g_ctx->audioEngine) {
        return 0.0f;
    }
    return g_ctx->audioEngine->getCpuLoad();
}

JNIEXPORT jint JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeGetXRunCount(JNIEnv* env, jobject thiz) {
    if (!g_ctx->audioEngine) {
        return 0;
    }
    return g_ctx->audioEngine->getXRunCount();
}

JNIEXPORT jboolean JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeIsInputClipping(JNIEnv* env, jobject thiz) {
    if (!g_ctx->audioEngine) {
        return JNI_FALSE;
    }
    return g_ctx->audioEngine->isInputClipping() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeIsOutputClipping(JNIEnv* env, jobject thiz) {
    if (!g_ctx->audioEngine) {
        return JNI_FALSE;
    }
    return g_ctx->audioEngine->isOutputClipping() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeResetClipping(JNIEnv* env, jobject thiz) {
    if (g_ctx->audioEngine) {
        g_ctx->audioEngine->resetClipping();
    }
}

// Helper function to create PortInfo object (uses cached JNI refs)
jobject createPortInfoObject(JNIEnv* env, const PortInfo& port) {
    if (!ensureJniCache(env)) return nullptr;

    // Build scale points list
    jobject scalePointsList = env->NewObject(g_jni.arrayListClass, g_jni.arrayListCtor,
        static_cast<jint>(port.scalePoints.size()));
    if (scalePointsList && !port.scalePoints.empty() && g_jni.scalePointClass && g_jni.scalePointCtor) {
        for (const auto& sp : port.scalePoints) {
            jstring labelStr = env->NewStringUTF(sp.label.c_str());
            jobject scalePointObj = env->NewObject(g_jni.scalePointClass, g_jni.scalePointCtor,
                labelStr, static_cast<jfloat>(sp.value));
            env->DeleteLocalRef(labelStr);
            if (scalePointObj) {
                env->CallBooleanMethod(scalePointsList, g_jni.arrayListAdd, scalePointObj);
                env->DeleteLocalRef(scalePointObj);
            }
        }
    }

    jstring nameStr = env->NewStringUTF(port.name.c_str());
    jstring symbolStr = env->NewStringUTF(port.symbol.c_str());

    jobject portObj = env->NewObject(g_jni.portInfoClass, g_jni.portInfoCtor,
        static_cast<jint>(port.index),
        nameStr,
        symbolStr,
        static_cast<jboolean>(port.isInput ? JNI_TRUE : JNI_FALSE),
        static_cast<jboolean>(port.isAudio ? JNI_TRUE : JNI_FALSE),
        static_cast<jboolean>(port.isControl ? JNI_TRUE : JNI_FALSE),
        static_cast<jboolean>(port.isToggle ? JNI_TRUE : JNI_FALSE),
        static_cast<jfloat>(port.defaultValue),
        static_cast<jfloat>(port.minValue),
        static_cast<jfloat>(port.maxValue),
        scalePointsList
    );

    env->DeleteLocalRef(nameStr);
    env->DeleteLocalRef(symbolStr);
    if (scalePointsList) env->DeleteLocalRef(scalePointsList);

    return portObj;
}

// Helper function to convert PluginInfo to Java object (uses cached JNI refs)
jobject createPluginInfoObject(JNIEnv* env, const PluginInfo& info) {
    if (!ensureJniCache(env)) return nullptr;

    jobject obj = env->NewObject(g_jni.pluginInfoClass, g_jni.pluginInfoCtor);
    if (!obj) return nullptr;

    auto setString = [&](jfieldID field, const std::string& val) {
        if (!field || val.empty()) return;
        jstring s = env->NewStringUTF(val.c_str());
        env->SetObjectField(obj, field, s);
        env->DeleteLocalRef(s);
    };

    setString(g_jni.piId, info.id);
    setString(g_jni.piName, info.name);
    setString(g_jni.piFormat, info.format);
    setString(g_jni.piModguiBasePath, info.modguiBasePath);
    setString(g_jni.piModguiIconTemplate, info.modguiIconTemplate);
    setString(g_jni.piX11UiBinaryPath, info.x11UiBinaryPath);
    setString(g_jni.piX11UiUri, info.x11UiUri);

    if (g_jni.piHasX11Ui)
        env->SetBooleanField(obj, g_jni.piHasX11Ui, info.hasX11Ui ? JNI_TRUE : JNI_FALSE);

    // Create ports list
    if (g_jni.piPorts && !info.ports.empty()) {
        jobject portsList = env->NewObject(g_jni.arrayListClass, g_jni.arrayListCtor,
            static_cast<jint>(info.ports.size()));
        for (const auto& port : info.ports) {
            jobject portObj = createPortInfoObject(env, port);
            if (portObj) {
                env->CallBooleanMethod(portsList, g_jni.arrayListAdd, portObj);
                env->DeleteLocalRef(portObj);
            }
        }
        env->SetObjectField(obj, g_jni.piPorts, portsList);
        env->DeleteLocalRef(portsList);
    }

    return obj;
}

JNIEXPORT jobjectArray JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeGetAvailablePlugins(JNIEnv* env, jobject thiz) {
    if (!ensureJniCache(env)) return nullptr;

    std::vector<PluginInfo> plugins;
    if (g_ctx && g_ctx->pluginRegistry) {
        plugins = g_ctx->pluginRegistry->getAllPlugins();
    }

    jobjectArray result = env->NewObjectArray(static_cast<jsize>(plugins.size()), g_jni.pluginInfoClass, nullptr);
    if (!result) {
        return nullptr;
    }

    for (size_t i = 0; i < plugins.size(); ++i) {
        jobject pluginObj = createPluginInfoObject(env, plugins[i]);
        if (pluginObj) {
            env->SetObjectArrayElement(result, static_cast<jsize>(i), pluginObj);
            env->DeleteLocalRef(pluginObj);
        }
    }

    return result;
}

JNIEXPORT jint JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeAddPluginToRack(JNIEnv* env, jobject thiz, jstring pluginId, jint position) {
    if (!g_ctx || !g_ctx->pluginRegistry || !g_ctx->audioEngine) {
        return -1;
    }

    const char* idStr = env->GetStringUTFChars(pluginId, nullptr);
    if (!idStr) {
        return -1;
    }

    std::string fullId = std::string(idStr);
    env->ReleaseStringUTFChars(pluginId, idStr);

    auto plugin = g_ctx->pluginRegistry->createPlugin(fullId);
    if (!plugin) {
        LOGE("Failed to create plugin: %s", fullId.c_str());
        return -1;
    }

    int pos = g_ctx->audioEngine->getChain().addPlugin(std::move(plugin), position);
    if (pos >= 0) {
        LOGI("nativeAddPluginToRack: pluginId=%s position=%d -> index=%d",
             fullId.c_str(), position, pos);
    }
    return pos;
}

JNIEXPORT jboolean JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeRemovePluginFromRack(JNIEnv* env, jobject thiz, jint position) {
    if (!g_ctx->audioEngine) {
        return JNI_FALSE;
    }

    // Detach the UI whose captured chain index matches the position being removed,
    // then shift all higher captured indices down by one so they stay in sync
    // with the chain's erase.  This prevents stale indexPtrs from causing
    // subsequent detachPlugin calls to target the wrong UI entry.
    if (g_ctx->pluginUIManager) {
        g_ctx->pluginUIManager->detachAndShiftForRemoval(position);
    }

    return g_ctx->audioEngine->getChain().removePlugin(position) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeReorderRack(JNIEnv* env, jobject thiz, jint fromPos, jint toPos) {
    if (!g_ctx->audioEngine) {
        return JNI_FALSE;
    }

    if (g_ctx->pluginUIManager) {
        g_ctx->pluginUIManager->pauseAllUIs();
    }
    bool ok = g_ctx->audioEngine->getChain().reorderPlugins(fromPos, toPos);
    if (g_ctx->pluginUIManager && ok) {
        g_ctx->pluginUIManager->reorderUIs(fromPos, toPos);
    }
    if (g_ctx->pluginUIManager) {
        g_ctx->pluginUIManager->resumeAllUIs();
    }
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeSetPluginFilePath(JNIEnv* env, jobject thiz, jint pluginIndex, jstring propertyUri, jstring filePath) {
    if (!g_ctx || !g_ctx->audioEngine) {
        return;
    }

    const char* propStr = env->GetStringUTFChars(propertyUri, nullptr);
    const char* pathStr = env->GetStringUTFChars(filePath, nullptr);
    if (propStr && pathStr) {
        g_ctx->audioEngine->getChain().setPluginFilePath(
            pluginIndex, std::string(propStr), std::string(pathStr));
    }
    if (propStr) env->ReleaseStringUTFChars(propertyUri, propStr);
    if (pathStr) env->ReleaseStringUTFChars(filePath, pathStr);
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeSetParameter(JNIEnv* env, jobject thiz, jint pluginIndex, jint portIndex, jfloat value) {
    if (!g_ctx->audioEngine) {
        return;
    }

    g_ctx->audioEngine->getChain().setParameter(pluginIndex, static_cast<uint32_t>(portIndex), static_cast<float>(value));
    if (g_ctx->pluginUIManager) {
        g_ctx->pluginUIManager->notifyUIParameterChange(pluginIndex, static_cast<uint32_t>(portIndex), static_cast<float>(value));
    }
}

JNIEXPORT jfloat JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeGetParameter(JNIEnv* env, jobject thiz, jint pluginIndex, jint portIndex) {
    if (!g_ctx->audioEngine) {
        return 0.0f;
    }

    return g_ctx->audioEngine->getChain().getParameter(pluginIndex, static_cast<uint32_t>(portIndex));
}

// --- WAV real-time playback ---

JNIEXPORT jboolean JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeLoadWav(JNIEnv* env, jobject thiz, jstring path) {
    if (!g_ctx->audioEngine) {
        return JNI_FALSE;
    }
    if (!path) {
        return JNI_FALSE;
    }
    const char* pathStr = env->GetStringUTFChars(path, nullptr);
    if (!pathStr) {
        return JNI_FALSE;
    }
    bool result = g_ctx->audioEngine->loadWav(std::string(pathStr));
    env->ReleaseStringUTFChars(path, pathStr);
    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeUnloadWav(JNIEnv* env, jobject thiz) {
    if (g_ctx->audioEngine) {
        g_ctx->audioEngine->unloadWav();
    }
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeWavPlay(JNIEnv* env, jobject thiz) {
    if (g_ctx->audioEngine) {
        g_ctx->audioEngine->wavPlay();
    }
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeWavPause(JNIEnv* env, jobject thiz) {
    if (g_ctx->audioEngine) {
        g_ctx->audioEngine->wavPause();
    }
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeWavSeek(JNIEnv* env, jobject thiz, jdouble positionSec) {
    if (!g_ctx->audioEngine) {
        return;
    }
    double sr = static_cast<double>(g_ctx->audioEngine->getSampleRate());
    if (sr <= 0.0) return;
    size_t frame = static_cast<size_t>(positionSec * sr + 0.5);
    g_ctx->audioEngine->wavSeekToFrame(frame);
}

JNIEXPORT jdouble JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeGetWavDurationSec(JNIEnv* env, jobject thiz) {
    if (!g_ctx->audioEngine) {
        return 0.0;
    }
    return g_ctx->audioEngine->getWavDurationSec();
}

JNIEXPORT jdouble JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeGetWavPositionSec(JNIEnv* env, jobject thiz) {
    if (!g_ctx->audioEngine) {
        return 0.0;
    }
    return g_ctx->audioEngine->getWavPositionSec();
}

JNIEXPORT jboolean JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeIsWavPlaying(JNIEnv* env, jobject thiz) {
    return g_ctx->audioEngine && g_ctx->audioEngine->isWavPlaying() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeIsWavLoaded(JNIEnv* env, jobject thiz) {
    return g_ctx->audioEngine && g_ctx->audioEngine->isWavLoaded() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeProcessFile(JNIEnv* env, jobject thiz, jstring inputPath, jstring outputPath) {
    if (!g_ctx || !g_ctx->offlineProcessor) {
        return JNI_FALSE;
    }

    const char* inputStr = env->GetStringUTFChars(inputPath, nullptr);
    const char* outputStr = env->GetStringUTFChars(outputPath, nullptr);
    
    if (!inputStr || !outputStr) {
        if (inputStr) env->ReleaseStringUTFChars(inputPath, inputStr);
        if (outputStr) env->ReleaseStringUTFChars(outputPath, outputStr);
        return JNI_FALSE;
    }

    bool result = g_ctx->offlineProcessor->processFile(std::string(inputStr), std::string(outputStr));

    env->ReleaseStringUTFChars(inputPath, inputStr);
    env->ReleaseStringUTFChars(outputPath, outputStr);

    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeGetRackSize(JNIEnv* env, jobject thiz) {
    if (!g_ctx->audioEngine) {
        return 0;
    }
    return static_cast<jint>(g_ctx->audioEngine->getChain().getSize());
}

JNIEXPORT jobject JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeGetRackPluginInfo(JNIEnv* env, jobject thiz, jint index) {
    if (!g_ctx->audioEngine) {
        return nullptr;
    }

    IPlugin* plugin = g_ctx->audioEngine->getChain().getPlugin(index);
    if (!plugin) {
        return nullptr;
    }

    PluginInfo info = plugin->getInfo();
    return createPluginInfoObject(env, info);
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeBeginCreatePluginUI(JNIEnv* env, jobject thiz, jint displayNumber, jint pluginIndex) {
    /* Set display state to Creating BEFORE the plugin UI creation starts.
     * This ensures signalDetachSurfaceFromDisplay will defer if called during creation. */
    {
        std::lock_guard<std::mutex> lock(displayStateMutex());
        displayStates()[displayNumber].phase = DisplayState::Phase::Creating;
        displayStates()[displayNumber].pluginIndex = pluginIndex;
        displayStates()[displayNumber].detachPending = false;
    }
    /* Also set the global flag for LV2Plugin destructor safety check */
    setCreatingPluginUI(true);
    LOGI("nativeBeginCreatePluginUI display=%d plugin=%d (signalDetach will defer)", displayNumber, pluginIndex);
}

JNIEXPORT jboolean JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeCreatePluginUI(JNIEnv* env, jobject thiz, jint pluginIndex, jint displayNumber, jlong parentWindowId) {
    LOGI("nativeCreatePluginUI ENTER tid=%ld pluginIndex=%d displayNumber=%d parentWindowId=0x%lx", getTid(), pluginIndex, displayNumber, (unsigned long)parentWindowId);
    
    if (!g_ctx->audioEngine) {
        LOGE("nativeCreatePluginUI: audio engine not initialized");
        setDisplayPhase(displayNumber, DisplayState::Phase::None);
        return JNI_FALSE;
    }
    
    bool result = g_ctx->pluginUIManager->createPluginUI(
        pluginIndex,
        displayNumber,
        (unsigned long)parentWindowId,
        ensureCtx()->nativeLibDir,
        ensureCtx()->x11LibsDir
    );
    
    // Check if detach was requested during creation
    bool detachPending = false;
    {
        std::lock_guard<std::mutex> lock(displayStateMutex());
        auto it = displayStates().find(displayNumber);
        if (it != displayStates().end()) {
            detachPending = it->second.detachPending;
            if (result) {
                it->second.phase = DisplayState::Phase::Ready;
            } else {
                it->second.phase = DisplayState::Phase::None;
            }
        }
    }
    
    if (detachPending && result) {
        /* Surface was destroyed while we were creating the UI.
         * Destroy the plugin UI now that creation is complete. */
        LOGI("nativeCreatePluginUI: deferred detach detected, destroying plugin UI");
        g_ctx->pluginUIManager->destroyPluginUI(pluginIndex);
        setDisplayPhase(displayNumber, DisplayState::Phase::None);
    }
    
    /* Clear the global creating flag when done */
    setCreatingPluginUI(false);
    
    LOGI("nativeCreatePluginUI EXIT tid=%ld result=%s", getTid(), result ? "true" : "false");
    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeDestroyPluginUI(JNIEnv* env, jobject thiz, jint pluginIndex) {
    LOGI("nativeDestroyPluginUI CALLED tid=%ld pluginIndex=%d", getTid(), pluginIndex);
    if (g_ctx->audioEngine) {
        g_ctx->pluginUIManager->destroyPluginUI(pluginIndex);
    }
    LOGI("nativeDestroyPluginUI RETURNED tid=%ld", getTid());
}

JNIEXPORT jboolean JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeIdlePluginUIs(JNIEnv* env, jobject thiz) {
    if (g_ctx->audioEngine) {
        return g_ctx->pluginUIManager->idleAllUIs() ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;
}

// --- X11 native display (EGL + ANativeWindow) ---

JNIEXPORT jlong JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeAttachSurfaceToDisplay(JNIEnv* env, jobject thiz, jint displayNumber, jobject surface, jint width, jint height) {
    if (!surface || width <= 0 || height <= 0) {
        LOGE("nativeAttachSurfaceToDisplay: invalid surface or size");
        return 0;
    }
    LOGI("nativeAttachSurfaceToDisplay display=%d width=%d height=%d", displayNumber, width, height);
    
    /* Initialize display state BEFORE attaching surface */
    {
        std::lock_guard<std::mutex> lock(displayStateMutex());
        displayStates()[displayNumber].phase = DisplayState::Phase::Attached;
        displayStates()[displayNumber].pluginIndex = -1;
        displayStates()[displayNumber].detachPending = false;
    }
    
    X11NativeDisplay* disp = getOrCreateX11Display(displayNumber);
    if (!disp->attachSurface(env, surface, width, height)) {
        LOGE("nativeAttachSurfaceToDisplay: attach failed for display %d", displayNumber);
        std::lock_guard<std::mutex> lock(displayStateMutex());
        displayStates().erase(displayNumber);
        return 0;
    }
    
    jlong rootId = static_cast<jlong>(disp->getRootWindowId());
    LOGI("nativeAttachSurfaceToDisplay display=%d -> rootId=%ld", displayNumber, (long)rootId);
    return rootId;
}

JNIEXPORT jboolean JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeSignalDetachSurfaceFromDisplay(JNIEnv* env, jobject thiz, jint displayNumber) {
    LOGI("nativeSignalDetachSurfaceFromDisplay ENTER display=%d tid=%ld", displayNumber, getTid());
    
    /* Always defer when creating - never close X connection during plugin creation.
     * This prevents "X connection closed" crashes when the plugin is still
     * initializing its X11 connection. */
    auto phase = getDisplayPhase(displayNumber);
    if (phase == DisplayState::Phase::Creating) {
        std::lock_guard<std::mutex> lock(displayStateMutex());
        auto it = displayStates().find(displayNumber);
        if (it != displayStates().end()) {
            it->second.detachPending = true;
        }
        LOGI("nativeSignalDetachSurfaceFromDisplay: DEFERRED (creating) display=%d", displayNumber);
        return JNI_TRUE;
    }
    
    /* For other phases, mark for destruction but don't actually close yet.
     * The Kotlin layer will call nativeDetachAndDestroyX11DisplayIfExists
     * after the appropriate delay. */
    {
        std::lock_guard<std::mutex> lock(displayStateMutex());
        auto it = displayStates().find(displayNumber);
        if (it != displayStates().end()) {
            it->second.phase = DisplayState::Phase::Destroying;
        }
    }
    
    LOGI("nativeSignalDetachSurfaceFromDisplay: marked destroying display=%d", displayNumber);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeStopX11RenderThreadOnly(JNIEnv* env, jobject thiz, jint displayNumber) {
    X11NativeDisplay* disp = getX11Display(displayNumber);
    if (disp) disp->stopRenderThreadOnly();
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeDetachSurfaceFromDisplay(JNIEnv* env, jobject thiz, jint displayNumber) {
    LOGI("nativeDetachSurfaceFromDisplay ENTER display=%d tid=%ld", displayNumber, getTid());
    X11NativeDisplay* disp = getX11Display(displayNumber);
    if (disp) disp->detachSurface();
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeDestroyX11Display(JNIEnv* env, jobject thiz, jint displayNumber) {
    destroyX11Display(displayNumber);
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeDetachAndDestroyX11DisplayIfExists(JNIEnv* env, jobject thiz, jint displayNumber) {
    LOGI("nativeDetachAndDestroyX11DisplayIfExists ENTER display=%d tid=%ld", displayNumber, getTid());
    
    auto phase = getDisplayPhase(displayNumber);
    
    /* If still creating, defer teardown to avoid closing X connection during plugin init. */
    if (phase == DisplayState::Phase::Creating) {
        LOGI("nativeDetachAndDestroyX11DisplayIfExists: SKIP (creating) display=%d", displayNumber);
        return;
    }
    
    /* Mark as destroying and proceed with teardown */
    setDisplayPhase(displayNumber, DisplayState::Phase::Destroying);
    
    X11NativeDisplay* disp = getX11Display(displayNumber);
    if (disp) {
        LOGI("nativeDetachAndDestroyX11DisplayIfExists: detaching and destroying display=%d", displayNumber);
        disp->detachSurface();
        destroyX11Display(displayNumber);
    }
    
    /* Clean up state */
    {
        std::lock_guard<std::mutex> lock(displayStateMutex());
        displayStates().erase(displayNumber);
    }
    
    LOGI("nativeDetachAndDestroyX11DisplayIfExists EXIT display=%d", displayNumber);
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeHideX11Display(JNIEnv* env, jobject thiz, jint displayNumber) {
    LOGI("nativeHideX11Display ENTER display=%d tid=%ld", displayNumber, getTid());
    
    X11NativeDisplay* disp = getX11Display(displayNumber);
    if (disp) {
        LOGI("nativeHideX11Display: stopping render thread for display=%d", displayNumber);
        // Stop the render thread to prevent eglSwapBuffers and driver mutex issues
        disp->stopRenderThreadOnly();
        LOGI("nativeHideX11Display: display=%d hidden (render thread stopped)", displayNumber);
    } else {
        LOGI("nativeHideX11Display: display=%d not found", displayNumber);
    }
    
    LOGI("nativeHideX11Display EXIT display=%d", displayNumber);
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeResumeX11Display(JNIEnv* env, jobject thiz, jint displayNumber) {
    LOGI("nativeResumeX11Display ENTER display=%d tid=%ld", displayNumber, getTid());
    
    X11NativeDisplay* disp = getX11Display(displayNumber);
    if (disp) {
        LOGI("nativeResumeX11Display: starting render thread for display=%d", displayNumber);
        // Restart the render thread to resume rendering
        disp->startRenderThread();
        LOGI("nativeResumeX11Display: display=%d resumed (render thread started)", displayNumber);
    } else {
        LOGI("nativeResumeX11Display: display=%d not found", displayNumber);
    }
    
    LOGI("nativeResumeX11Display EXIT display=%d", displayNumber);
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeSetSurfaceSize(JNIEnv* env, jobject thiz, jint displayNumber, jint width, jint height) {
    withDisplaySetSurfaceSize(displayNumber, width, height);
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeInjectTouch(JNIEnv* env, jobject thiz, jint displayNumber, jint action, jint x, jint y) {
    withDisplayInjectTouch(displayNumber, action, x, y);
}

JNIEXPORT jboolean JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeIsWidgetAtPoint(JNIEnv* env, jobject thiz, jint displayNumber, jint x, jint y) {
    return withDisplayIsWidgetAtPoint(displayNumber, x, y) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeRequestX11Frame(JNIEnv* env, jobject thiz, jint displayNumber) {
    withDisplayRequestFrame(displayNumber);
}

JNIEXPORT jintArray JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeGetX11PluginSize(JNIEnv* env, jobject thiz, jint displayNumber) {
    int w = 0, h = 0;
    withDisplayGetPluginSize(displayNumber, w, h);
    jintArray result = env->NewIntArray(2);
    if (result) {
        jint arr[2] = { w, h };
        env->SetIntArrayRegion(result, 0, 2, arr);
    }
    return result;
}

JNIEXPORT jfloat JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeGetX11UIScale(JNIEnv* env, jobject thiz, jint displayNumber) {
    return withDisplayGetUIScale(displayNumber);
}

JNIEXPORT jobjectArray JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativePollFileRequest(JNIEnv* env, jobject thiz) {
    if (!g_ctx || !g_ctx->pluginUIManager) return nullptr;

    guitarrackcraft::PluginUIManager::FileRequest req;
    if (!g_ctx->pluginUIManager->pollFileRequest(req)) return nullptr;

    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray result = env->NewObjectArray(2, stringClass, nullptr);
    if (!result) return nullptr;

    jstring indexStr = env->NewStringUTF(std::to_string(req.pluginIndex).c_str());
    jstring uriStr = env->NewStringUTF(req.propertyUri.c_str());
    env->SetObjectArrayElement(result, 0, indexStr);
    env->SetObjectArrayElement(result, 1, uriStr);
    env->DeleteLocalRef(indexStr);
    env->DeleteLocalRef(uriStr);
    env->DeleteLocalRef(stringClass);

    return result;
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeDeliverFileToPluginUI(
    JNIEnv* env, jobject thiz, jint pluginIndex, jstring propertyUri, jstring filePath)
{
    if (!g_ctx || !g_ctx->pluginUIManager) return;

    const char* propStr = env->GetStringUTFChars(propertyUri, nullptr);
    const char* pathStr = env->GetStringUTFChars(filePath, nullptr);
    if (propStr && pathStr) {
        g_ctx->pluginUIManager->deliverFileToUI(
            pluginIndex, std::string(propStr), std::string(pathStr));
    }
    if (propStr) env->ReleaseStringUTFChars(propertyUri, propStr);
    if (pathStr) env->ReleaseStringUTFChars(filePath, pathStr);
}

// --- Real-time recording ---

JNIEXPORT jboolean JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeStartRecording(JNIEnv* env, jobject thiz, jstring rawPath, jstring processedPath) {
    if (!g_ctx || !g_ctx->audioEngine) {
        return JNI_FALSE;
    }
    if (!rawPath || !processedPath) {
        return JNI_FALSE;
    }
    const char* rawStr = env->GetStringUTFChars(rawPath, nullptr);
    const char* procStr = env->GetStringUTFChars(processedPath, nullptr);
    if (!rawStr || !procStr) {
        if (rawStr) env->ReleaseStringUTFChars(rawPath, rawStr);
        if (procStr) env->ReleaseStringUTFChars(processedPath, procStr);
        return JNI_FALSE;
    }
    bool result = g_ctx->audioEngine->getRecorder().startRecording(
        std::string(rawStr), std::string(procStr), g_ctx->audioEngine->getSampleRate());
    env->ReleaseStringUTFChars(rawPath, rawStr);
    env->ReleaseStringUTFChars(processedPath, procStr);
    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeStopRecording(JNIEnv* env, jobject thiz) {
    if (g_ctx && g_ctx->audioEngine) {
        g_ctx->audioEngine->getRecorder().stopRecording();
    }
}

JNIEXPORT jboolean JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeIsRecording(JNIEnv* env, jobject thiz) {
    if (!g_ctx || !g_ctx->audioEngine) {
        return JNI_FALSE;
    }
    return g_ctx->audioEngine->getRecorder().isRecording() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jdouble JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeGetRecordingDurationSec(JNIEnv* env, jobject thiz) {
    if (!g_ctx || !g_ctx->audioEngine) {
        return 0.0;
    }
    return g_ctx->audioEngine->getRecorder().getDurationSec();
}

// --- State save/restore (presets) ---

JNIEXPORT jstring JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeSaveChainState(JNIEnv* env, jobject thiz) {
    if (!g_ctx || !g_ctx->audioEngine) {
        return nullptr;
    }

    auto chainState = g_ctx->audioEngine->getChain().saveChainState();
    std::string json = serializeChainStateToJson(chainState);
    return env->NewStringUTF(json.c_str());
}

JNIEXPORT jboolean JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeRestorePluginState(
    JNIEnv* env, jobject thiz, jint pluginIndex,
    jfloatArray portValues, jintArray portIndices,
    jobjectArray propKeys, jobjectArray propTypes,
    jobjectArray propValues, jintArray propFlags)
{
    if (!g_ctx || !g_ctx->audioEngine) {
        return JNI_FALSE;
    }

    PluginState state;

    // Unpack control port values
    if (portValues && portIndices) {
        jsize portCount = env->GetArrayLength(portValues);
        jfloat* vals = env->GetFloatArrayElements(portValues, nullptr);
        jint* idxs = env->GetIntArrayElements(portIndices, nullptr);
        if (vals && idxs) {
            for (jsize i = 0; i < portCount; ++i) {
                state.controlPortValues.emplace_back(
                    static_cast<uint32_t>(idxs[i]), static_cast<float>(vals[i]));
            }
        }
        if (vals) env->ReleaseFloatArrayElements(portValues, vals, JNI_ABORT);
        if (idxs) env->ReleaseIntArrayElements(portIndices, idxs, JNI_ABORT);
    }

    // Unpack state properties
    if (propKeys && propTypes && propValues && propFlags) {
        jsize propCount = env->GetArrayLength(propKeys);
        jint* flags = env->GetIntArrayElements(propFlags, nullptr);
        for (jsize i = 0; i < propCount; ++i) {
            StateProperty prop;

            auto keyStr = static_cast<jstring>(env->GetObjectArrayElement(propKeys, i));
            auto typeStr = static_cast<jstring>(env->GetObjectArrayElement(propTypes, i));
            auto valArr = static_cast<jbyteArray>(env->GetObjectArrayElement(propValues, i));

            if (keyStr) {
                const char* k = env->GetStringUTFChars(keyStr, nullptr);
                if (k) { prop.keyUri = k; env->ReleaseStringUTFChars(keyStr, k); }
                env->DeleteLocalRef(keyStr);
            }
            if (typeStr) {
                const char* t = env->GetStringUTFChars(typeStr, nullptr);
                if (t) { prop.typeUri = t; env->ReleaseStringUTFChars(typeStr, t); }
                env->DeleteLocalRef(typeStr);
            }
            if (valArr) {
                jsize len = env->GetArrayLength(valArr);
                prop.value.resize(len);
                env->GetByteArrayRegion(valArr, 0, len, reinterpret_cast<jbyte*>(prop.value.data()));
                env->DeleteLocalRef(valArr);
            }
            if (flags) prop.flags = static_cast<uint32_t>(flags[i]);

            state.properties.push_back(std::move(prop));
        }
        if (flags) env->ReleaseIntArrayElements(propFlags, flags, JNI_ABORT);
    }

    bool ok = g_ctx->audioEngine->getChain().restorePluginState(pluginIndex, state);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeSetChainBypass(JNIEnv* /*env*/, jobject /*thiz*/, jboolean bypass) {
    if (g_ctx && g_ctx->audioEngine) {
        g_ctx->audioEngine->setChainBypass(bypass == JNI_TRUE);
    }
}

JNIEXPORT void JNICALL
Java_com_varcain_guitarrackcraft_engine_NativeEngine_nativeSetWavBypassChain(JNIEnv* /*env*/, jobject /*thiz*/, jboolean bypass) {
    if (g_ctx && g_ctx->audioEngine) {
        g_ctx->audioEngine->setWavBypassChain(bypass == JNI_TRUE);
    }
}

} // extern "C"
