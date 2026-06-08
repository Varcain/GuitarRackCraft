/* Desktop stub for <android/native_window.h> */
#ifndef XTEST_STUB_ANDROID_NATIVE_WINDOW_H
#define XTEST_STUB_ANDROID_NATIVE_WINDOW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque; never dereferenced by the harness (attachSurface is never called). */
typedef struct ANativeWindow ANativeWindow;

void ANativeWindow_release(ANativeWindow* window);
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow* window,
                                         int32_t width, int32_t height,
                                         int32_t format);

#ifdef __cplusplus
}
#endif

#endif /* XTEST_STUB_ANDROID_NATIVE_WINDOW_H */
