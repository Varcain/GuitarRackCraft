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
 * GL/glext.h compatibility shim for Android EGL+GLES2 builds.
 * Redirects to GLES2/gl2ext.h when DGL_USE_GLES2 is defined.
 */
#ifndef _GL_GLEXT_H_COMPAT_SHIM
#define _GL_GLEXT_H_COMPAT_SHIM

#ifdef DGL_USE_GLES2
#  include <GLES2/gl2ext.h>
#else
#  error "This GL/glext.h shim is only for DGL_USE_GLES2 builds"
#endif

#endif /* _GL_GLEXT_H_COMPAT_SHIM */
