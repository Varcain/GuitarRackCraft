#include "WineVstPlugin.h"
#include "../util/log.h"
#include "../x11/X11NativeDisplay.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace vsthost {

WineVstPlugin::WineVstPlugin(RegistryEntry entry,
                             std::string filesDir,
                             std::string wineRoot,
                             std::string assetsDir,
                             std::string nativeLibDir,
                             int displayNumber)
    : entry_(std::move(entry)),
      filesDir_(std::move(filesDir)),
      wineRoot_(std::move(wineRoot)),
      assetsDir_(std::move(assetsDir)),
      nativeLibDir_(std::move(nativeLibDir)),
      displayNumber_(displayNumber) {}

WineVstPlugin::~WineVstPlugin() {
    if (active_.load()) deactivate();
}

void WineVstPlugin::activate(float sampleRate, uint32_t bufferSize) {
    if (active_.load()) return;
    sampleRate_ = sampleRate;
    bufferSize_ = bufferSize ? bufferSize : 1024;
    interleaved_.assign(static_cast<size_t>(bufferSize_) * 2, 0.0f);

    // Per-plugin shm + picker files. Naming matches vstpoc convention so
    // wine-side env vars + tmpfs lookups behave the same. The "_v" + uuid
    // suffix keeps multiple VstFactory plugins from colliding.
    const std::string tmpDir     = filesDir_ + "/tmp";
    ::mkdir(tmpDir.c_str(), 0700);  // idempotent — ignore EEXIST
    const std::string shmPath    = tmpDir + "/vst_shm_v" + entry_.uuid + ".dat";
    const std::string pickerPath = tmpDir + "/vst_picker_v" + entry_.uuid + ".dat";

    ring_ = std::make_unique<SharedRing>(shmPath);
    if (!ring_->valid()) {
        LOGE("WineVstPlugin[%s]: shared ring at %s invalid",
             entry_.displayName.c_str(), shmPath.c_str());
        ring_.reset();
        return;
    }
    picker_ = std::make_unique<PickerChannel>(pickerPath);
    if (!picker_->valid()) {
        LOGW("WineVstPlugin[%s]: picker channel invalid; native picker disabled",
             entry_.displayName.c_str());
        picker_.reset();
    }

    WineHostProcess::Config cfg;
    cfg.nativeLibDir      = nativeLibDir_;
    cfg.cacheDir          = filesDir_ + "/../cache";
    cfg.wineBinary        = wineRoot_ + "/bin/wine";
    cfg.wineserverBinary  = wineRoot_ + "/bin/wineserver";
    cfg.wineDllPath       = wineRoot_ + "/lib/wine/aarch64-windows";
    cfg.winePrefix        = filesDir_ + "/wineprefix_v" + entry_.uuid;
    // Use the 64-bit host for x86_64 PE plugins, 32-bit host otherwise.
    cfg.primaryExe        = assetsDir_ + (entry_.is64Bit ? "/vst_host.exe" : "/vst_host_x86.exe");
    cfg.shmPath           = shmPath;
    if (picker_) cfg.pickerShmPath = pickerPath;
    cfg.pluginPaths       = { entry_.dllPath };
    cfg.logSuffix         = "v" + entry_.uuid;

    // CRITICAL: bring up the in-process X11 server on this plugin's display
    // BEFORE forking wine. Wine's winex11.drv calls XOpenDisplay at startup;
    // if the server isn't listening on 127.0.0.1:(6000+N) yet, it disables
    // X11 rendering permanently and the editor never shows.
    // Framebuffer placeholder = 4096x2160: wine's win32u get_surface_rect
    // crops the GDI surface to virtual_screen_rect() before allocating the
    // bitmap. A small placeholder (800x600) would clip plugins with editors
    // wider than that (AmpCraft is 1290x612, etc.). 4096x2160 fits any
    // current VST editor; nativeSetX11PluginSize() later tells the server
    // the actual editor size so the SurfaceView letterboxes correctly.
    guitarrackcraft::withDisplayStartServer(displayNumber_, 4096, 2160);
    // Read the ACTUAL port the X11 server bound on. If a previous wine
    // subprocess crashed and left an orphan listener on 6001, our server
    // skips up to 6101/6201/… and reports the higher port. We need to
    // pass the matching display number to wine so DISPLAY=127.0.0.1:N
    // points at OUR new listener, not the orphan's stuck accept backlog.
    int actualPort = guitarrackcraft::withDisplayGetActualPort(displayNumber_);
    int actualDisplay = (actualPort > 0) ? actualPort - 6000 : displayNumber_;
    cfg.displayNumber = actualDisplay;
    LOGI("WineVstPlugin[%s]: X11 server pre-started on display %d "
         "(requested) → port %d (actual, display %d for wine DISPLAY env)",
         entry_.displayName.c_str(), displayNumber_, actualPort, actualDisplay);

    guest_ = std::make_unique<WineHostProcess>(std::move(cfg));
    if (!guest_->start()) {
        LOGE("WineVstPlugin[%s]: failed to start wine guest", entry_.displayName.c_str());
        guest_.reset();
        ring_.reset();
        picker_.reset();
        return;
    }

    // Tell the ring we're feeding live input (vs. wine's internal sawtooth
    // test signal). Without this, the guest's vst_host loop reads from a
    // generated buffer instead of audio_in.
    ring_->setMicActive(true);

    // Block (with timeout) until the guest sets guest_ready=1 and populates
    // param_count + param_names. RackScreen captures pluginInfo ONCE via
    // remember(pluginIndex) — if getInfo() returns no control ports at that
    // moment, the sliders panel stays empty for the lifetime of the rack
    // row. Worth a short wait here so the slider list is correct on first
    // render. Timeout = 5s; if the guest hasn't reported by then it
    // probably never will, but the rest of the chain still works (audio
    // path doesn't depend on params).
    {
        auto* shared = ring_->raw();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (shared && shared->guest_ready == 0 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        if (shared && shared->guest_ready == 0) {
            LOGW("WineVstPlugin[%s] guest_ready timeout after 5s — sliders won't render",
                 entry_.displayName.c_str());
        } else if (shared) {
            paramMirror_.assign(static_cast<size_t>(std::max(0, shared->param_count)), 0.5f);
            LOGI("WineVstPlugin[%s] guest_ready param_count=%d",
                 entry_.displayName.c_str(), shared->param_count);
        }
    }

    active_.store(true);
    LOGI("WineVstPlugin[%s] activated pid=%d sr=%.0f bs=%u",
         entry_.displayName.c_str(), guest_->pid(), sampleRate, bufferSize_);
}

void WineVstPlugin::deactivate() {
    if (!active_.exchange(false)) return;
    if (ring_) ring_->signalStop();
    if (guest_) {
        if (!guest_->waitFor(3000)) {
            LOGW("WineVstPlugin[%s]: guest didn't exit on stop_flag, killing",
                 entry_.displayName.c_str());
            guest_->killHard();
        }
    }
    ring_.reset();
    picker_.reset();
    guest_.reset();
    /* Tear down the X11 server for this plugin so port 6000+N is freed.
     * Without this, the listening socket leaks: removing the plugin from
     * the rack leaves the X11NativeDisplay alive in g_displays, its
     * accept thread still bound to port 6001/6002/…, and the NEXT wine
     * subprocess that tries to connect lands in the orphan's accept
     * backlog forever. Symptoms: wine's `x11drv_init: XOpenDisplay...`
     * hangs, falls back to nodrv_CreateWindow, and the plugin's editor
     * fails with ERROR_INVALID_WINDOW_HANDLE (1400). Especially visible
     * after a wine subprocess crashes mid-init (TONEX / AmpliTube). */
    guitarrackcraft::destroyX11Display(displayNumber_);
    LOGI("WineVstPlugin[%s] deactivated (underruns=%d)",
         entry_.displayName.c_str(), underruns_.load(std::memory_order_relaxed));
}

void WineVstPlugin::process(const float* const* inputs,
                            float* const* outputs,
                            uint32_t numFrames) {
    if (!ring_ || !inputs || !outputs) {
        // Not active or no ring: silence outputs. Don't pass-through —
        // that would mask the underlying failure to the audio chain.
        if (outputs) {
            for (uint32_t ch = 0; ch < getNumOutputPorts(); ++ch) {
                if (outputs[ch]) std::memset(outputs[ch], 0, numFrames * sizeof(float));
            }
        }
        return;
    }

    // 1) push input into wine — interleave stereo into the reusable buffer.
    if (interleaved_.size() < static_cast<size_t>(numFrames) * 2) {
        interleaved_.assign(static_cast<size_t>(numFrames) * 2, 0.0f);
    }
    const float* inL = inputs[0];
    const float* inR = inputs[1] ? inputs[1] : inputs[0];   // mono → dup
    for (uint32_t i = 0; i < numFrames; ++i) {
        interleaved_[2 * i + 0] = inL ? inL[i] : 0.0f;
        interleaved_[2 * i + 1] = inR ? inR[i] : 0.0f;
    }
    ring_->pushInput(interleaved_.data(), static_cast<int32_t>(numFrames));

    // 2) pull processed output. There's a ≥1-block round-trip latency by
    //    design; pulled < numFrames is expected at startup and on any
    //    transient stall. Zero-fill the gap (don't reuse stale data) and
    //    bump the underrun counter. Never zero-pad inputs upstream —
    //    feedback_vst_host_no_zero_pad.
    const int32_t pulled = ring_->pullAudio(outputs[0], outputs[1], static_cast<int32_t>(numFrames));
    if (pulled < static_cast<int32_t>(numFrames)) {
        for (int32_t i = pulled; i < static_cast<int32_t>(numFrames); ++i) {
            outputs[0][i] = 0.0f;
            if (outputs[1]) outputs[1][i] = 0.0f;
        }
        underruns_.fetch_add(1, std::memory_order_relaxed);
    }
}

int32_t WineVstPlugin::getEditorWidth() const {
    return (ring_ && ring_->raw()) ? ring_->raw()->editor_width : 0;
}

int32_t WineVstPlugin::getEditorHeight() const {
    return (ring_ && ring_->raw()) ? ring_->raw()->editor_height : 0;
}

guitarrackcraft::PluginInfo WineVstPlugin::getInfo() const {
    guitarrackcraft::PluginInfo info;
    info.id = entry_.format + ":" + entry_.uuid;
    info.name = entry_.displayName;
    info.format = entry_.format;
    // Audio ports — stereo in/out, indices 0..3. Control ports for VST
    // params start at 4 (numAudio); the JNI bridge / RackScreen separates
    // them by isControl/isAudio flags.
    guitarrackcraft::PortInfo in_l { 0, "In L",  "in_l",  true,  true, false, false, 0, 0, 0, {} };
    guitarrackcraft::PortInfo in_r { 1, "In R",  "in_r",  true,  true, false, false, 0, 0, 0, {} };
    guitarrackcraft::PortInfo out_l{ 2, "Out L", "out_l", false, true, false, false, 0, 0, 0, {} };
    guitarrackcraft::PortInfo out_r{ 3, "Out R", "out_r", false, true, false, false, 0, 0, 0, {} };
    info.ports = { in_l, in_r, out_l, out_r };

    // Every VST has an X11-renderable editor (wine + winex11.drv). Setting
    // hasX11Ui=true makes the rack UI offer the "Native UI" chip alongside
    // the auto-generated SLIDERS view, so the user can pick which to see —
    // same pattern as LV2 plugins with X11UI bundles. RackScreen branches
    // on format to use VstInlineEditor (vsthost X11 server) instead of
    // PluginX11UiView (LV2's X11 server).
    info.hasX11Ui = true;

    // VST params reported by the guest via shm. param_count + param_names
    // are populated before guest_ready=1 (we block in activate). VST2
    // params are normalized [0,1]; min=0 max=1 default=0.5. We don't know
    // the plugin's TRUE defaults — the guest could supply them in a future
    // shared-layout bump.
    if (ring_ && ring_->raw() && ring_->raw()->guest_ready != 0) {
        const auto* shared = ring_->raw();
        const int32_t numAudio = static_cast<int32_t>(info.ports.size());
        const int32_t pc = std::max(0, std::min<int32_t>(shared->param_count, VSTPOC_MAX_PARAMS));
        for (int32_t i = 0; i < pc; ++i) {
            std::string name(shared->param_names[i]);
            if (name.empty()) name = "Param " + std::to_string(i + 1);
            guitarrackcraft::PortInfo p {
                static_cast<uint32_t>(numAudio + i),
                name, name,
                /*isInput=*/true, /*isAudio=*/false, /*isControl=*/true, /*isToggle=*/false,
                /*defaultValue=*/0.5f, /*minValue=*/0.0f, /*maxValue=*/1.0f,
                /*scalePoints=*/{}
            };
            info.ports.push_back(p);
        }
    }
    return info;
}

void WineVstPlugin::setParameter(uint32_t portIndex, float value) {
    // Control port indices start past the audio ports (4 = 2 in + 2 out).
    const int32_t numAudio = 4;
    const int32_t vstIdx = static_cast<int32_t>(portIndex) - numAudio;
    if (vstIdx < 0) return;
    if (vstIdx >= static_cast<int32_t>(paramMirror_.size())) {
        paramMirror_.resize(static_cast<size_t>(vstIdx) + 1, 0.5f);
    }
    paramMirror_[vstIdx] = value;
    if (ring_) ring_->pushParam(vstIdx, value);
}

float WineVstPlugin::getParameter(uint32_t portIndex) const {
    const int32_t numAudio = 4;
    const int32_t vstIdx = static_cast<int32_t>(portIndex) - numAudio;
    if (vstIdx < 0) return 0.0f;
    if (vstIdx >= static_cast<int32_t>(paramMirror_.size())) return 0.5f;
    return paramMirror_[vstIdx];
}

guitarrackcraft::PluginState WineVstPlugin::saveState() {
    guitarrackcraft::PluginState ps;
    ps.pluginUri = entry_.uuid;
    ps.format    = entry_.format;
    const uint32_t numAudio = 4;
    for (size_t i = 0; i < paramMirror_.size(); ++i) {
        ps.controlPortValues.emplace_back(static_cast<uint32_t>(numAudio + i), paramMirror_[i]);
    }
    return ps;
}

bool WineVstPlugin::restoreState(const guitarrackcraft::PluginState& state) {
    // Push each saved param back through setParameter — that updates the
    // host mirror AND forwards to the param ring so the wine editor's
    // knobs reflect the restored value.
    for (const auto& [portIndex, value] : state.controlPortValues) {
        setParameter(portIndex, value);
    }
    return true;
}

} // namespace vsthost
