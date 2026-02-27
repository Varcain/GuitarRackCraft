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

#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <memory>
#include <atomic>
#include <future>

namespace guitarrackcraft {

/**
 * X11Worker - Single dedicated thread for all X11 operations.
 * 
 * This class ensures all X11 operations (server, plugin UI instantiation,
 * idle calls, cleanup) happen on a single thread, eliminating the
 * xcb_xlib_threads_sequence_lost crash caused by multi-threaded X11 usage.
 * 
 * All operations are posted to a message queue and executed serially
 * on the worker thread.
 */
class X11Worker {
public:
    using Task = std::function<void()>;
    
    X11Worker();
    ~X11Worker();
    
    // Start the worker thread
    void start();
    
    // Stop the worker thread gracefully
    void stop();
    
    // Post a task to be executed on the worker thread (non-blocking)
    void post(Task task);
    
    // Post a task and wait for completion (blocking)
    void postAndWait(Task task);
    
    // Check if running
    bool isRunning() const { return running_.load(); }
    
    // Get the worker thread ID (for debugging)
    std::thread::id getThreadId() const { return threadId_; }

private:
    void run();
    
    std::thread thread_;
    std::thread::id threadId_;
    std::atomic<bool> running_{false};
    
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<Task> tasks_;
};

// Global X11 worker instance
X11Worker& getX11Worker();

} // namespace guitarrackcraft
