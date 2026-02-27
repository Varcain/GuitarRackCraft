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

#include "StateSerializer.h"
#include <sstream>
#include <cstring>

namespace guitarrackcraft {

// Base64 encoding table
static const char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64Encode(const uint8_t* data, size_t len) {
    std::string result;
    result.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);
        result += kBase64Chars[(n >> 18) & 0x3F];
        result += kBase64Chars[(n >> 12) & 0x3F];
        result += (i + 1 < len) ? kBase64Chars[(n >> 6) & 0x3F] : '=';
        result += (i + 2 < len) ? kBase64Chars[n & 0x3F] : '=';
    }
    return result;
}

static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

// Check if a type URI represents a string-like value (store as JSON string, not base64)
static bool isStringType(const std::string& typeUri) {
    return typeUri == "http://lv2plug.in/ns/ext/atom#String" ||
           typeUri == "http://lv2plug.in/ns/ext/atom#Path" ||
           typeUri == "http://lv2plug.in/ns/ext/atom#URI";
}

std::string serializeChainStateToJson(const PluginChain::ChainState& state) {
    std::ostringstream os;
    os << "{\n  \"version\": 1,\n  \"plugins\": [";

    for (size_t pi = 0; pi < state.plugins.size(); ++pi) {
        const auto& ps = state.plugins[pi];
        if (pi > 0) os << ",";
        os << "\n    {\n      \"uri\": \"" << jsonEscape(ps.pluginUri) << "\",\n";

        // Control ports
        os << "      \"controlPorts\": [";
        for (size_t ci = 0; ci < ps.controlPortValues.size(); ++ci) {
            if (ci > 0) os << ", ";
            os << "{\"index\": " << ps.controlPortValues[ci].first
               << ", \"value\": " << ps.controlPortValues[ci].second << "}";
        }
        os << "],\n";

        // State properties
        os << "      \"stateProperties\": [";
        for (size_t si = 0; si < ps.properties.size(); ++si) {
            const auto& prop = ps.properties[si];
            if (si > 0) os << ",";
            os << "\n        {\n";
            os << "          \"key\": \"" << jsonEscape(prop.keyUri) << "\",\n";
            os << "          \"type\": \"" << jsonEscape(prop.typeUri) << "\",\n";
            os << "          \"flags\": " << prop.flags << ",\n";

            if (isStringType(prop.typeUri) && !prop.value.empty()) {
                // String value — strip trailing null if present
                size_t len = prop.value.size();
                if (len > 0 && prop.value[len - 1] == 0) --len;
                std::string strVal(reinterpret_cast<const char*>(prop.value.data()), len);
                os << "          \"value\": \"" << jsonEscape(strVal) << "\"\n";
            } else if (!prop.value.empty()) {
                // Binary — base64 encode
                os << "          \"encoding\": \"base64\",\n";
                os << "          \"value\": \"" << base64Encode(prop.value.data(), prop.value.size()) << "\"\n";
            } else {
                os << "          \"value\": \"\"\n";
            }
            os << "        }";
        }
        if (!ps.properties.empty()) os << "\n      ";
        os << "]\n    }";
    }

    if (!state.plugins.empty()) os << "\n  ";
    os << "]\n}\n";
    return os.str();
}

} // namespace guitarrackcraft
