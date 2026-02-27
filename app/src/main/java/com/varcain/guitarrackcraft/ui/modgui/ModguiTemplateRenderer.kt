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

package com.varcain.guitarrackcraft.ui.modgui

import com.varcain.guitarrackcraft.engine.PluginInfo
import com.varcain.guitarrackcraft.engine.PortInfo
import java.io.File

/**
 * Data extracted from modgui.ttl + plugin ports for Mustache-style template rendering.
 */
data class ModguiTemplateData(
    val brand: String = "",
    val label: String = "",
    val color: String = "",
    val knob: String = "",
    val cns: String = "",
    val controls: List<ControlEntry> = emptyList(),
    val effect: EffectPortsData = EffectPortsData(),
    val pathParameters: List<PathParameterEntry> = emptyList()
)

data class ControlEntry(
    val symbol: String = "",
    val name: String = "",
    val comment: String = ""
)

data class EffectPortsData(
    val ports: PortsByType = PortsByType()
)

data class PortsByType(
    val audio: AudioPorts = AudioPorts(),
    val midi: MidiPorts = MidiPorts(),
    val cv: CvPorts = CvPorts()
)

data class AudioPorts(
    val input: List<PortEntry> = emptyList(),
    val output: List<PortEntry> = emptyList()
)

data class MidiPorts(
    val input: List<PortEntry> = emptyList(),
    val output: List<PortEntry> = emptyList()
)

data class CvPorts(
    val input: List<PortEntry> = emptyList(),
    val output: List<PortEntry> = emptyList()
)

data class PortEntry(
    val symbol: String = "",
    val name: String = ""
)

data class PathParameterEntry(
    val uri: String = ""
)

/**
 * Parse modgui.ttl in the bundle directory to get brand, label, color, knob.
 * Controls list is built from pluginInfo.controlPorts (symbol/name); effect.ports from pluginInfo.ports.
 */
fun parseModguiTtlAndPluginInfo(basePath: String, pluginInfo: PluginInfo): ModguiTemplateData {
    val audioInput = pluginInfo.ports.filter { it.isAudio && it.isInput }.map { PortEntry(it.symbol, it.name.ifEmpty { it.symbol }) }
    val audioOutput = pluginInfo.ports.filter { it.isAudio && !it.isInput }.map { PortEntry(it.symbol, it.name.ifEmpty { it.symbol }) }
    var brand = pluginInfo.name
    var label = pluginInfo.name
    var color = "black"
    var knob = "cairo"

    // Build a lookup from symbol to control port info
    val portBySymbol = pluginInfo.controlPorts.associateBy { it.symbol }

    // Parse modgui.ttl for port ordering and scalar values
    var controls: List<ControlEntry> = emptyList()
    var modguiTtl = File(basePath, "modgui.ttl")
    if (!modguiTtl.exists()) modguiTtl = File(basePath, "modguis.ttl")
    if (modguiTtl.exists()) {
        val fullText = modguiTtl.readText()
        // For multi-plugin bundles (modguis.ttl), extract only the block for this plugin.
        // Each plugin block starts with <URI> and ends with " .\n"
        val text = run {
            val idStart = fullText.indexOf(pluginInfo.id)
            if (idStart < 0) fullText
            else {
                val blockEnd = fullText.indexOf(" .", idStart)
                if (blockEnd < 0) fullText.substring(idStart) else fullText.substring(idStart, blockEnd + 2)
            }
        }
        Regex("""modgui:brand\s+"([^"]*)"\s*""").find(text)?.groupValues?.get(1)?.let { brand = it }
        Regex("""modgui:label\s+"([^"]*)"\s*""").find(text)?.groupValues?.get(1)?.let { label = it }
        Regex("""modgui:color\s+"([^"]*)"\s*""").find(text)?.groupValues?.get(1)?.let { color = it }
        Regex("""modgui:knob\s+"([^"]*)"\s*""").find(text)?.groupValues?.get(1)?.let { knob = it }

        // Parse modgui:port entries to get the correct control ordering
        // Each port block: [ lv2:index N ; lv2:symbol "SYM" ; lv2:name "NAME" ; ]
        // Note: trailing semicolons before ] are optional in TTL
        val portBlockPattern = Regex("""\[\s*lv2:index\s+(\d+)\s*;\s*lv2:symbol\s+"([^"]*)"\s*;\s*lv2:name\s+"([^"]*)"\s*;?\s*\]""")
        val modguiPorts = portBlockPattern.findAll(text).map { m ->
            Triple(m.groupValues[1].toInt(), m.groupValues[2], m.groupValues[3])
        }.sortedBy { it.first }.toList()

        if (modguiPorts.isNotEmpty()) {
            controls = modguiPorts.map { (_, symbol, name) ->
                val displayName = name.ifEmpty { portBySymbol[symbol]?.name ?: symbol }
                ControlEntry(symbol = symbol, name = displayName, comment = displayName)
            }
        }
    }

    // Fallback: if modgui.ttl didn't define port order, use pluginInfo order
    // Exclude toggle ports (bypass/enable) — they are handled by the footswitch UI
    if (controls.isEmpty()) {
        controls = pluginInfo.controlPorts
            .filter { !it.isToggle }
            .map { p ->
                ControlEntry(symbol = p.symbol, name = p.name.ifEmpty { p.symbol }, comment = p.name.ifEmpty { p.symbol })
            }
    }

    // Detect path-type parameters (e.g. NAM/AIDA-X model file path)
    val pathParams = when {
        pluginInfo.id.contains("neural-amp-modeler") ->
            listOf(PathParameterEntry(uri = "http://github.com/mikeoliphant/neural-amp-modeler-lv2#model"))
        pluginInfo.id.contains("aidadsp") ->
            listOf(PathParameterEntry(uri = "http://aidadsp.cc/plugins/aidadsp-bundle/rt-neural-generic#json"))
        else -> emptyList()
    }

    return ModguiTemplateData(
        brand = brand,
        label = label,
        color = color,
        knob = knob,
        cns = "",
        controls = controls,
        effect = EffectPortsData(
            ports = PortsByType(
                audio = AudioPorts(input = audioInput, output = audioOutput),
                midi = MidiPorts(),
                cv = CvPorts()
            )
        ),
        pathParameters = pathParams
    )
}

/**
 * Minimal Mustache-style render: {{key}}, {{{key}}}, {{#key}}...{{/key}} for lists.
 * Supports nested effect.ports.audio.input etc. via a flat map of string keys to values or list of maps.
 */
fun renderModguiTemplate(html: String, data: ModguiTemplateData): String {
    val scalar = mapOf(
        "brand" to data.brand,
        "label" to data.label,
        "color" to data.color,
        "knob" to data.knob,
        "cns" to data.cns,
        "ns" to ""
    )
    val sections: Map<String, List<Map<String, String>>> = mapOf(
        "controls" to data.controls.map { listOf("symbol" to it.symbol, "name" to it.name, "comment" to it.comment).toMap() },
        "effect.ports.audio.input" to data.effect.ports.audio.input.map { listOf("symbol" to it.symbol, "name" to it.name).toMap() },
        "effect.ports.audio.output" to data.effect.ports.audio.output.map { listOf("symbol" to it.symbol, "name" to it.name).toMap() },
        "effect.ports.midi.input" to data.effect.ports.midi.input.map { listOf("symbol" to it.symbol, "name" to it.name).toMap() },
        "effect.ports.midi.output" to data.effect.ports.midi.output.map { listOf("symbol" to it.symbol, "name" to it.name).toMap() },
        "effect.ports.cv.input" to data.effect.ports.cv.input.map { listOf("symbol" to it.symbol, "name" to it.name).toMap() },
        "effect.ports.cv.output" to data.effect.ports.cv.output.map { listOf("symbol" to it.symbol, "name" to it.name).toMap() },
        "effect.parameters.path" to data.pathParameters.map { mapOf("uri" to it.uri) }
    )
    var out = html
    // Flatten nested {{#effect.parameters}}{{#path}}...{{/path}}{{/effect.parameters}}
    // into {{#effect.parameters.path}}...{{/effect.parameters.path}} so our flat renderer can handle it.
    out = out.replace("{{#effect.parameters}}", "")
    out = out.replace("{{/effect.parameters}}", "")
    out = out.replace("{{#path}}", "{{#effect.parameters.path}}")
    out = out.replace("{{/path}}", "{{/effect.parameters.path}}")
    for ((k, v) in scalar) {
        out = out.replace(Regex("""\{\{\{\s*$k\s*\}\}\}"""), v)
        out = out.replace(Regex("""\{\{\s*$k\s*\}\}"""), v)
    }
    // Process indexed sections like {{#controls.0}}...{{/controls.0}}
    // These reference a single item by index from a list section.
    val indexedPattern = Regex("""\{\{#(\w[\w.]*?)\.(\d+)\}\}([\s\S]*?)\{\{/\1\.\2\}\}""")
    out = indexedPattern.replace(out) { match ->
        val sectionName = match.groupValues[1]
        val index = match.groupValues[2].toInt()
        val block = match.groupValues[3]
        val list = sections[sectionName]
        if (list != null && index < list.size) {
            val item = list[index]
            var b = block
            for ((key, value) in item) {
                b = b.replace(Regex("""\{\{\{\s*$key\s*\}\}\}"""), value)
                b = b.replace(Regex("""\{\{\s*$key\s*\}\}"""), value)
            }
            b
        } else {
            "" // Index out of bounds or unknown section — remove block
        }
    }

    // Process list sections like {{#effect.ports.audio.input}}...{{/effect.ports.audio.input}}
    for ((sectionName, list) in sections) {
        val startTag = "{{#$sectionName}}"
        val endTag = "{{/$sectionName}}"
        val start = out.indexOf(startTag)
        if (start < 0) continue
        val end = out.indexOf(endTag)
        if (end < 0 || end <= start) continue
        val block = out.substring(start + startTag.length, end)
        val repeated = list.joinToString("") { item ->
            var b = block
            for ((key, value) in item) {
                b = b.replace(Regex("""\{\{\{\s*$key\s*\}\}\}"""), value)
                b = b.replace(Regex("""\{\{\s*$key\s*\}\}"""), value)
            }
            b
        }
        out = out.substring(0, start) + repeated + out.substring(end + endTag.length)
    }
    // Strip any remaining unresolved section blocks (e.g. effect.parameters)
    out = Regex("""\{\{#[\w.]+\}\}[\s\S]*?\{\{/[\w.]+\}\}""").replace(out, "")
    return out
}
