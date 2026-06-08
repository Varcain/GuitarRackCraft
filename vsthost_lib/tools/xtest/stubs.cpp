/* Desktop no-op stubs for the Android/EGL/GLES/JNI symbols the X server
 * references but never actually executes in the differential test harness
 * (attachSurface — the only caller of the EGL/GLES/ANativeWindow path — is
 * never invoked). __android_log_print is the one REAL implementation: it
 * forwards the X server's own logs to stderr. */

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <cstdarg>
#include <cstdio>

extern "C" {

/* ---- android/log.h : keep real, surfaces X server logs ---- */
int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
    const char* lvl = "?";
    switch (prio) {
        case ANDROID_LOG_VERBOSE: lvl = "V"; break;
        case ANDROID_LOG_DEBUG:   lvl = "D"; break;
        case ANDROID_LOG_INFO:    lvl = "I"; break;
        case ANDROID_LOG_WARN:    lvl = "W"; break;
        case ANDROID_LOG_ERROR:   lvl = "E"; break;
        case ANDROID_LOG_FATAL:   lvl = "F"; break;
        default: break;
    }
    fprintf(stderr, "[%s/%s] ", lvl, tag ? tag : "");
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    return n;
}

/* ---- android/native_window.h ---- */
void ANativeWindow_release(ANativeWindow*) {}
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow*, int32_t, int32_t, int32_t) { return 0; }

/* ---- android/native_window_jni.h ---- */
ANativeWindow* ANativeWindow_fromSurface(JNIEnv*, jobject) { return nullptr; }

/* ---- EGL/egl.h ---- */
EGLDisplay eglGetDisplay(EGLNativeDisplayType) { return EGL_NO_DISPLAY; }
EGLBoolean eglInitialize(EGLDisplay, EGLint*, EGLint*) { return EGL_FALSE; }
EGLBoolean eglTerminate(EGLDisplay) { return EGL_FALSE; }
EGLBoolean eglChooseConfig(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint* num_config) {
    if (num_config) *num_config = 0;
    return EGL_FALSE;
}
EGLSurface eglCreateWindowSurface(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint*) {
    return EGL_NO_SURFACE;
}
EGLBoolean eglDestroySurface(EGLDisplay, EGLSurface) { return EGL_FALSE; }
EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint*) { return EGL_NO_CONTEXT; }
EGLBoolean eglDestroyContext(EGLDisplay, EGLContext) { return EGL_FALSE; }
EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext) { return EGL_FALSE; }
EGLBoolean eglSwapBuffers(EGLDisplay, EGLSurface) { return EGL_FALSE; }

/* ---- GLES2/gl2.h ---- */
void   glActiveTexture(GLenum) {}
void   glAttachShader(GLuint, GLuint) {}
void   glBindBuffer(GLenum, GLuint) {}
void   glBindTexture(GLenum, GLuint) {}
void   glBufferData(GLenum, GLsizeiptr, const void*, GLenum) {}
void   glClear(GLbitfield) {}
void   glClearColor(GLclampf, GLclampf, GLclampf, GLclampf) {}
void   glCompileShader(GLuint) {}
GLuint glCreateProgram(void) { return 0; }
GLuint glCreateShader(GLenum) { return 0; }
void   glDeleteShader(GLuint) {}
void   glDrawArrays(GLenum, GLint, GLsizei) {}
void   glEnableVertexAttribArray(GLuint) {}
void   glGenBuffers(GLsizei, GLuint* buffers) { if (buffers) *buffers = 0; }
void   glGenTextures(GLsizei, GLuint* textures) { if (textures) *textures = 0; }
GLint  glGetAttribLocation(GLuint, const GLchar*) { return -1; }
GLint  glGetUniformLocation(GLuint, const GLchar*) { return -1; }
void   glLinkProgram(GLuint) {}
void   glShaderSource(GLuint, GLsizei, const GLchar* const*, const GLint*) {}
void   glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) {}
void   glTexParameteri(GLenum, GLenum, GLint) {}
void   glUniform1i(GLint, GLint) {}
void   glUseProgram(GLuint) {}
void   glVertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) {}
void   glViewport(GLint, GLint, GLsizei, GLsizei) {}

} // extern "C"

/* ---- ../plugin/PluginUIGuard.h : C++ (namespaced) ---- */
namespace guitarrackcraft {
bool isCreatingPluginUI() { return false; }
void setCreatingPluginUI(bool) {}
bool isCreatingPluginUIForDisplay(int) { return false; }
} // namespace guitarrackcraft
