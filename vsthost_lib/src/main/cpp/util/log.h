#pragma once

#include <android/log.h>

#ifndef VSTPOC_LOG_TAG
#define VSTPOC_LOG_TAG "vstpoc"
#endif

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  VSTPOC_LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  VSTPOC_LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, VSTPOC_LOG_TAG, __VA_ARGS__)
