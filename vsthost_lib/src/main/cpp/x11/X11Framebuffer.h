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
#include <cstddef>
#include <cstring>
#include <vector>

namespace guitarrackcraft {

struct ClipRect {
    int x1, y1, x2, y2;
};

class X11Framebuffer {
public:
    void resize(int w, int h, uint32_t fillColor = 0xFF302020);
    int width() const { return width_; }
    int height() const { return height_; }
    uint32_t* data() { return pixels_.data(); }
    const uint32_t* data() const { return pixels_.data(); }
    size_t pixelCount() const { return pixels_.size(); }
    bool empty() const { return pixels_.empty(); }

    // PutImage: blit source pixels into this framebuffer at (x, y).
    // Handles bounds clipping, byte-order conversion, alpha forcing, and child window clip rects.
    // pixelData is raw X11 wire format (4 bytes per pixel).
    void putImage(int x, int y, int w, int h,
                  const uint8_t* pixelData, size_t pixelDataLen,
                  bool msbFirst, const std::vector<ClipRect>& childClip);

    // GetImage: extract pixels from this framebuffer into dst buffer.
    // dst must have room for w*h uint32_t values. Out-of-bounds pixels are zeroed.
    // Returns data in X11 wire format (BGRA, same as stored).
    void getImage(int x, int y, int w, int h,
                  uint32_t* dst) const;

    // CopyArea: copy a w*h rectangle within this framebuffer (or between two buffers).
    // Static version operates on raw pointers for flexibility.
    static void copyArea(const uint32_t* src, int srcW, int srcH, int srcX, int srcY,
                         uint32_t* dst, int dstW, int dstH, int dstX, int dstY,
                         int w, int h);

    void clear();

private:
    int width_ = 0, height_ = 0;
    std::vector<uint32_t> pixels_;
};

} // namespace guitarrackcraft
