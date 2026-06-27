#include "WineVstPlugin.h"
#include "../util/log.h"
#include "../x11/X11NativeDisplay.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace vsthost {

namespace {

constexpr uint32_t kNumAudioPorts = 4;
constexpr int kGuestReadyForStateTimeoutMs = 30000;
constexpr int kGuestStateTimeoutMs = 10000;
constexpr size_t kMaxGuestStateBytes = 64u * 1024u * 1024u;
constexpr char kVstStatePropertyKey[] = "urn:guitarrackcraft:vst:state";
constexpr char kBinaryType[] = "application/octet-stream";

std::atomic<uint32_t> gStateTransferSeq{0};

size_t boundedStringLength(const char* value, size_t maxLen) {
    size_t len = 0;
    while (len < maxLen && value[len] != '\0') ++len;
    return len;
}

std::string flattenWin32FilterPatterns(const char* filter, size_t maxLen) {
    std::string patterns;
    size_t pos = 0;
    bool nextIsPattern = false;
    while (pos < maxLen && filter[pos] != '\0') {
        const size_t len = boundedStringLength(filter + pos, maxLen - pos);
        if (nextIsPattern && len > 0) {
            if (!patterns.empty()) patterns.push_back('\n');
            patterns.append(filter + pos, len);
        }
        nextIsPattern = !nextIsPattern;
        pos += len + 1;
    }
    return patterns;
}

float sanitizeNormalizedParam(float value) {
    if (!std::isfinite(value)) return 0.0f;
    return std::clamp(value, 0.0f, 1.0f);
}

bool readGuestParamSnapshot(const SharedRing* ring, std::vector<float>& out) {
    const VstpocShared* shared = ring ? ring->raw() : nullptr;
    if (!shared) return false;

    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint64_t seqBefore = __atomic_load_n(&shared->param_values_seq, __ATOMIC_ACQUIRE);
        if (seqBefore == 0) return false;
        if ((seqBefore & 1u) != 0) continue;

        const int32_t count = std::max(0, std::min<int32_t>(shared->param_count, VSTPOC_MAX_PARAMS));
        if (count <= 0) return false;

        std::vector<float> snapshot(static_cast<size_t>(count));
        for (int32_t i = 0; i < count; ++i) {
            snapshot[static_cast<size_t>(i)] = sanitizeNormalizedParam(shared->param_values[i]);
        }

        const uint64_t seqAfter = __atomic_load_n(&shared->param_values_seq, __ATOMIC_ACQUIRE);
        if (seqBefore == seqAfter && (seqAfter & 1u) == 0) {
            out = std::move(snapshot);
            return true;
        }
    }
    return false;
}

std::string makeStateTransferPath(const std::string& filesDir, const std::string& uuid) {
    const std::string tmpDir = filesDir + "/tmp";
    ::mkdir(tmpDir.c_str(), 0700);
    const uint32_t seq = gStateTransferSeq.fetch_add(1, std::memory_order_relaxed) + 1;
    return tmpDir + "/vst_state_v" + uuid + "_" + std::to_string(::getpid()) +
           "_" + std::to_string(seq) + ".bin";
}

bool readBinaryFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff size = f.tellg();
    if (size < 0 || static_cast<uint64_t>(size) > kMaxGuestStateBytes) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (!out.empty()) {
        f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    }
    return f.good() || f.eof();
}

bool writeBinaryFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    if (bytes.size() > kMaxGuestStateBytes) return false;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (!bytes.empty()) {
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    return f.good();
}

} // namespace

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
    // Shared "activation environment" prefix (manager-installed plugins point
    // at their environment's wineprefix_e<uuid>); empty => legacy per-plugin
    // wineprefix_v<uuid>. shm/picker/display stay keyed by uuid (see below) so
    // two plugins sharing a prefix still get distinct IPC + X11 displays.
    winePrefix_           = entry_.prefixPath.empty()
                              ? (filesDir_ + "/wineprefix_v" + entry_.uuid)
                              : entry_.prefixPath;
    cfg.winePrefix        = winePrefix_;
    // Use the 64-bit host for x86_64 PE plugins, 32-bit host otherwise.
    cfg.primaryExe        = assetsDir_ + (entry_.is64Bit ? "/vst_host.exe" : "/vst_host_x86.exe");
    cfg.shmPath           = shmPath;
    if (picker_) cfg.pickerShmPath = pickerPath;
    cfg.pluginPaths       = { entry_.dllPath };
    cfg.logSuffix         = "v" + entry_.uuid;

    // vstpoc experiment hook (2026-06-02, BIAS FX 2 / CEF editors): inject extra
    // host argv WITHOUT a rebuild. vst3_host.exe reads only argv[1]=shm + argv[2]=
    // plugin and ignores the rest, but a libcef-based editor (BIAS FX 2 ships
    // Chromium 116) parses GetCommandLineW(), so Chromium switches appended here
    // flow through to it (unless the plugin sets command_line_args_disabled). Drop
    // whitespace/newline-separated tokens in <cache>/cef_args.txt and relaunch the
    // plugin to iterate switch sets (e.g. "--disable-gpu-compositing
    // --disable-audio-output") without recompiling. Lines starting with '#' are
    // skipped. Absent file = no extra args (default, zero regression).
    {
        std::ifstream af(cfg.cacheDir + "/cef_args.txt");
        std::string line;
        while (std::getline(af, line)) {
            size_t b = line.find_first_not_of(" \t\r\n");
            if (b == std::string::npos || line[b] == '#') continue;
            std::istringstream ls(line.substr(b));
            std::string tok;
            while (ls >> tok) cfg.extraArgs.push_back(tok);
        }
        if (!cfg.extraArgs.empty())
            LOGI("WineVstPlugin[%s]: cef_args.txt injected %zu host arg(s); first=%s",
                 entry_.displayName.c_str(), cfg.extraArgs.size(),
                 cfg.extraArgs.front().c_str());
    }

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
            std::vector<float> guestValues;
            if (readGuestParamSnapshot(ring_.get(), guestValues)) {
                paramMirror_ = std::move(guestValues);
            }
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

bool WineVstPlugin::pollNativeFilePicker(guitarrackcraft::NativeFilePickerRequest& request) {
    if (!picker_) return false;

    uint32_t seq = 0;
    if (!picker_->hasRequest(&seq)) return false;

    char title[VSTPOC_PICKER_TITLE_LEN] = {};
    char filter[VSTPOC_PICKER_FILTER_LEN] = {};
    char initialDir[VSTPOC_PICKER_PATH_LEN] = {};
    picker_->readRequest(title, filter, initialDir);

    request.sequence = seq;
    request.title = title;
    request.filterPatterns = flattenWin32FilterPatterns(filter, sizeof(filter));
    request.initialDir = initialDir;

    const std::string prefix = winePrefix_.empty()
        ? (entry_.prefixPath.empty() ? (filesDir_ + "/wineprefix_v" + entry_.uuid) : entry_.prefixPath)
        : winePrefix_;
    request.copyDirLinux = prefix + "/drive_c/vstpoc_picker";
    request.copyDirWindows = "C:\\vstpoc_picker";
    return true;
}

void WineVstPlugin::respondNativeFilePicker(uint32_t sequence,
                                            bool cancelled,
                                            const std::string& windowsPath) {
    if (!picker_) return;
    picker_->writeResponse(sequence, cancelled, windowsPath.c_str());
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
    const int32_t vstIdx = static_cast<int32_t>(portIndex) - static_cast<int32_t>(kNumAudioPorts);
    if (vstIdx < 0) return;
    if (vstIdx >= static_cast<int32_t>(paramMirror_.size())) {
        paramMirror_.resize(static_cast<size_t>(vstIdx) + 1, 0.5f);
    }
    const float normalized = sanitizeNormalizedParam(value);
    paramMirror_[vstIdx] = normalized;
    if (ring_) ring_->pushParam(vstIdx, normalized);
}

float WineVstPlugin::getParameter(uint32_t portIndex) const {
    const int32_t vstIdx = static_cast<int32_t>(portIndex) - static_cast<int32_t>(kNumAudioPorts);
    if (vstIdx < 0) return 0.0f;
    std::vector<float> guestValues;
    if (readGuestParamSnapshot(ring_.get(), guestValues) &&
        vstIdx < static_cast<int32_t>(guestValues.size())) {
        paramMirror_ = std::move(guestValues);
        return paramMirror_[static_cast<size_t>(vstIdx)];
    }
    if (vstIdx >= static_cast<int32_t>(paramMirror_.size())) return 0.5f;
    return paramMirror_[vstIdx];
}

bool WineVstPlugin::requestGuestState(uint32_t command,
                                      const std::string& path,
                                      uint64_t size,
                                      uint64_t* outSize,
                                      std::string* error) const {
    VstpocShared* shared = ring_ ? ring_->raw() : nullptr;
    if (!shared) {
        if (error) *error = "guest not ready";
        return false;
    }

    const auto readyDeadline = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(kGuestReadyForStateTimeoutMs);
    while (__atomic_load_n(&shared->guest_ready, __ATOMIC_ACQUIRE) == 0 &&
           std::chrono::steady_clock::now() < readyDeadline) {
        if (__atomic_load_n(&shared->stop_flag, __ATOMIC_ACQUIRE) != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (__atomic_load_n(&shared->guest_ready, __ATOMIC_ACQUIRE) == 0) {
        if (error) *error = "guest not ready";
        return false;
    }
    if (path.empty() || path.size() >= VSTPOC_STATE_PATH_LEN) {
        if (error) *error = "state transfer path too long";
        return false;
    }

    std::memset(shared->state_path, 0, sizeof(shared->state_path));
    std::memcpy(shared->state_path, path.data(), path.size());
    std::memset(shared->state_message, 0, sizeof(shared->state_message));
    __atomic_store_n(&shared->state_size, size, __ATOMIC_RELEASE);
    __atomic_store_n(&shared->state_status, VSTPOC_STATE_STATUS_IDLE, __ATOMIC_RELEASE);
    __atomic_store_n(&shared->state_command, command, __ATOMIC_RELEASE);
    __sync_synchronize();

    const uint32_t requestSeq =
        __atomic_add_fetch(&shared->state_request_seq, 1, __ATOMIC_RELEASE);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kGuestStateTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const uint32_t responseSeq =
            __atomic_load_n(&shared->state_response_seq, __ATOMIC_ACQUIRE);
        if (responseSeq == requestSeq) {
            const uint32_t status =
                __atomic_load_n(&shared->state_status, __ATOMIC_ACQUIRE);
            if (outSize) {
                *outSize = __atomic_load_n(&shared->state_size, __ATOMIC_ACQUIRE);
            }
            if (status == VSTPOC_STATE_STATUS_OK) return true;
            if (error) {
                const size_t len = boundedStringLength(shared->state_message,
                                                       sizeof(shared->state_message));
                *error = len > 0 ? std::string(shared->state_message, len)
                                 : (status == VSTPOC_STATE_STATUS_UNSUPPORTED
                                        ? "guest state unsupported"
                                        : "guest state request failed");
            }
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (error) *error = "guest state request timed out";
    return false;
}

std::vector<uint8_t> WineVstPlugin::saveGuestStateBlob() const {
    const std::string path = makeStateTransferPath(filesDir_, entry_.uuid);
    ::unlink(path.c_str());

    uint64_t size = 0;
    std::string error;
    if (!requestGuestState(VSTPOC_STATE_CMD_SAVE, path, 0, &size, &error)) {
        if (!error.empty() && error.find("unsupported") == std::string::npos) {
            LOGW("WineVstPlugin[%s]: save guest state failed: %s",
                 entry_.displayName.c_str(), error.c_str());
        }
        ::unlink(path.c_str());
        return {};
    }
    if (size == 0 || size > kMaxGuestStateBytes) {
        LOGW("WineVstPlugin[%s]: guest state size invalid: %llu",
             entry_.displayName.c_str(), (unsigned long long)size);
        ::unlink(path.c_str());
        return {};
    }

    std::vector<uint8_t> blob;
    if (!readBinaryFile(path, blob)) {
        LOGW("WineVstPlugin[%s]: failed to read guest state file %s",
             entry_.displayName.c_str(), path.c_str());
        blob.clear();
    }
    ::unlink(path.c_str());
    return blob;
}

bool WineVstPlugin::restoreGuestStateBlob(const std::vector<uint8_t>& blob) const {
    if (blob.empty()) return false;
    const std::string path = makeStateTransferPath(filesDir_, entry_.uuid);
    if (!writeBinaryFile(path, blob)) {
        LOGW("WineVstPlugin[%s]: failed to write guest state file %s",
             entry_.displayName.c_str(), path.c_str());
        ::unlink(path.c_str());
        return false;
    }

    uint64_t ignored = 0;
    std::string error;
    const bool ok = requestGuestState(VSTPOC_STATE_CMD_LOAD, path, blob.size(), &ignored, &error);
    if (!ok) {
        LOGW("WineVstPlugin[%s]: restore guest state failed: %s",
             entry_.displayName.c_str(),
             error.empty() ? "unknown error" : error.c_str());
    }
    ::unlink(path.c_str());
    return ok;
}

guitarrackcraft::PluginState WineVstPlugin::saveState() {
    guitarrackcraft::PluginState ps;
    ps.pluginUri = entry_.uuid;
    ps.format    = entry_.format;

    std::vector<float> values;
    if (readGuestParamSnapshot(ring_.get(), values)) {
        paramMirror_ = values;
    } else {
        values = paramMirror_;
    }

    for (size_t i = 0; i < values.size(); ++i) {
        ps.controlPortValues.emplace_back(static_cast<uint32_t>(kNumAudioPorts + i),
                                          sanitizeNormalizedParam(values[i]));
    }

    std::vector<uint8_t> stateBlob = saveGuestStateBlob();
    if (!stateBlob.empty()) {
        LOGI("WineVstPlugin[%s]: saved guest state blob %zu bytes",
             entry_.displayName.c_str(), stateBlob.size());
        guitarrackcraft::StateProperty prop;
        prop.keyUri = kVstStatePropertyKey;
        prop.typeUri = kBinaryType;
        prop.flags = 0;
        prop.value = std::move(stateBlob);
        ps.properties.push_back(std::move(prop));
    }
    return ps;
}

bool WineVstPlugin::restoreState(const guitarrackcraft::PluginState& state) {
    bool hadStateBlob = false;
    bool restoredStateBlob = false;
    for (const auto& prop : state.properties) {
        if (prop.keyUri == kVstStatePropertyKey && !prop.value.empty()) {
            hadStateBlob = true;
            restoredStateBlob = restoreGuestStateBlob(prop.value);
            break;
        }
    }
    if (restoredStateBlob) {
        std::vector<float> guestValues;
        if (readGuestParamSnapshot(ring_.get(), guestValues)) {
            paramMirror_ = std::move(guestValues);
        }
        LOGI("WineVstPlugin[%s]: restored guest state blob; skipped %zu control fallback values",
             entry_.displayName.c_str(), state.controlPortValues.size());
        return true;
    }
    if (hadStateBlob) {
        LOGW("WineVstPlugin[%s]: guest state blob restore failed; replaying %zu control fallback values",
             entry_.displayName.c_str(), state.controlPortValues.size());
    }

    // Push each saved param back through setParameter — that updates the
    // host mirror AND forwards to the param ring so the wine editor's
    // knobs reflect the restored value.
    for (const auto& [portIndex, value] : state.controlPortValues) {
        setParameter(portIndex, value);
    }
    return !hadStateBlob || !state.controlPortValues.empty();
}

} // namespace vsthost
