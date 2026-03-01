# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# This file is part of Guitar RackCraft.
#
# Guitar RackCraft is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Guitar RackCraft is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with Guitar RackCraft. If not, see <https://www.gnu.org/licenses/>.

# cmake/LV2Compat.cmake
# Generates LV2 compatibility redirect headers for old-style include paths.
# Replaces scripts/common/lv2_compat.sh.

# generate_lv2_compat_headers(<output_dir>)
function(generate_lv2_compat_headers DIR)
    # Bare <lv2.h> → lv2/core/lv2.h
    file(MAKE_DIRECTORY "${DIR}")
    file(WRITE "${DIR}/lv2.h"
        "/* compat: redirect bare <lv2.h> to lv2/core/lv2.h */\n"
        "#include \"lv2/core/lv2.h\"\n")

    # Old lv2plug.in paths
    file(MAKE_DIRECTORY "${DIR}/lv2/lv2plug.in/ns/lv2core")
    file(WRITE "${DIR}/lv2/lv2plug.in/ns/lv2core/lv2.h"
        "/* compat: redirect lv2plug.in path to lv2/core */\n"
        "#include \"lv2/core/lv2.h\"\n")

    file(MAKE_DIRECTORY "${DIR}/lv2/lv2plug.in/ns/extensions/ui")
    file(WRITE "${DIR}/lv2/lv2plug.in/ns/extensions/ui/ui.h"
        "/* compat: redirect lv2plug.in path to lv2/ui */\n"
        "#include \"lv2/ui/ui.h\"\n")

    # Old extension paths (atom, urid, time, etc.)
    set(_ext_pairs
        "atom/atom"   "atom/util"   "atom/forge"
        "urid/urid"   "time/time"   "buf-size/buf-size"
        "midi/midi"   "worker/worker"   "patch/patch"
        "state/state"   "options/options"
        "log/log"   "log/logger"
        "presets/presets"
        "dynmanifest/dynmanifest"
    )
    foreach(_ext IN LISTS _ext_pairs)
        get_filename_component(_ext_dir "${_ext}" DIRECTORY)
        file(MAKE_DIRECTORY "${DIR}/lv2/lv2plug.in/ns/ext/${_ext_dir}")
        file(WRITE "${DIR}/lv2/lv2plug.in/ns/ext/${_ext}.h"
            "/* compat */ #include \"lv2/${_ext}.h\"\n")
    endforeach()

    message(STATUS "Generated LV2 compat headers in ${DIR}")
endfunction()

# generate_faust_compat_header(<output_path>)
#   Creates the min/max using-declaration header for Faust-generated code.
function(generate_faust_compat_header OUTPUT_PATH)
    file(WRITE "${OUTPUT_PATH}"
        "#include <algorithm>\n"
        "#include <cmath>\n"
        "using std::min;\n"
        "using std::max;\n")
endfunction()

# generate_sigcpp_shim(<output_dir>)
#   Creates a minimal sigc++ shim (signal<void> + mem_fun) for trunk plugins.
function(generate_sigcpp_shim DIR)
    file(MAKE_DIRECTORY "${DIR}/sigc++/sigc++")

    file(WRITE "${DIR}/sigc++/sigc++.h"
        "#include \"sigc++/sigc++.h\"\n")

    file(WRITE "${DIR}/sigc++/sigc++/sigc++.h"
[=[/* Minimal sigc++ shim for Android — only signal<void> + mem_fun */
#pragma once
#include <functional>
#include <vector>
namespace sigc {
template<typename Signature> class signal;
template<> class signal<void> {
    std::vector<std::function<void()>> slots_;
public:
    void emit() { for (auto& s : slots_) s(); }
    void operator()() { emit(); }
    struct connection {};
    connection connect(std::function<void()> f) { slots_.push_back(f); return {}; }
};
template<typename R, typename T>
std::function<R()> mem_fun(T* obj, R(T::*f)()) {
    return [obj, f]() { (obj->*f)(); };
}
template<typename R, typename T, typename A1>
std::function<R(A1)> mem_fun(T* obj, R(T::*f)(A1)) {
    return [obj, f](A1 a) { (obj->*f)(a); };
}
template<typename Slot, typename... Args>
auto bind(Slot slot, Args... args) {
    return [slot, args...]() { slot(args...); };
}
} // namespace sigc
]=])
endfunction()

# generate_sndfile_stub(<output_path>)
function(generate_sndfile_stub OUTPUT_PATH)
    file(WRITE "${OUTPUT_PATH}"
[=[/* Stub sndfile for Android — looper works in-memory only */
#pragma once
#include <cstdio>
#define SFM_READ 0
#define SFM_WRITE 1
struct SF_INFO { int frames; int samplerate; int channels; int format; int sections; int seekable; };
typedef struct SNDFILE_tag SNDFILE;
static inline SNDFILE* sf_open(const char*, int, SF_INFO*) { return nullptr; }
static inline int sf_read_float(SNDFILE*, float*, int) { return 0; }
static inline int sf_write_float(SNDFILE*, const float*, int) { return 0; }
static inline void sf_write_sync(SNDFILE*) {}
static inline int sf_close(SNDFILE*) { return 0; }
]=])
endfunction()
