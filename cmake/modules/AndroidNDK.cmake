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

# cmake/AndroidNDK.cmake
# Extracts NDK tool paths for use by ExternalProject_Add targets.
# Requires the Android NDK CMake toolchain to be active (CMAKE_ANDROID_NDK set).

if(NOT CMAKE_ANDROID_NDK)
    message(FATAL_ERROR
        "CMAKE_ANDROID_NDK not set. Use:\n"
        "  cmake -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake\n"
        "or set ANDROID_NDK / ANDROID_HOME environment variable.")
endif()

set(NDK_TOOLCHAIN "${CMAKE_ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64")
set(NDK_API ${ANDROID_NATIVE_API_LEVEL})

# Compiler / tool paths (for Meson, Autotools, Waf, manual compilation)
set(NDK_CC  "${NDK_TOOLCHAIN}/bin/aarch64-linux-android${NDK_API}-clang")
set(NDK_CXX "${NDK_TOOLCHAIN}/bin/aarch64-linux-android${NDK_API}-clang++")
set(NDK_AR     "${NDK_TOOLCHAIN}/bin/llvm-ar")
set(NDK_STRIP  "${NDK_TOOLCHAIN}/bin/llvm-strip")
set(NDK_RANLIB "${NDK_TOOLCHAIN}/bin/llvm-ranlib")
set(NDK_NM     "${NDK_TOOLCHAIN}/bin/llvm-nm")
set(NDK_LD     "${NDK_TOOLCHAIN}/bin/ld.lld")
set(NDK_AS     "${NDK_CC}")
set(NDK_HOST   "aarch64-linux-android")

# Common compiler flags (matches scripts/common/ndk_setup.sh)
# NDK_CFLAGS_STR: space-separated string for ExternalProject env vars
# NDK_CFLAGS: CMake list for target_compile_options()
set(NDK_CFLAGS_STR "-fPIC -DANDROID -D__ANDROID_API__=${NDK_API} -O2")
set(NDK_CXXFLAGS_STR "${NDK_CFLAGS_STR} -std=c++17")
set(NDK_LDFLAGS    "")
separate_arguments(NDK_CFLAGS UNIX_COMMAND "${NDK_CFLAGS_STR}")
separate_arguments(NDK_CXXFLAGS UNIX_COMMAND "${NDK_CXXFLAGS_STR}")

# NDK sysroot paths
set(NDK_SYSROOT "${NDK_TOOLCHAIN}/sysroot")
set(NDK_LIBDIR  "${NDK_SYSROOT}/usr/lib/aarch64-linux-android/${NDK_API}")

# Parallel jobs
include(ProcessorCount)
ProcessorCount(NJOBS)
if(NJOBS EQUAL 0)
    set(NJOBS 4)
endif()

# ─── ccache support ──────────────────────────────────────────────────────────
find_program(CCACHE_PROGRAM ccache)
set(NDK_CCACHE_CMAKE_ARGS "")
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(NDK_CCACHE_CMAKE_ARGS
        -DCMAKE_C_COMPILER_LAUNCHER=${CCACHE_PROGRAM}
        -DCMAKE_CXX_COMPILER_LAUNCHER=${CCACHE_PROGRAM}
    )
    message(STATUS "ccache found: ${CCACHE_PROGRAM} — wrapping NDK compilers")
endif()

# Reusable env-var prefix for cmake -E env (used by ExternalProject commands)
if(CCACHE_PROGRAM)
    set(NDK_ENV_CMD ${CMAKE_COMMAND} -E env
        "CC=${CCACHE_PROGRAM} ${NDK_CC}"
        "CXX=${CCACHE_PROGRAM} ${NDK_CXX}"
        "AR=${NDK_AR}"
        "STRIP=${NDK_STRIP}"
        "RANLIB=${NDK_RANLIB}"
        "NM=${NDK_NM}"
        "LD=${NDK_LD}"
        "AS=${NDK_CC}"
        "PATH=${NDK_TOOLCHAIN}/bin:$ENV{PATH}"
    )
else()
    set(NDK_ENV_CMD ${CMAKE_COMMAND} -E env
        "CC=${NDK_CC}"
        "CXX=${NDK_CXX}"
        "AR=${NDK_AR}"
        "STRIP=${NDK_STRIP}"
        "RANLIB=${NDK_RANLIB}"
        "NM=${NDK_NM}"
        "LD=${NDK_LD}"
        "AS=${NDK_CC}"
        "PATH=${NDK_TOOLCHAIN}/bin:$ENV{PATH}"
    )
endif()
