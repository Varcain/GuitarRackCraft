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
 * It comes on the include path AFTER the X11 sysroot (CMake puts the include
 * directories before CMAKE_CXX_FLAGS, where aidax_full_configure.sh adds it),
 * so it is the GL/gl.h only because the sysroot has none since the native
 * Mesa build is gone. (An older build/x11_ui may still hold Mesa's desktop
 * GL headers, which declare a superset of this.)
 */
#ifndef _GL_GL_H_COMPAT_SHIM
#define _GL_GL_H_COMPAT_SHIM

#ifndef DGL_USE_GLES2
#  error "This GL/gl.h shim is only for DGL_USE_GLES2 builds"
#else

#include <GLES2/gl2.h>

/* GL_BGR, which DPF's image-format mapping (OpenGL.hpp, NanoVG.cpp) uses
 * besides GLES2's formats; the value is desktop GL's. */
#define GL_BGR 0x80E0

/* DPF's OpenGL.cpp also compiles its legacy GL 1.x drawing (immediate mode,
 * matrix stack), which a DGL_USE_GLES2 build never calls. Declared only so it
 * compiles: --gc-sections drops it, and libGLESv2 doesn't export these, so
 * --no-undefined fails the link if any of them ever becomes reachable. */
#define GL_QUADS                0x0007
#define GL_POLYGON              0x0009
#define GL_TEXTURE_BORDER_COLOR 0x1004
typedef double GLdouble;
#ifdef __cplusplus
extern "C" {
#endif
GL_APICALL void GL_APIENTRY glBegin(GLenum mode);
GL_APICALL void GL_APIENTRY glEnd(void);
GL_APICALL void GL_APIENTRY glVertex2d(GLdouble x, GLdouble y);
GL_APICALL void GL_APIENTRY glTexCoord2f(GLfloat s, GLfloat t);
GL_APICALL void GL_APIENTRY glColor3f(GLfloat red, GLfloat green, GLfloat blue);
GL_APICALL void GL_APIENTRY glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
GL_APICALL void GL_APIENTRY glPushMatrix(void);
GL_APICALL void GL_APIENTRY glPopMatrix(void);
GL_APICALL void GL_APIENTRY glTranslatef(GLfloat x, GLfloat y, GLfloat z);
GL_APICALL void GL_APIENTRY glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
#ifdef __cplusplus
}
#endif

#endif /* DGL_USE_GLES2 */
#endif /* _GL_GL_H_COMPAT_SHIM */
