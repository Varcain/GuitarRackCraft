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

#include "X11ConnectionHandler.h"

namespace guitarrackcraft {

HandshakeResult X11ConnectionHandler::parseConnectionRequest(const uint8_t* req) {
    HandshakeResult result;
    result.msbFirst = (req[0] == 0x42);  // 0x42 = MSB, 0x6c = LSB
    result.success = (req[0] == 0x42 || req[0] == 0x6c);

    X11ByteOrder bo{result.msbFirst};
    result.majorVersion = bo.read16(req, 2);
    result.minorVersion = bo.read16(req, 4);
    result.authNameLen = bo.read16(req, 6);
    result.authDataLen = bo.read16(req, 8);
    return result;
}

std::vector<uint8_t> X11ConnectionHandler::buildConnectionReply(
    const X11ByteOrder& bo, int displayWidth, int displayHeight) {

    std::vector<uint8_t> body(120, 0);
    size_t off = 0;

    // 0) Reply header (8 bytes)
    body[off++] = kX11ConnectionAccepted;
    body[off++] = 0;
    bo.write16(body.data() + off, 0, kX11Major); off += 2;
    bo.write16(body.data() + off, 0, kX11Minor); off += 2;
    bo.write16(body.data() + off, 0, 28); off += 2;  // additional data length in 4-byte units (112/4)

    // 1) Fixed setup prefix (18 bytes)
    bo.write32(body.data() + off, 0, 0);          // release-number
    bo.write32(body.data() + off, 4, 0x00200000); // resource-id-base
    bo.write32(body.data() + off, 8, 0x001FFFFF); // resource-id-mask
    bo.write32(body.data() + off, 12, 256);        // motion-buffer-size
    bo.write16(body.data() + off, 16, 0);          // vendor-length
    off += 18;

    // 2) Max request length, counts, image format, keycodes, pad (14 bytes)
    bo.write16(body.data() + off, 0, 32767); off += 2;  // max_request_length
    body[off++] = 1;   // num roots (screens)
    body[off++] = 1;   // num formats
    body[off++] = bo.msbFirst ? 1 : 0;  // image byte order
    body[off++] = bo.msbFirst ? 1 : 0;  // bitmap bit order
    body[off++] = 8; body[off++] = 8;    // bitmap scanline unit, pad
    body[off++] = 8; body[off++] = 255;  // min/max keycode
    body[off++] = 0; body[off++] = 0; body[off++] = 0; body[off++] = 0;  // pad

    // 3) PixmapFormat (8 bytes)
    body[off++] = 24; body[off++] = 32;  // depth, bits_per_pixel
    bo.write16(body.data() + off, 0, 32); off += 2;  // scanline_pad
    memset(body.data() + off, 0, 4); off += 4;

    // 4) WindowRoot (40 bytes)
    bo.write32(body.data() + off, 0, kRootWindowId);
    bo.write32(body.data() + off, 4, kDefaultColormapId);
    bo.write32(body.data() + off, 8, kWhitePixel);
    bo.write32(body.data() + off, 12, kBlackPixel);
    bo.write32(body.data() + off, 16, 0);  // current-input-masks
    bo.write16(body.data() + off, 20, (uint16_t)displayWidth);
    bo.write16(body.data() + off, 22, (uint16_t)displayHeight);
    bo.write16(body.data() + off, 24, (uint16_t)(displayWidth * 254 / 100));   // width-mm
    bo.write16(body.data() + off, 26, (uint16_t)(displayHeight * 254 / 100));  // height-mm
    bo.write16(body.data() + off, 28, 0);  // min-installed-maps
    bo.write16(body.data() + off, 30, 0);  // max-installed-maps
    bo.write32(body.data() + off, 32, kDefaultVisualId);  // root_visual
    body[off + 36] = 0;   // backing-stores
    body[off + 37] = 0;   // save-unders
    body[off + 38] = 24;  // root-depth
    body[off + 39] = 1;   // allowed-depths-count
    off += 40;

    // 5) Depth (8 bytes) + VisualType (24 bytes)
    body[off++] = 24; body[off++] = 0;  // depth, pad
    bo.write16(body.data() + off, 0, 1); off += 2;  // visuals-count
    memset(body.data() + off, 0, 4); off += 4;      // pad

    bo.write32(body.data() + off, 0, kDefaultVisualId);  // visual ID
    body[off + 4] = 4;     // class = TrueColor
    body[off + 5] = 8;     // bits_per_rgb_value (per channel, not total)
    bo.write16(body.data() + off, 6, 256);  // colormap_entries
    bo.write32(body.data() + off, 8, 0xff0000);   // red_mask
    bo.write32(body.data() + off, 12, 0x00ff00);  // green_mask
    bo.write32(body.data() + off, 16, 0x0000ff);  // blue_mask
    memset(body.data() + off + 20, 0, 4);         // pad
    off += 24;

    return body;
}

} // namespace guitarrackcraft
