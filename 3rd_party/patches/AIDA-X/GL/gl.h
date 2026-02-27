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

/*
 * GL/gl.h compatibility shim for Android EGL+GLES2 builds.
 *
 * When DGL_USE_GLES2 is defined, redirect to GLES2/gl2.h so that DPF code
 * (OpenGL-include.hpp, nanovg, etc.) that does #include <GL/gl.h> gets the
 * GLES2 declarations instead. This avoids modifying any AIDA-X submodule files.
 *
 * Placed on the include path BEFORE the NDK/Mesa sysroot so it shadows
 * any system GL/gl.h.
 */
#ifndef _GL_GL_H_COMPAT_SHIM
#define _GL_GL_H_COMPAT_SHIM

#ifdef DGL_USE_GLES2
#  include <GLES2/gl2.h>
#else
#  error "This GL/gl.h shim is only for DGL_USE_GLES2 builds"
#endif

#endif /* _GL_GL_H_COMPAT_SHIM */
