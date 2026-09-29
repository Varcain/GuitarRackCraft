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
# cmake/targets/lv2_libs.cmake — Build LV2 static libraries via Meson/Waf
# =============================================================================

set(_lv2_cross "${LV2_BUILD_DIR}/android_cross.txt")
set(_lv2_pkg   "${LV2_PREFIX}/lib/pkgconfig")

# ─── LV2 headers (header-only install + pkg-config) ──────────────────────────
set(_lv2_headers_stamp "${LV2_BUILD_DIR}/lv2_headers_gen.stamp")
add_custom_command(
    OUTPUT "${_lv2_headers_stamp}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${THIRD_PARTY}/lv2/include/lv2" "${LV2_PREFIX}/include/lv2"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_lv2_pkg}"
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT=${_lv2_pkg}/lv2.pc -DPREFIX=${LV2_PREFIX}
        -P "${CMAKE_BINARY_DIR}/scripts/WriteLV2PC.cmake"
    COMMAND ${CMAKE_COMMAND} -E touch "${_lv2_headers_stamp}"
    COMMENT "Installing LV2 headers + lv2.pc"
)
add_custom_target(lv2_headers_gen DEPENDS "${_lv2_headers_stamp}")

# We use the existing script but could use write_pkg_config if we wanted to change it to a function call here.
# Actually, let's use the function for consistency.
file(WRITE "${CMAKE_BINARY_DIR}/scripts/WriteLV2PC.cmake" "
include(\"${PROJECT_ROOT}/cmake/modules/ExternalBuild.cmake\")
write_pkg_config(OUTPUT \"\${OUTPUT}\" NAME LV2 DESCRIPTION \"LV2 plugin specification\" VERSION 1.18.4 PREFIX \"\${PREFIX}\")
")

# ─── serd (RDF serialization, Meson) ─────────────────────────────────────────
add_meson_project(serd
    SOURCE_DIR  "${THIRD_PARTY}/serd"
    BINARY_DIR  "${LV2_BUILD_DIR}/serd"
    INSTALL_DIR "${LV2_PREFIX}"
    CROSS_FILE  ${_lv2_cross}
    DEPENDS     lv2_headers_gen
    MESON_ARGS  -Dtests=disabled -Dtools=disabled -Ddocs=disabled
)

# ─── zix (data structures, Meson) ────────────────────────────────────────────
add_meson_project(zix
    SOURCE_DIR  "${THIRD_PARTY}/zix"
    BINARY_DIR  "${LV2_BUILD_DIR}/zix"
    INSTALL_DIR "${LV2_PREFIX}"
    CROSS_FILE  ${_lv2_cross}
    DEPENDS     lv2_headers_gen
    MESON_ARGS  -Dtests=disabled -Dbenchmarks=disabled -Ddocs=disabled
)

# ─── sord (RDF triple store, Meson — needs dependency patching) ──────────────
set(_sord_patch_script "${LV2_BUILD_DIR}/patch_sord.py")
file(WRITE "${_sord_patch_script}"
[=[
import re, sys, os
meson_file = os.path.join(sys.argv[1], 'meson.build')
prefix = sys.argv[2]
with open(meson_file, 'r') as f:
    content = f.read()
if 'required: false' in content and 'declare_dependency' in content:
    sys.exit(0)
lines = content.split('\n')
result = []
for line in lines:
    if 'zix-0' in line and 'dependency' in line and 'required: false' not in line:
        result.append("zix_dep = dependency('zix-0', required: false)")
        result.append("if not zix_dep.found()")
        result.append(f"  zix_dep = declare_dependency(include_directories: include_directories('{prefix}/include/zix-0'), link_args: ['-L{prefix}/lib', '-lzix-0'])")
        result.append("endif")
    elif 'serd-0' in line and 'dependency' in line and 'required: false' not in line:
        result.append("serd_dep = dependency('serd-0', required: false)")
        result.append("if not serd_dep.found()")
        result.append(f"  serd_dep = declare_dependency(include_directories: include_directories('{prefix}/include/serd-0'), link_args: ['-L{prefix}/lib', '-lserd-0'])")
        result.append("endif")
    else:
        result.append(line)
with open(meson_file, 'w') as f:
    f.write('\n'.join(result))
]=])

add_meson_project(sord
    SOURCE_DIR  "${THIRD_PARTY}/sord"
    BINARY_DIR  "${LV2_BUILD_DIR}/sord"
    INSTALL_DIR "${LV2_PREFIX}"
    CROSS_FILE  ${_lv2_cross}
    DEPENDS     serd zix
    MESON_ARGS  -Dtests=disabled -Dtools=disabled -Ddocs=disabled
)

ExternalProject_Add_Step(sord patch_meson
    COMMAND ${CMAKE_COMMAND} -E env python3 "${_sord_patch_script}" <SOURCE_DIR> "${LV2_PREFIX}"
    DEPENDEES download
    DEPENDERS configure
)

# ─── sratom (RDF atom serialization, Meson) ──────────────────────────────────
add_meson_project(sratom
    SOURCE_DIR  "${THIRD_PARTY}/sratom"
    BINARY_DIR  "${LV2_BUILD_DIR}/sratom"
    INSTALL_DIR "${LV2_PREFIX}"
    CROSS_FILE  ${_lv2_cross}
    DEPENDS     serd sord
    MESON_ARGS  -Ddocs=disabled
)

# ─── lilv (LV2 host library, Waf) ────────────────────────────────────────────
ExternalProject_Add(lilv
    SOURCE_DIR      "${THIRD_PARTY}/lilv"
    INSTALL_DIR     "${LV2_PREFIX}"
    # 3rd_party/patches/lilv (also applied by build.sh; skipped when present):
    # waflib's compiler detection honouring CC/CXX, and the Android dlopen fallback.
    PATCH_COMMAND   bash "${PROJECT_ROOT}/scripts/apply-patches.sh"
                        "${THIRD_PARTY}/patches/lilv/0001-android-dlopen-ext-fd-fallback.patch" <SOURCE_DIR>
            COMMAND bash "${PROJECT_ROOT}/scripts/apply-patches.sh"
                        "${THIRD_PARTY}/patches/lilv/waflib/0001-android-ndk-compiler-detection.patch" <SOURCE_DIR>/waflib
    CONFIGURE_COMMAND ${NDK_ENV_CMD} "SERD_DIR=${LV2_PREFIX}" "SORD_DIR=${LV2_PREFIX}" "ZIX_DIR=${LV2_PREFIX}" "CFLAGS=-fPIC -DANDROID -Wno-error=implicit-function-declaration" "CXXFLAGS=-fPIC -DANDROID" "PKG_CONFIG_PATH=${_lv2_pkg}" python3 waf configure --prefix=<INSTALL_DIR> --static --no-utils
    BUILD_COMMAND ${NDK_ENV_CMD} "PKG_CONFIG_PATH=${_lv2_pkg}" python3 waf build
    INSTALL_COMMAND ${NDK_ENV_CMD} python3 waf install
    BUILD_IN_SOURCE TRUE
    DEPENDS         serd zix sord sratom
    INSTALL_BYPRODUCTS "${LV2_PREFIX}/lib/liblilv-0.a"
    LOG_CONFIGURE TRUE
    LOG_BUILD TRUE
)
set_property(TARGET lilv PROPERTY EP_STEP_TARGETS build)

# Incremental rebuilds
watch_external_sources(serd   DIRECTORIES "${THIRD_PARTY}/serd/src")
watch_external_sources(zix    DIRECTORIES "${THIRD_PARTY}/zix/src")
watch_external_sources(sord   DIRECTORIES "${THIRD_PARTY}/sord/src")
watch_external_sources(sratom DIRECTORIES "${THIRD_PARTY}/sratom/src")
watch_external_sources(lilv   DIRECTORIES "${THIRD_PARTY}/lilv/src")

add_custom_target(lv2_libs DEPENDS lilv)
