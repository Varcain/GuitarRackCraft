/* Desktop stub for <android/log.h> */
#ifndef XTEST_STUB_ANDROID_LOG_H
#define XTEST_STUB_ANDROID_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Subset of android_LogPriority used by the X server source. */
#define ANDROID_LOG_UNKNOWN 0
#define ANDROID_LOG_DEFAULT 1
#define ANDROID_LOG_VERBOSE 2
#define ANDROID_LOG_DEBUG   3
#define ANDROID_LOG_INFO    4
#define ANDROID_LOG_WARN    5
#define ANDROID_LOG_ERROR   6
#define ANDROID_LOG_FATAL   7
#define ANDROID_LOG_SILENT  8

/* Real implementation lives in stubs.cpp and vfprintf's to stderr so the
 * X server's own logs surface in the harness. */
int __android_log_print(int prio, const char* tag, const char* fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* XTEST_STUB_ANDROID_LOG_H */
