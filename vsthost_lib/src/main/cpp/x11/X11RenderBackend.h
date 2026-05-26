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

// Abstract interface for the display rendering pipeline.
// Production uses EGL/GLES; tests use NullRenderBackend.
class X11RenderBackend {
public:
    virtual ~X11RenderBackend() = default;
    virtual bool init(int width, int height) = 0;
    virtual void present(const uint32_t* framebuffer, int w, int h) = 0;
    virtual void resize(int width, int height) = 0;
    virtual void shutdown() = 0;
};

// No-op render backend for testing — accepts all calls, renders nothing.
class NullRenderBackend : public X11RenderBackend {
public:
    bool init(int, int) override { return true; }
    void present(const uint32_t*, int, int) override {}
    void resize(int, int) override {}
    void shutdown() override {}
};

} // namespace guitarrackcraft
