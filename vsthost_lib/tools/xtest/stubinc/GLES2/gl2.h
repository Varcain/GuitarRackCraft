/* Desktop stub for <GLES2/gl2.h>
 * Only the symbols / constants referenced by X11NativeDisplay.cpp. None of
 * these are ever called (the GLES render path is only reached via
 * attachSurface, which the harness never calls). */
#ifndef XTEST_STUB_GLES2_GL2_H
#define XTEST_STUB_GLES2_GL2_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef signed char    GLbyte;
typedef short          GLshort;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned char  GLubyte;
typedef unsigned short GLushort;
typedef unsigned int   GLuint;
typedef float          GLfloat;
typedef float          GLclampf;
typedef void           GLvoid;
typedef char           GLchar;
typedef ptrdiff_t      GLsizeiptr;
typedef ptrdiff_t      GLintptr;

#define GL_FALSE 0
#define GL_TRUE  1

#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_FLOAT            0x1406
#define GL_UNSIGNED_BYTE    0x1401
#define GL_RGBA             0x1908
#define GL_TRIANGLE_STRIP   0x0005
#define GL_TEXTURE_2D       0x0DE1
#define GL_TEXTURE0         0x84C0
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_LINEAR           0x2601
#define GL_ARRAY_BUFFER     0x8892
#define GL_STATIC_DRAW      0x88E4
#define GL_DYNAMIC_DRAW     0x88E8
#define GL_FRAGMENT_SHADER  0x8B30
#define GL_VERTEX_SHADER    0x8B31

void   glActiveTexture(GLenum texture);
void   glAttachShader(GLuint program, GLuint shader);
void   glBindBuffer(GLenum target, GLuint buffer);
void   glBindTexture(GLenum target, GLuint texture);
void   glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage);
void   glClear(GLbitfield mask);
void   glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
void   glCompileShader(GLuint shader);
GLuint glCreateProgram(void);
GLuint glCreateShader(GLenum type);
void   glDeleteShader(GLuint shader);
void   glDrawArrays(GLenum mode, GLint first, GLsizei count);
void   glEnableVertexAttribArray(GLuint index);
void   glGenBuffers(GLsizei n, GLuint* buffers);
void   glGenTextures(GLsizei n, GLuint* textures);
GLint  glGetAttribLocation(GLuint program, const GLchar* name);
GLint  glGetUniformLocation(GLuint program, const GLchar* name);
void   glLinkProgram(GLuint program);
void   glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length);
void   glTexImage2D(GLenum target, GLint level, GLint internalformat,
                    GLsizei width, GLsizei height, GLint border,
                    GLenum format, GLenum type, const void* pixels);
void   glTexParameteri(GLenum target, GLenum pname, GLint param);
void   glUniform1i(GLint location, GLint v0);
void   glUseProgram(GLuint program);
void   glVertexAttribPointer(GLuint index, GLint size, GLenum type,
                             GLboolean normalized, GLsizei stride, const void* pointer);
void   glViewport(GLint x, GLint y, GLsizei width, GLsizei height);

#ifdef __cplusplus
}
#endif

#endif /* XTEST_STUB_GLES2_GL2_H */
