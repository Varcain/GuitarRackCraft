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

#ifndef GUITARRACKCRAFT_IPLUGIN_H
#define GUITARRACKCRAFT_IPLUGIN_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/** A single atom event read from a plugin's atom output port after run(). */
struct OutputAtomEvent {
    uint32_t portIndex;
    std::vector<uint8_t> data;  // LV2_Atom header + body
};

namespace guitarrackcraft {

/** A single state property stored by a plugin via LV2 state:interface. */
struct StateProperty {
    std::string keyUri;
    std::vector<uint8_t> value;
    std::string typeUri;
    uint32_t flags = 0;
};

/** Complete snapshot of a plugin's state (control ports + extension properties). */
struct PluginState {
    std::string pluginUri;
    std::vector<std::pair<uint32_t, float>> controlPortValues;
    std::vector<StateProperty> properties;
};

/** One enumeration/scale point for a control port (label + value). */
struct ScalePoint {
    std::string label;
    float value;
};

struct PortInfo {
    uint32_t index;
    std::string name;
    std::string symbol;
    bool isInput;
    bool isAudio;
    bool isControl;
    bool isToggle;
    float defaultValue;
    float minValue;
    float maxValue;
    /** Enumeration values for this port; empty for continuous ports. */
    std::vector<ScalePoint> scalePoints;
};

struct PluginInfo {
    std::string id;
    std::string name;
    std::string format;  // "LV2", "CLAP", "VST3", etc.
    std::vector<PortInfo> ports;
    /** If non-empty, this plugin has modgui; path to bundle directory (no trailing slash). */
    std::string modguiBasePath;
    /** Relative path to modgui icon HTML (e.g. modgui/icon-gxmicroamp.html). Only set if modguiBasePath is set. */
    std::string modguiIconTemplate;
    /** True when the plugin ships an X11UI binary. */
    bool hasX11Ui = false;
    /** Absolute path to the X11 UI shared library (.so). */
    std::string x11UiBinaryPath;
    /** LV2 URI of the X11 UI (from the TTL). */
    std::string x11UiUri;
};

/**
 * Abstract interface for audio plugins.
 * All plugin formats (LV2, CLAP, VST3) must implement this interface.
 */
class IPlugin {
public:
    virtual ~IPlugin() = default;

    /**
     * Activate the plugin (called before processing starts).
     * @param sampleRate Audio sample rate in Hz
     * @param bufferSize Nominal audio callback buffer size in frames (0 = unknown)
     */
    virtual void activate(float sampleRate, uint32_t bufferSize = 0) = 0;

    /**
     * Deactivate the plugin (called after processing stops).
     */
    virtual void deactivate() = 0;

    /**
     * Process audio through the plugin.
     * @param inputs Array of input audio buffers (one per input port)
     * @param outputs Array of output audio buffers (one per output port)
     * @param numFrames Number of audio frames to process
     */
    virtual void process(const float* const* inputs, float* const* outputs, uint32_t numFrames) = 0;

    /**
     * Get plugin metadata.
     */
    virtual PluginInfo getInfo() const = 0;

    /**
     * Set a control parameter value.
     * @param portIndex Index of the control port
     * @param value Normalized value [0.0, 1.0] or denormalized depending on port
     */
    virtual void setParameter(uint32_t portIndex, float value) = 0;

    /**
     * Get a control parameter value.
     * @param portIndex Index of the control port
     * @return Current parameter value
     */
    virtual float getParameter(uint32_t portIndex) const = 0;

    /**
     * Get number of input audio ports.
     */
    virtual uint32_t getNumInputPorts() const = 0;

    /**
     * Get number of output audio ports.
     */
    virtual uint32_t getNumOutputPorts() const = 0;

    /**
     * Send a file path to the plugin via format-specific mechanism (e.g. LV2 patch:Set).
     * @param propertyUri URI of the property to set (e.g. model file parameter)
     * @param path Absolute file path
     */
    virtual void setFilePath(const std::string& propertyUri, const std::string& path) {}

    /**
     * Inject a raw atom message into the plugin's atom input port.
     * Used to forward atom writes from the UI (e.g. DPF state sync) to the DSP.
     * @param data  Atom data (LV2_Atom header + body)
     * @param size  Total size of atom data in bytes
     */
    virtual void injectAtom(const void* data, uint32_t size) {}

    /**
     * Save the plugin's complete state (control ports + extension properties).
     * @return PluginState snapshot; empty if plugin has no state to save.
     */
    virtual PluginState saveState() { return {}; }

    /**
     * Restore plugin state from a previously saved snapshot.
     * Must be called with exclusive access (no concurrent process()).
     * @return true if restore succeeded.
     */
    virtual bool restoreState(const PluginState& state) { return false; }

    /**
     * Drain queued atom events from output ports (produced by process/run).
     * Called from the UI thread to forward DSP atom responses to the UI.
     * @return Vector of atom events; empty if none pending.
     */
    virtual std::vector<OutputAtomEvent> drainOutputAtoms() { return {}; }
};

} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_IPLUGIN_H
