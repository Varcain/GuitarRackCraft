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

# =============================================================================
# cmake/targets/doubletracker.cmake — Build DoubleTracker LV2 plugin (DSP-only)
#
# Guitar double-track emulation built with faust2lv2 architecture.
# Faust DSP → C++ codegen (lv2.cpp arch) → host TTL generation → cross-compile.
# =============================================================================

set(_dt_src    "${THIRD_PARTY}/doubletracker.lv2")
set(_dt_assets "${ASSETS_DIR}/doubletracker.lv2")
set(_dt_build  "${CMAKE_BINARY_DIR}/doubletracker")
set(_dt_gen    "${_dt_build}/gen")

file(MAKE_DIRECTORY "${_dt_assets}" "${_dt_build}" "${_dt_gen}")

# ─── Phase 1: Generate manifest.ttl (configure-time) ──────────────────
# Static manifest matching faust2lv2's template (no dynamic manifest).
file(WRITE "${_dt_assets}/manifest.ttl"
"########## https://faustlv2.bitbucket.io/doubletracker ##########

@prefix doap: <http://usefulinc.com/ns/doap#> .
@prefix foaf: <http://xmlns.com/foaf/0.1/> .
@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .
@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .

<https://faustlv2.bitbucket.io/doubletracker>
    a lv2:Plugin ;
    lv2:binary <doubletracker.so> ;
    rdfs:seeAlso <doubletracker.ttl> .
")

# ─── Phase 2a: Faust codegen (host tool) ──────────────────────────────
# Generate C++ from Faust DSP using lv2.cpp architecture.
# The -i flag inlines all .dsp library imports AND faust C++ headers.
find_program(FAUST faust REQUIRED)

set(_dt_dsp "${_dt_src}/dsp/doubletracker.dsp")
set(_dt_cpp "${_dt_gen}/doubletracker.cpp")

add_custom_command(
    OUTPUT "${_dt_cpp}"
    COMMAND ${FAUST} -i
        -a /usr/share/faust/lv2.cpp
        -cn doubletracker
        -o "${_dt_cpp}"
        "${_dt_dsp}"
    DEPENDS "${_dt_dsp}"
    COMMENT "Generating doubletracker C++ from Faust DSP (lv2.cpp architecture)"
)
add_custom_target(doubletracker_faust DEPENDS "${_dt_cpp}")

# ─── Phase 2b: Generate doubletracker.ttl (host compile + run) ────────
# The lv2.cpp architecture includes main() which outputs the plugin's TTL
# when compiled as an executable and run.
find_program(HOST_CXX g++ c++ REQUIRED)

set(_dt_ttl_gen "${_dt_build}/doubletracker_ttlgen")
set(_dt_ttl     "${_dt_assets}/doubletracker.ttl")

# Write defines to a header file to avoid cmake/ninja/shell quoting hell.
file(WRITE "${_dt_gen}/doubletracker_defines.h"
    "#define PLUGIN_URI \"https://faustlv2.bitbucket.io/doubletracker\"\n"
    "#define FAUST_META 1\n"
    "#define FAUST_MIDICC 1\n"
    "#define FAUST_MTS 1\n"
    "#define FAUST_UI 0\n"
    "#define VOICE_CTRLS 1\n"
    "#define DLLEXT \".so\"\n"
)

add_custom_command(
    OUTPUT "${_dt_ttl}"
    COMMAND ${HOST_CXX} -std=c++11 -O2
        -include "${_dt_gen}/doubletracker_defines.h"
        "${_dt_cpp}" -o "${_dt_ttl_gen}"
    COMMAND ${_dt_ttl_gen} > "${_dt_ttl}"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${_dt_ttl_gen}"
    DEPENDS "${_dt_cpp}"
    COMMENT "Generating doubletracker.ttl (host compile + run)"
)
add_custom_target(doubletracker_ttl DEPENDS "${_dt_ttl}")

# ─── Phase 2c: Cross-compile for Android ──────────────────────────────
# The generated C++ needs boost/circular_buffer.hpp (header-only) and
# LV2 headers (via compat redirects). Faust headers are already inlined.

# Create isolated include directory with only boost from host system.
set(_dt_host_inc "${_dt_build}/host_include")
file(MAKE_DIRECTORY "${_dt_host_inc}")
if(EXISTS "/usr/include/boost" AND NOT EXISTS "${_dt_host_inc}/boost")
    file(CREATE_LINK "/usr/include/boost" "${_dt_host_inc}/boost" SYMBOLIC)
endif()

add_library(doubletracker_dsp SHARED "${_dt_cpp}")
add_dependencies(doubletracker_dsp doubletracker_faust)
lv2_set_dsp_properties(doubletracker_dsp "doubletracker" "${_dt_build}")

target_compile_definitions(doubletracker_dsp PRIVATE
    PLUGIN_URI="https://faustlv2.bitbucket.io/doubletracker"
    FAUST_META=1
    FAUST_MIDICC=1
    FAUST_MTS=1
    FAUST_UI=0
    VOICE_CTRLS=1
    DLLEXT=".so"
)

target_include_directories(doubletracker_dsp PRIVATE
    "${_dt_host_inc}"
    "${LV2_COMPAT_DIR}"
    "${LV2_INCLUDE}"
)

# ─── Phase 3: Sync to jniLibs (stamp-based) ───────────────────────────
set(_dt_stamp "${CMAKE_BINARY_DIR}/stamps/doubletracker_sync.stamp")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/stamps")

add_custom_command(
    OUTPUT "${_dt_stamp}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${JNILIBS_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_dt_build}/doubletracker.so" "${JNILIBS_DIR}/libdoubletracker.so"
    COMMAND ${NDK_STRIP} --strip-unneeded "${JNILIBS_DIR}/libdoubletracker.so"
    COMMAND ${CMAKE_COMMAND} -E touch "${_dt_stamp}"
    DEPENDS doubletracker_dsp
    COMMENT "Syncing doubletracker to jniLibs"
)
add_custom_target(doubletracker_sync DEPENDS "${_dt_stamp}")

add_custom_target(doubletracker_done DEPENDS doubletracker_sync doubletracker_ttl)
