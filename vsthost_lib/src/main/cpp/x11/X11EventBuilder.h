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

#include "X11ByteOrder.h"
#include "X11Protocol.h"
#include <array>
#include <cstdint>
#include <cstring>

namespace guitarrackcraft {

class X11EventBuilder {
public:
    explicit X11EventBuilder(const X11ByteOrder& byteOrder)
        : bo_(byteOrder) {}

    void setByteOrder(const X11ByteOrder& bo) { bo_ = bo; }

    // Pointer events (ButtonPress, ButtonRelease, MotionNotify)
    std::array<uint8_t, 32> pointerEvent(uint8_t type, uint32_t window,
                                          int x, int y, uint8_t button,
                                          uint16_t seq, uint32_t timestamp,
                                          uint16_t state) const {
        std::array<uint8_t, 32> evt{};
        evt[0] = type;
        evt[1] = (type == X11Event::ButtonPress || type == X11Event::ButtonRelease)
                 ? button : 0;
        bo_.write16(evt.data(), 2, seq);
        bo_.write32(evt.data(), 4, timestamp);
        bo_.write32(evt.data(), 8, kRootWindowId);   // root
        bo_.write32(evt.data(), 12, window);          // event window
        bo_.write32(evt.data(), 16, 0);               // child (None)
        bo_.write16(evt.data(), 20, (uint16_t)(int16_t)x);  // root-x
        bo_.write16(evt.data(), 22, (uint16_t)(int16_t)y);  // root-y
        bo_.write16(evt.data(), 24, (uint16_t)(int16_t)x);  // event-x
        bo_.write16(evt.data(), 26, (uint16_t)(int16_t)y);  // event-y
        bo_.write16(evt.data(), 28, state);
        evt[30] = 1;  // same-screen = True
        return evt;
    }

    std::array<uint8_t, 32> buttonPress(uint32_t window, int x, int y,
                                         uint8_t button, uint16_t seq,
                                         uint32_t timestamp) const {
        return pointerEvent(X11Event::ButtonPress, window, x, y, button, seq, timestamp, 0);
    }

    std::array<uint8_t, 32> buttonRelease(uint32_t window, int x, int y,
                                           uint8_t button, uint16_t seq,
                                           uint32_t timestamp) const {
        uint16_t state = (button == 1) ? (1 << 8) : 0;  // Button1Mask
        return pointerEvent(X11Event::ButtonRelease, window, x, y, button, seq, timestamp, state);
    }

    std::array<uint8_t, 32> motionNotify(uint32_t window, int x, int y,
                                          uint16_t state, uint16_t seq,
                                          uint32_t timestamp) const {
        return pointerEvent(X11Event::MotionNotify, window, x, y, 0, seq, timestamp, state);
    }

    std::array<uint8_t, 32> expose(uint32_t window, int x, int y,
                                    int w, int h, uint16_t count,
                                    uint16_t seq) const {
        std::array<uint8_t, 32> evt{};
        evt[0] = X11Event::Expose;
        bo_.write16(evt.data(), 2, seq);
        bo_.write32(evt.data(), 4, window);
        bo_.write16(evt.data(), 8, (uint16_t)x);
        bo_.write16(evt.data(), 10, (uint16_t)y);
        bo_.write16(evt.data(), 12, (uint16_t)w);
        bo_.write16(evt.data(), 14, (uint16_t)h);
        bo_.write16(evt.data(), 16, count);
        return evt;
    }

    std::array<uint8_t, 32> configureNotify(uint32_t window, int x, int y,
                                             int w, int h, uint16_t borderWidth,
                                             uint16_t seq) const {
        std::array<uint8_t, 32> evt{};
        evt[0] = X11Event::ConfigureNotify;
        bo_.write16(evt.data(), 2, seq);
        bo_.write32(evt.data(), 4, window);     // event
        bo_.write32(evt.data(), 8, window);     // window
        bo_.write32(evt.data(), 12, 0);         // above-sibling (None)
        bo_.write16(evt.data(), 16, (uint16_t)(int16_t)x);
        bo_.write16(evt.data(), 18, (uint16_t)(int16_t)y);
        bo_.write16(evt.data(), 20, (uint16_t)w);
        bo_.write16(evt.data(), 22, (uint16_t)h);
        bo_.write16(evt.data(), 24, borderWidth);
        evt[26] = 0;  // override-redirect = False
        return evt;
    }

    std::array<uint8_t, 32> destroyNotify(uint32_t window, uint16_t seq) const {
        std::array<uint8_t, 32> evt{};
        evt[0] = X11Event::DestroyNotify;
        evt[1] = 0;
        bo_.write16(evt.data(), 2, seq);
        bo_.write32(evt.data(), 4, window);     // event
        bo_.write32(evt.data(), 8, window);     // window
        return evt;
    }

private:
    X11ByteOrder bo_;
};

} // namespace guitarrackcraft
