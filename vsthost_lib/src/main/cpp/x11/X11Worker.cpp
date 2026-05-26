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

#include "X11Worker.h"
#include <android/log.h>

#define LOG_TAG "X11Worker"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace guitarrackcraft {

X11Worker::X11Worker() = default;

X11Worker::~X11Worker() {
    stop();
}

void X11Worker::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_.load()) {
        running_.store(true);
        thread_ = std::thread(&X11Worker::run, this);
        LOGI("X11Worker started");
    }
}

void X11Worker::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_.load()) return;
        running_.store(false);
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
        LOGI("X11Worker stopped");
    }
}

void X11Worker::post(Task task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_.load()) {
            LOGI("X11Worker not running, discarding task");
            return;
        }
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
}

void X11Worker::postAndWait(Task task) {
    std::promise<void> promise;
    std::future<void> future = promise.get_future();
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_.load()) {
            LOGI("X11Worker not running, task not executed");
            return;
        }
        tasks_.push([&promise, task]() {
            task();
            promise.set_value();
        });
    }
    cv_.notify_one();
    
    future.wait();
}

void X11Worker::run() {
    threadId_ = std::this_thread::get_id();
    LOGI("X11Worker thread started tid=%lu", 
         static_cast<unsigned long>(std::hash<std::thread::id>{}(threadId_)));
    
    while (running_.load()) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !tasks_.empty() || !running_.load(); });
            
            if (!running_.load() && tasks_.empty()) break;
            
            if (!tasks_.empty()) {
                task = std::move(tasks_.front());
                tasks_.pop();
            }
        }
        
        if (task) {
            try {
                task();
            } catch (...) {
                LOGE("X11Worker task exception caught");
            }
        }
    }
    
    LOGI("X11Worker thread exiting");
}

// Global instance
X11Worker& getX11Worker() {
    static X11Worker instance;
    return instance;
}

} // namespace guitarrackcraft
