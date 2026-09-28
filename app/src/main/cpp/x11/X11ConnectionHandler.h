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
#include <cstdint>
#include <cstring>
#include <vector>

namespace guitarrackcraft {

struct HandshakeResult {
    bool success = false;
    bool msbFirst = true;
    uint16_t majorVersion = 0;
    uint16_t minorVersion = 0;
    uint16_t authNameLen = 0;
    uint16_t authDataLen = 0;
};

class X11ConnectionHandler {
public:
    // Parse a 12-byte connection request. Does NOT read from socket —
    // caller provides the raw bytes. Returns handshake info.
    static HandshakeResult parseConnectionRequest(const uint8_t* request12);

    // Build the 132-byte connection reply into a buffer.
    // `resourceIdBase` is per-connection — see Java X server's behavior
    // where each new client gets base 0x100000, 0x200000, 0x300000...
    // Wine spawns multiple processes (explorer, vst_host, wineserver),
    // each opening its own X connection. If they all share the same
    // base, their resource ID allocations collide and wine deadlocks
    // waiting for resource cleanup.
    static std::vector<uint8_t> buildConnectionReply(
        const X11ByteOrder& bo, int displayWidth, int displayHeight,
        uint32_t resourceIdBase = 0x00100000);
};

} // namespace guitarrackcraft
