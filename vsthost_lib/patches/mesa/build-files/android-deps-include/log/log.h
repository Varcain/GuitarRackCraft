/* vstpoc stub: Android liblog log/log.h (private). Map to the public
 * android/log.h. Mesa links -llog for __android_log_print. */
#ifndef _VSTPOC_LOG_LOG_H
#define _VSTPOC_LOG_LOG_H
#include <android/log.h>
#ifndef LOG_TAG
#define LOG_TAG "MESA"
#endif
#define LOG_PRI(prio, tag, ...) __android_log_print((prio), (tag), __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define ALOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, __VA_ARGS__)
#define ALOGE_IF(c, ...) do { if (c) ALOGE(__VA_ARGS__); } while (0)
#endif
