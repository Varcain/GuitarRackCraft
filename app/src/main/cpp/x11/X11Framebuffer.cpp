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

#include "X11Framebuffer.h"
#include <algorithm>

namespace guitarrackcraft {

void X11Framebuffer::resize(int w, int h, uint32_t fillColor) {
    width_ = w;
    height_ = h;
    pixels_.assign((size_t)w * h, fillColor);
}

void X11Framebuffer::putImage(int x, int y, int w, int h,
                               const uint8_t* pixelData, size_t pixelDataLen,
                               bool msbFirst, const std::vector<ClipRect>& childClip) {
    if (pixels_.empty() || w <= 0 || h <= 0) return;

    uint32_t* dstBuf = pixels_.data();
    int dW = width_, dH = height_;

    bool fullyCovered = (x >= 0 && y >= 0 && x + w <= dW && y + h <= dH
                         && pixelDataLen >= (size_t)w * h * 4);

    if (fullyCovered && !msbFirst && childClip.empty()) {
        // Fast path: fully inside, LSB-first, no child clipping
        const uint32_t* src32 = reinterpret_cast<const uint32_t*>(pixelData);
        for (int row = 0; row < h; row++) {
            uint32_t* dstRow = dstBuf + (y + row) * dW + x;
            const uint32_t* srcRow = src32 + row * w;
            for (int col = 0; col < w; col++) {
                dstRow[col] = srcRow[col] | 0xFF000000u;
            }
        }
    } else if (fullyCovered && !msbFirst) {
        // Fast path with child clipping
        const uint32_t* src32 = reinterpret_cast<const uint32_t*>(pixelData);
        for (int row = 0; row < h; row++) {
            int dstY = y + row;
            uint32_t* dstRow = dstBuf + dstY * dW + x;
            const uint32_t* srcRow = src32 + row * w;
            for (int col = 0; col < w; col++) {
                int dstX = x + col;
                bool clipped = false;
                for (auto& cr : childClip) {
                    if (dstX >= cr.x1 && dstX < cr.x2 && dstY >= cr.y1 && dstY < cr.y2) {
                        clipped = true; break;
                    }
                }
                if (!clipped) dstRow[col] = srcRow[col] | 0xFF000000u;
            }
        }
    } else {
        // Slow path with bounds checking and child clipping
        size_t maxPixelIdx = pixelDataLen;
        for (int row = 0; row < h; row++) {
            int dstY = y + row;
            if (dstY < 0 || dstY >= dH) continue;
            for (int col = 0; col < w; col++) {
                int dstX = x + col;
                if (dstX < 0 || dstX >= dW) continue;
                bool clipped = false;
                for (auto& cr : childClip) {
                    if (dstX >= cr.x1 && dstX < cr.x2 && dstY >= cr.y1 && dstY < cr.y2) {
                        clipped = true; break;
                    }
                }
                if (clipped) continue;
                size_t srcIdx = (row * w + col) * 4;
                if (srcIdx + 3 >= maxPixelIdx) continue;
                uint32_t pixel;
                if (msbFirst) {
                    uint8_t a = pixelData[srcIdx], r = pixelData[srcIdx+1],
                            g = pixelData[srcIdx+2], b = pixelData[srcIdx+3];
                    pixel = (a << 24) | (r << 16) | (g << 8) | b;
                } else {
                    memcpy(&pixel, &pixelData[srcIdx], 4);
                }
                dstBuf[(size_t)dstY * dW + dstX] = pixel | 0xFF000000u;
            }
        }
    }
}

void X11Framebuffer::getImage(int x, int y, int w, int h, uint32_t* dst) const {
    if (w <= 0 || h <= 0) return;

    bool fullyCovered = (x >= 0 && y >= 0 && x + w <= width_ && y + h <= height_);
    if (fullyCovered && !pixels_.empty()) {
        for (int row = 0; row < h; row++) {
            memcpy(dst + row * w, pixels_.data() + (y + row) * width_ + x, w * 4);
        }
    } else {
        // Partial coverage: zero first, then copy overlapping region
        memset(dst, 0, (size_t)w * h * 4);
        if (!pixels_.empty()) {
            int startRow = std::max(0, -y);
            int endRow = std::min(h, height_ - y);
            int startCol = std::max(0, -x);
            int endCol = std::min(w, width_ - x);
            if (startRow < endRow && startCol < endCol) {
                int copyW = endCol - startCol;
                for (int row = startRow; row < endRow; row++) {
                    memcpy(dst + row * w + startCol,
                           pixels_.data() + (y + row) * width_ + (x + startCol),
                           copyW * 4);
                }
            }
        }
    }
}

void X11Framebuffer::copyArea(const uint32_t* src, int srcW, int srcH, int srcX, int srcY,
                               uint32_t* dst, int dstW, int dstH, int dstX, int dstY,
                               int w, int h) {
    if (!src || !dst || w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++) {
        int sy = srcY + row, dy = dstY + row;
        if (sy < 0 || sy >= srcH || dy < 0 || dy >= dstH) continue;
        for (int col = 0; col < w; col++) {
            int sx = srcX + col, dx = dstX + col;
            if (sx < 0 || sx >= srcW || dx < 0 || dx >= dstW) continue;
            dst[dy * dstW + dx] = src[sy * srcW + sx];
        }
    }
}

void X11Framebuffer::clear() {
    width_ = 0;
    height_ = 0;
    pixels_.clear();
}

} // namespace guitarrackcraft
