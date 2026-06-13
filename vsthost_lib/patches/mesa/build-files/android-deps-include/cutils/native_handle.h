/* vstpoc stub: Android libcutils native_handle (gralloc buffer handle). Matches
 * the platform layout; only the struct is needed by vk_android_native_buffer.h. */
#ifndef _VSTPOC_CUTILS_NATIVE_HANDLE_H
#define _VSTPOC_CUTILS_NATIVE_HANDLE_H
#ifdef __cplusplus
extern "C" {
#endif
typedef struct native_handle {
    int version;
    int numFds;
    int numInts;
    int data[0];
} native_handle_t;
typedef const native_handle_t *buffer_handle_t;
static inline native_handle_t *native_handle_create(int n, int m) { (void)n; (void)m; return 0; }
static inline int native_handle_delete(native_handle_t *h) { (void)h; return 0; }
static inline int native_handle_close(const native_handle_t *h) { (void)h; return 0; }
#ifdef __cplusplus
}
#endif
#endif
