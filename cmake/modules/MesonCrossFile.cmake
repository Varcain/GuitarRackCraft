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

# cmake/MesonCrossFile.cmake
# Generates Meson cross-compilation files for Android NDK builds.
# Requires AndroidNDK.cmake to be included first.

# generate_meson_cross_file(<output_path> <install_prefix> [SYSTEM <system>])
#   SYSTEM defaults to "android"; use "linux" for Mesa builds.
function(generate_meson_cross_file OUTPUT_PATH PREFIX)
    cmake_parse_arguments(ARG "" "SYSTEM" "" ${ARGN})
    if(NOT ARG_SYSTEM)
        set(ARG_SYSTEM "android")
    endif()

    get_filename_component(_dir "${OUTPUT_PATH}" DIRECTORY)
    file(MAKE_DIRECTORY "${_dir}")

    if(CCACHE_PROGRAM)
        set(_c_line   "c     = ['ccache', '${NDK_CC}']")
        set(_cxx_line "cpp   = ['ccache', '${NDK_CXX}']")
    else()
        set(_c_line   "c     = '${NDK_CC}'")
        set(_cxx_line "cpp   = '${NDK_CXX}'")
    endif()

    file(WRITE "${OUTPUT_PATH}"
"[binaries]
${_c_line}
${_cxx_line}
ar    = '${NDK_AR}'
strip = '${NDK_STRIP}'
ranlib = '${NDK_RANLIB}'
nm    = '${NDK_NM}'
ld    = '${NDK_LD}'
pkg-config = '/usr/bin/pkg-config'

[built-in options]
c_args   = ['-fPIC', '-DANDROID', '-D__ANDROID_API__=${NDK_API}', '-O2', '-I${PREFIX}/include']
cpp_args = ['-fPIC', '-DANDROID', '-D__ANDROID_API__=${NDK_API}', '-O2', '-I${PREFIX}/include']
c_link_args   = ['-L${PREFIX}/lib']
cpp_link_args = ['-L${PREFIX}/lib']

[properties]
pkg_config_libdir = '${PREFIX}/lib/pkgconfig:${PREFIX}/share/pkgconfig'
sys_root = '${PREFIX}'
ipc_rmid_deferred_release = false

[host_machine]
system     = '${ARG_SYSTEM}'
cpu_family = 'aarch64'
cpu        = 'aarch64'
endian     = 'little'
")
    message(STATUS "Generated Meson cross-file: ${OUTPUT_PATH}")
endfunction()

# generate_mesa_cross_file(<output_path> <sysroot>)
#   Special cross-file for Mesa: system='linux', adds -DMESA_FORCE_LINUX.
function(generate_mesa_cross_file OUTPUT_PATH SYSROOT)
    get_filename_component(_dir "${OUTPUT_PATH}" DIRECTORY)
    file(MAKE_DIRECTORY "${_dir}")

    if(CCACHE_PROGRAM)
        set(_c_line   "c     = ['ccache', '${NDK_CC}']")
        set(_cxx_line "cpp   = ['ccache', '${NDK_CXX}']")
    else()
        set(_c_line   "c     = '${NDK_CC}'")
        set(_cxx_line "cpp   = '${NDK_CXX}'")
    endif()

    file(WRITE "${OUTPUT_PATH}"
"[binaries]
${_c_line}
${_cxx_line}
ar    = '${NDK_AR}'
strip = '${NDK_STRIP}'
ranlib = '${NDK_RANLIB}'
nm    = '${NDK_NM}'
ld    = '${NDK_LD}'
pkg-config = '/usr/bin/pkg-config'

[built-in options]
c_args   = ['-fPIC', '-DMESA_FORCE_LINUX', '-O2', '-I${SYSROOT}/include']
cpp_args = ['-fPIC', '-DMESA_FORCE_LINUX', '-O2', '-I${SYSROOT}/include', '-std=c++17']
c_link_args   = ['-L${SYSROOT}/lib']
cpp_link_args = ['-L${SYSROOT}/lib']

[properties]
pkg_config_libdir = '${SYSROOT}/lib/pkgconfig:${SYSROOT}/share/pkgconfig'
sys_root = '${SYSROOT}'

[host_machine]
system     = 'linux'
cpu_family = 'aarch64'
cpu        = 'aarch64'
endian     = 'little'
")
    message(STATUS "Generated Mesa cross-file: ${OUTPUT_PATH}")
endfunction()
