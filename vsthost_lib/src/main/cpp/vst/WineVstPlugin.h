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

    /** Persist the host-side mirror of VST parameter values so presets
     *  round-trip slider state. NOT captured: knob turns inside the wine
     *  editor — the guest doesn't push values back to host yet (would need
     *  a `param_values` array in shared_layout.h + a vst_host.exe rebuild).
     *  Slider-driven workflow saves/restores correctly. */
    guitarrackcraft::PluginState saveState() override;

    uint32_t getNumInputPorts() const override  { return 2; }
    uint32_t getNumOutputPorts() const override { return 2; }
    int getX11DisplayNumber() const override    { return displayNumber_; }
    int32_t getEditorWidth()  const override;
    int32_t getEditorHeight() const override;

    /** No control-port / state-property save-restore yet — we'd need to
     *  query wine for current param values via shm. For now the override
     *  exists only so loadPreset doesn't return false (default returns
     *  false → PresetManager flags the whole load as failed). The plugin
     *  is still re-instantiated correctly; it just loads in its DAW-default
     *  param state, which is also the standalone-vstpoc behaviour today. */
    bool restoreState(const guitarrackcraft::PluginState& state) override;

private:
    RegistryEntry entry_;
    std::string filesDir_;
    std::string wineRoot_;
    std::string assetsDir_;
    std::string nativeLibDir_;
    int displayNumber_ = -1;

    float sampleRate_ = 48000.0f;
    uint32_t bufferSize_ = 0;
    std::atomic<bool> active_{false};

    std::unique_ptr<SharedRing>      ring_;
    std::unique_ptr<PickerChannel>   picker_;
    std::unique_ptr<WineHostProcess> guest_;

    // Reused per process() call to avoid alloc on the audio thread.
    std::vector<float> interleaved_;

    // Host-side mirror of param values pushed via setParameter. Source of
    // truth for getParameter() readback + saveState. NOT updated by guest
    // (wine editor's knob turns aren't reflected here — see saveState doc).
    mutable std::vector<float> paramMirror_;

    std::atomic<int32_t> underruns_{0};
};

} // namespace vsthost

#endif
