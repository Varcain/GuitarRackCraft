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

#pragma once

#include <cstdint>

namespace guitarrackcraft {

struct X11ByteOrder {
    bool msbFirst = true;

    uint16_t read16(const uint8_t* p, int off) const {
        if (msbFirst) return (uint16_t)((p[off] << 8) | p[off + 1]);
        return (uint16_t)(p[off] | (p[off + 1] << 8));
    }

    uint32_t read32(const uint8_t* p, int off) const {
        if (msbFirst) return (uint32_t)((p[off]<<24)|(p[off+1]<<16)|(p[off+2]<<8)|p[off+3]);
        return (uint32_t)(p[off]|(p[off+1]<<8)|(p[off+2]<<16)|(p[off+3]<<24));
    }

    void write16(uint8_t* p, int off, uint16_t val) const {
        if (msbFirst) { p[off] = (val >> 8) & 0xff; p[off+1] = val & 0xff; }
        else { p[off] = val & 0xff; p[off+1] = (val >> 8) & 0xff; }
    }

    void write32(uint8_t* p, int off, uint32_t val) const {
        if (msbFirst) {
            p[off] = (val >> 24) & 0xff; p[off+1] = (val >> 16) & 0xff;
            p[off+2] = (val >> 8) & 0xff; p[off+3] = val & 0xff;
        } else {
            p[off] = val & 0xff; p[off+1] = (val >> 8) & 0xff;
            p[off+2] = (val >> 16) & 0xff; p[off+3] = (val >> 24) & 0xff;
        }
    }
};

} // namespace guitarrackcraft
