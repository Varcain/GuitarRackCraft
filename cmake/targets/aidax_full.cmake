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
# cmake/targets/aidax_full.cmake — Build full AIDA-X (DPF with EGL+GLES2 UI)
# =============================================================================

set(_aidax_full_src  "${THIRD_PARTY}/AIDA-X")
set(_aidax_full_build "${PROJECT_ROOT}/build/aidax_full")
set(_aidax_full_assets "${ASSETS_DIR}/AIDA-X.lv2")
set(_dsp_only_cmake  "${CMAKE_CURRENT_LIST_DIR}/aidax_dsp")
set(_patches_dir     "${THIRD_PARTY}/patches/AIDA-X")
set(_gl_shim_dir     "${_patches_dir}")

# ─── Prepare source tree ─────────────────────────────────────────────────────
set(_pugl_extra "${_aidax_full_src}/modules/dpf/dgl/src/pugl-extra")
if(EXISTS "${_patches_dir}/x11_egl.c")
    configure_file("${_patches_dir}/x11_egl.c" "${_pugl_extra}/x11_egl.c" COPYONLY)
endif()

if(EXISTS "${_patches_dir}/0001-dpf-egl-gles2-android-backend.patch")
    execute_process(COMMAND patch -p1 --forward --no-backup-if-mismatch INPUT_FILE "${_patches_dir}/0001-dpf-egl-gles2-android-backend.patch" WORKING_DIRECTORY "${_aidax_full_src}" RESULT_VARIABLE _patch_result ERROR_QUIET OUTPUT_QUIET)
endif()

set(_fb_cpp "${_aidax_full_src}/modules/dpf/distrho/extra/FileBrowserDialogImpl.cpp")
if(EXISTS "${_fb_cpp}")
    file(READ "${_fb_cpp}" _fb_content)
    if(NOT _fb_content MATCHES "__ANDROID__")
        string(REPLACE "x11display = XOpenDisplay(nullptr);" "#ifndef __ANDROID__\n        x11display = XOpenDisplay(nullptr);\n#endif" _fb_content "${_fb_content}")
        file(WRITE "${_fb_cpp}" "${_fb_content}")
    endif()
endif()

# ─── Phase 1: Generate TTL via native host build ─────────────────────────────
set(_native_build "${_aidax_full_build}/native")
set(_ttl_dir "${_aidax_full_build}/ttl")

ExternalProject_Add(aidax_full_native
    SOURCE_DIR "${_dsp_only_cmake}" BINARY_DIR "${_native_build}" INSTALL_DIR "${_ttl_dir}"
    CMAKE_ARGS -DCMAKE_C_COMPILER=cc -DCMAKE_CXX_COMPILER=c++ -DCMAKE_BUILD_TYPE=Release -DAIDAX_SRC=${_aidax_full_src} -DRTNEURAL_XSIMD=ON ${NDK_CCACHE_CMAKE_ARGS} CMAKE_CACHE_ARGS -DCMAKE_TOOLCHAIN_FILE:FILEPATH=
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> -j${NJOBS}
    INSTALL_COMMAND bash -c "mkdir -p '${_ttl_dir}' && cp <BINARY_DIR>/bin/AIDA-X.lv2/*.ttl '${_ttl_dir}/'"
)

# ─── Phase 2: CMake cross-compile for ARM64 ──────────────────────────────────
set(_cross_build "${_aidax_full_build}/cross")

# Generate configure script from template
set(_aidax_full_configure_script "${_aidax_full_build}/configure.sh")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/aidax_full_configure.sh.in"
    "${_aidax_full_configure_script}"
    @ONLY
)

ExternalProject_Add(aidax_full_cross
    SOURCE_DIR "${_aidax_full_src}" BINARY_DIR "${_cross_build}" CONFIGURE_COMMAND bash "${_aidax_full_configure_script}"
    BUILD_COMMAND bash -c "cmake --build '${_cross_build}' --target AIDA-X-lv2 -j${NJOBS} 2>&1 | grep -v lv2_ttl_generator || true"
    INSTALL_COMMAND bash -c "cmake --build '${_cross_build}' --target AIDA-X-lv2-ui -j${NJOBS} 2>&1 | grep -v lv2_ttl_generator || true"
    DEPENDS aidax_full_native x11_sysroot lv2_libs
)

watch_external_sources(aidax_full_native DIRECTORIES "${_aidax_full_src}/src")
watch_external_sources(aidax_full_cross  DIRECTORIES "${_aidax_full_src}/src" "${_aidax_full_src}/modules/dpf/dgl/src" "${_aidax_full_src}/modules/dpf/distrho")

# ─── Phase 3: Sync & Patch ───────────────────────────────────────────────────
# Generate sync script from template
set(_aidax_full_sync_script "${_aidax_full_build}/sync.sh")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/aidax_full_sync.sh.in"
    "${_aidax_full_sync_script}"
    @ONLY
)

set(_aidax_full_sync_stamp "${CMAKE_BINARY_DIR}/stamps/aidax_full_sync.stamp")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/stamps")
add_custom_command(
    OUTPUT "${_aidax_full_sync_stamp}"
    COMMAND bash "${_aidax_full_sync_script}"
    COMMAND ${CMAKE_COMMAND} -E touch "${_aidax_full_sync_stamp}"
    DEPENDS aidax_full_cross
    COMMENT "Syncing AIDA-X (full) to assets"
)
add_custom_target(aidax_full_sync DEPENDS "${_aidax_full_sync_stamp}")

# aidax_full_sync.sh.in now copies .so directly to jniLibs (no assets duplication)
add_custom_target(aidax_full_done DEPENDS aidax_full_sync)
