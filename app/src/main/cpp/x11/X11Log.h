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

#pragma once

#ifdef __ANDROID__
#include <android/log.h>
#define X11_LOGI(tag, ...) __android_log_print(ANDROID_LOG_INFO, tag, __VA_ARGS__)
#define X11_LOGE(tag, ...) __android_log_print(ANDROID_LOG_ERROR, tag, __VA_ARGS__)
#define X11_LOGW(tag, ...) __android_log_print(ANDROID_LOG_WARN, tag, __VA_ARGS__)
#else
#include <cstdio>
#define X11_LOGI(tag, ...) do { fprintf(stderr, "[I/%s] ", tag); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#define X11_LOGE(tag, ...) do { fprintf(stderr, "[E/%s] ", tag); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#define X11_LOGW(tag, ...) do { fprintf(stderr, "[W/%s] ", tag); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#endif
