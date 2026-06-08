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

// Build a connection-setup reply that matches the Java X server
// (au.com.darkside.xserver) byte-for-byte, with only the screen
// width/height parameterized. Verified via on-device wire capture
// where wine progresses past CreateWindow against Java's reply but
// loops on probes against our older custom reply.
//
// Total: 132 bytes (8-byte header + 124 bytes of additional data, i.e.
// length field = 31 four-byte words).
std::vector<uint8_t> X11ConnectionHandler::buildConnectionReply(
    const X11ByteOrder& bo, int displayWidth, int displayHeight,
    uint32_t resourceIdBase) {

    std::vector<uint8_t> body(132, 0);
    auto put32 = [&](size_t off, uint32_t v) { bo.write32(body.data() + off, 0, v); };
    auto put16 = [&](size_t off, uint16_t v) { bo.write16(body.data() + off, 0, v); };

    // [0..7] Reply header.
    body[0] = 1;             // success
    body[1] = 0;             // pad
    put16(2, 11);            // proto major
    put16(4, 0);             // proto minor
    put16(6, 31);            // length: 31 * 4 = 124 bytes follow

    // [8..39] Fixed setup info.
    put32(8, 0);             // release_number
    put32(12, resourceIdBase);   // per-connection — Java increments by 0x100000
    put32(16, 0x000FFFFF);   // resource_id_mask   (Java: 0x000fffff)
    put32(20, 0);            // motion_buffer_size (Java: 0)
    put16(24, 11);           // vendor length      ("Open source" = 11)
    put16(26, 0x7FFF);       // max_request_length
    body[28] = 1;            // num roots/screens
    body[29] = 1;            // num formats
    body[30] = 0;            // image byte order = LSB
    body[31] = 1;            // bitmap bit order = MSB  (Java: 1)
    body[32] = 8;            // bitmap scanline unit
    body[33] = 8;            // bitmap scanline pad
    body[34] = 8;            // min keycode
    body[35] = 0xA4;         // max keycode = 164       (Java: 0xa4)
    // [36..39] pad — already zero.

    // [40..51] Vendor string "Open source" + 1 pad byte.
    static const char* vendor = "Open source";
    for (size_t i = 0; i < 11; i++) body[40 + i] = (uint8_t)vendor[i];
    body[51] = 0;

    // [52..59] PixmapFormat:  depth=32, bpp=32, scanline_pad=32.
    // (Was bpp=24/pad=8 to match the Java X server. But depth=32 windows
    //  carry genuine 32-bit BGRA pixels — e.g. CEF/Chromium editors like
    //  BIAS FX 2. With bpp=24, wine computes the PutImage width as
    //  row_bytes/3 and ships the raw 4-byte BGRA mislabeled as 3-byte, so
    //  our reader unpacks it 3-wide → garbled/striped render. bpp=32 makes
    //  wine send 4-byte BGRA at the true width; the PutImage handler
    //  auto-detects 3 vs 4 bytes, so 3-byte-era plugins are unaffected.
    //  The "endless pixmap probing" warning was about depth=24, not this.)
    body[52] = 32;
    body[53] = 32;
    body[54] = 32;
    // [55..59] pad.

    // [60..91] Screen header (32 bytes).
    put32(60, kRootWindowId);                          // root window
    put32(64, kDefaultColormapId);                     // default colormap
    put32(68, kWhitePixel);                            // white pixel
    put32(72, kBlackPixel);                            // black pixel
    put32(76, 0);                                      // current input masks
    put16(80, (uint16_t)displayWidth);                 // width
    put16(82, (uint16_t)displayHeight);                // height
    // Java X server (au.com.darkside.xserver) hardcodes 65mm x 27mm
    // and that's what wine sees when it works against Java. Our prior
    // `px * 254 / 100` gave 3373mm x 1422mm which is 100x too big and
    // makes wine think DPI ≈ 10. Match Java byte-for-byte.
    put16(84, 65);                                     // mm-width
    put16(86, 27);                                     // mm-height
    put16(88, 1);                                      // min installed maps
    put16(90, 1);                                      // max installed maps
    put32(92, kDefaultVisualId);                       // root visual ID

    // [96..99]
    body[96] = 2;            // backing-stores = Always
    body[97] = 0;            // save-unders = false
    body[98] = 32;           // root depth (must match format above)
    body[99] = 1;             // num allowed depths

    // [100..107] Allowed-depth entry.
    body[100] = 32;          // depth
    body[101] = 0;            // pad
    put16(102, 1);            // num visuals at this depth
    // [104..107] pad

    // [108..131] Visual (24 bytes).
    put32(108, kDefaultVisualId);     // visual ID
    body[112] = 4;                    // class = TrueColor
    body[113] = 8;                    // bits per RGB value
    put16(114, 256);                  // colormap entries
    put32(116, 0x00FF0000);           // red mask
    put32(120, 0x0000FF00);           // green mask
    put32(124, 0x000000FF);           // blue mask
    // [128..131] pad

    return body;
}

} // namespace guitarrackcraft
