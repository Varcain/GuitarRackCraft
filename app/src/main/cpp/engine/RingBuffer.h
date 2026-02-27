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

#ifndef GUITARRACKCRAFT_RING_BUFFER_H
#define GUITARRACKCRAFT_RING_BUFFER_H

#include <atomic>
#include <cstring>
#include <vector>

namespace guitarrackcraft {

/**
 * Single-producer single-consumer lock-free ring buffer for float samples.
 * Uses power-of-2 capacity with bitmask for fast modulo.
 *
 * Thread safety:
 *   - write() called from one thread (audio callback)
 *   - read() called from one thread (writer thread)
 *   - reset() called only when idle (no concurrent read/write)
 */
class RingBuffer {
public:
    explicit RingBuffer(size_t minCapacity = 0)
        : writePos_(0), readPos_(0)
    {
        if (minCapacity > 0) {
            resize(minCapacity);
        }
    }

    void resize(size_t minCapacity) {
        // Round up to power of 2
        size_t cap = 1;
        while (cap < minCapacity) cap <<= 1;
        buffer_.resize(cap);
        mask_ = cap - 1;
        writePos_.store(0, std::memory_order_relaxed);
        readPos_.store(0, std::memory_order_relaxed);
    }

    /**
     * Write samples into the ring buffer.
     * @return number of floats actually written (may be < count if full)
     */
    size_t write(const float* data, size_t count) {
        size_t w = writePos_.load(std::memory_order_relaxed);
        size_t r = readPos_.load(std::memory_order_acquire);
        size_t available = capacity() - (w - r);
        size_t toWrite = count < available ? count : available;

        for (size_t i = 0; i < toWrite; ++i) {
            buffer_[(w + i) & mask_] = data[i];
        }

        writePos_.store(w + toWrite, std::memory_order_release);
        return toWrite;
    }

    /**
     * Read samples from the ring buffer.
     * @return number of floats actually read (may be < count if empty)
     */
    size_t read(float* data, size_t count) {
        size_t r = readPos_.load(std::memory_order_relaxed);
        size_t w = writePos_.load(std::memory_order_acquire);
        size_t available = w - r;
        size_t toRead = count < available ? count : available;

        for (size_t i = 0; i < toRead; ++i) {
            data[i] = buffer_[(r + i) & mask_];
        }

        readPos_.store(r + toRead, std::memory_order_release);
        return toRead;
    }

    size_t available() const {
        size_t w = writePos_.load(std::memory_order_acquire);
        size_t r = readPos_.load(std::memory_order_acquire);
        return w - r;
    }

    size_t capacity() const { return buffer_.size(); }

    void reset() {
        writePos_.store(0, std::memory_order_relaxed);
        readPos_.store(0, std::memory_order_relaxed);
    }

private:
    std::vector<float> buffer_;
    size_t mask_ = 0;
    std::atomic<size_t> writePos_;
    std::atomic<size_t> readPos_;
};

} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_RING_BUFFER_H
