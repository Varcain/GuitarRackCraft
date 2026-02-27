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
# cmake/targets/aidax.cmake — Build AIDA-X LV2 plugin (headless, DSP-only)
# =============================================================================

set(_aidax_src    "${THIRD_PARTY}/aidadsp-lv2")
set(_aidax_build  "${PROJECT_ROOT}/build/aidadsp")
set(_aidax_assets "${ASSETS_DIR}/aidadsp.lv2")

# ─── Phase 1: Copy TTL + modgui + rename to "headless" ──────────────────────
file(MAKE_DIRECTORY "${_aidax_assets}")

if(EXISTS "${_aidax_src}/rt-neural-generic/ttl/manifest.ttl")
    configure_file("${_aidax_src}/rt-neural-generic/ttl/manifest.ttl"
                   "${_aidax_assets}/manifest.ttl" COPYONLY)
endif()

if(EXISTS "${_aidax_src}/rt-neural-generic/ttl/rt-neural-generic.ttl")
    file(READ "${_aidax_src}/rt-neural-generic/ttl/rt-neural-generic.ttl" _aidax_ttl)
    string(REPLACE "doap:name \"AIDA-X\"" "doap:name \"AIDA-X (headless)\"" _aidax_ttl "${_aidax_ttl}")
    string(REPLACE "mod:label \"AIDA-X\"" "mod:label \"AIDA-X (headless)\"" _aidax_ttl "${_aidax_ttl}")
    file(WRITE "${_aidax_assets}/rt-neural-generic.ttl" "${_aidax_ttl}")
endif()

if(EXISTS "${_aidax_src}/rt-neural-generic/ttl/modgui.ttl")
    configure_file("${_aidax_src}/rt-neural-generic/ttl/modgui.ttl"
                   "${_aidax_assets}/modgui.ttl" COPYONLY)
endif()
if(IS_DIRECTORY "${_aidax_src}/rt-neural-generic/ttl/modgui")
    file(COPY "${_aidax_src}/rt-neural-generic/ttl/modgui/" DESTINATION "${_aidax_assets}/modgui/")
endif()

# ─── Phase 2: CMake cross-compile ────────────────────────────────────────────
set(_aidax_so_output "${_aidax_build}/rt-neural-generic/rt-neural-generic.so")

ExternalProject_Add(aidax_build
    SOURCE_DIR      "${_aidax_src}"
    BINARY_DIR      "${_aidax_build}"
    INSTALL_DIR     "${_aidax_assets}"
    CMAKE_ARGS
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        -DANDROID_ABI=${ANDROID_ABI}
        -DANDROID_PLATFORM=${ANDROID_PLATFORM}
        -DCMAKE_BUILD_TYPE=Release
        -DRTNEURAL_XSIMD=ON
        -DCMAKE_CXX_FLAGS_RELEASE=-O3\ -DNDEBUG\ -funroll-loops
        -DCMAKE_SHARED_LINKER_FLAGS_RELEASE=-Wl,--as-needed\ -Wl,--strip-all
        -DCMAKE_PREFIX_PATH=${LV2_PREFIX}
        -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH
        ${NDK_CCACHE_CMAKE_ARGS}
    BUILD_COMMAND   ${CMAKE_COMMAND} --build <BINARY_DIR> -j${NJOBS}
    INSTALL_COMMAND ""
    DEPENDS         lv2_libs
    BUILD_BYPRODUCTS "${_aidax_so_output}"
    LOG_CONFIGURE TRUE
    LOG_BUILD TRUE
)

watch_external_sources(aidax_build DIRECTORIES "${_aidax_src}/rt-neural-generic")

# ─── Phase 3: Sync to assets + jniLibs ──────────────────────────────────────
lv2_sync_to_jnilibs(aidax_sync "${_aidax_build}/rt-neural-generic" "${_aidax_so_output}")

add_custom_target(aidax_done DEPENDS aidax_sync)
