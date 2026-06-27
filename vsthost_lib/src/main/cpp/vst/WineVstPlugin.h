#ifndef VSTHOST_WINE_VST_PLUGIN_H
#define VSTHOST_WINE_VST_PLUGIN_H

#include "../../../../../app/src/main/cpp/plugin/IPlugin.h"
#include "VstFactory.h"
#include "../ipc/SharedRing.h"
#include "../ipc/PickerChannel.h"
#include "../launcher/WineHostProcess.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace vsthost {

/**
 * Wraps a single wine subprocess hosting one user-imported VST as a
 * GuitarRackCraft IPlugin. Audio thread calls process() → frames flow
 * through SysV shm rings to vst_host.exe (running under wine-arm64ec with
 * FEX-Emu translating x86_64 PE code) and back.
 *
 * Latency: at least one block of round-trip (push input now → pull output
 * next call). Acceptable for the use case; documented in the integration
 * plan. Never zero-pad short input — feeder upstream guarantees full
 * blocks (feedback_vst_host_no_zero_pad).
 */
class WineVstPlugin : public guitarrackcraft::IPlugin {
public:
    WineVstPlugin(RegistryEntry entry,
                  std::string filesDir,
                  std::string wineRoot,
                  std::string assetsDir,
                  std::string nativeLibDir,
                  int displayNumber);
    ~WineVstPlugin() override;

    void activate(float sampleRate, uint32_t bufferSize) override;
    void deactivate() override;
    void process(const float* const* inputs, float* const* outputs, uint32_t numFrames) override;

    guitarrackcraft::PluginInfo getInfo() const override;
    void setParameter(uint32_t portIndex, float value) override;
    float getParameter(uint32_t portIndex) const override;
    bool pollNativeFilePicker(guitarrackcraft::NativeFilePickerRequest& request) override;
    void respondNativeFilePicker(uint32_t sequence, bool cancelled, const std::string& windowsPath) override;

    /** Persist the latest normalized VST parameter values published by the
     *  Wine guest, falling back to the host-side mirror for legacy guests. */
    guitarrackcraft::PluginState saveState() override;

    uint32_t getNumInputPorts() const override  { return 2; }
    uint32_t getNumOutputPorts() const override { return 2; }
    int getX11DisplayNumber() const override    { return displayNumber_; }
    int32_t getEditorWidth()  const override;
    int32_t getEditorHeight() const override;

    /** Restore normalized parameter values through the host→guest parameter
     *  ring so the plugin processor and native editor receive the preset. */
    bool restoreState(const guitarrackcraft::PluginState& state) override;

    int32_t getUnderrunCount() const override {
        return underruns_.load(std::memory_order_relaxed);
    }
    int getSubprocessPid() const override {
        return guest_ ? guest_->pid() : -1;
    }

private:
    RegistryEntry entry_;
    std::string filesDir_;
    std::string wineRoot_;
    std::string assetsDir_;
    std::string nativeLibDir_;
    std::string winePrefix_;
    int displayNumber_ = -1;

    float sampleRate_ = 48000.0f;
    uint32_t bufferSize_ = 0;
    std::atomic<bool> active_{false};

    std::unique_ptr<SharedRing>      ring_;
    std::unique_ptr<PickerChannel>   picker_;
    std::unique_ptr<WineHostProcess> guest_;

    // Reused per process() call to avoid alloc on the audio thread.
    std::vector<float> interleaved_;

    // Host-side mirror of param values pushed via setParameter or refreshed
    // from the guest snapshot. Used as a fallback for legacy guest builds.
    mutable std::vector<float> paramMirror_;

    bool requestGuestState(uint32_t command, const std::string& path,
                           uint64_t size, uint64_t* outSize,
                           std::string* error) const;
    std::vector<uint8_t> saveGuestStateBlob() const;
    bool restoreGuestStateBlob(const std::vector<uint8_t>& blob) const;

    std::atomic<int32_t> underruns_{0};
};

} // namespace vsthost

#endif
