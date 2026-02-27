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
# cmake/modules/ExternalBuild.cmake — Helpers for Meson and Autotools projects
# =============================================================================

# ─── Add Meson Project ───────────────────────────────────────────────────────
function(add_meson_project TARGET_NAME)
    cmake_parse_arguments(ARG "" "SOURCE_DIR;BINARY_DIR;INSTALL_DIR;CROSS_FILE" "DEPENDS;MESON_ARGS;PKG_CONFIG_PATH;EXTERNAL_PROJECT_ARGS" ${ARGN})

    set(_pkg_config_path "${ARG_PKG_CONFIG_PATH}")
    if(NOT _pkg_config_path)
        set(_pkg_config_path "${ARG_INSTALL_DIR}/lib/pkgconfig:${ARG_INSTALL_DIR}/share/pkgconfig")
    endif()

    ExternalProject_Add(${TARGET_NAME}
        SOURCE_DIR      "${ARG_SOURCE_DIR}"
        BINARY_DIR      "${ARG_BINARY_DIR}"
        INSTALL_DIR     "${ARG_INSTALL_DIR}"
        CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env "PKG_CONFIG_PATH=${_pkg_config_path}"
            meson setup <BINARY_DIR> <SOURCE_DIR>
                --prefix=<INSTALL_DIR>
                --cross-file ${ARG_CROSS_FILE}
                -Ddefault_library=static
                ${ARG_MESON_ARGS}
        BUILD_COMMAND     ninja -C <BINARY_DIR> -j${NJOBS}
        INSTALL_COMMAND   ninja -C <BINARY_DIR> install
        DEPENDS           ${ARG_DEPENDS}
        ${ARG_EXTERNAL_PROJECT_ARGS}
        LOG_CONFIGURE TRUE
        LOG_BUILD TRUE
    )
endfunction()

# ─── Add Autotools Project ───────────────────────────────────────────────────
function(add_autotools_project TARGET_NAME)
    cmake_parse_arguments(ARG "" "SOURCE_DIR;BINARY_DIR;INSTALL_DIR" "DEPENDS;CONFIGURE_ARGS;PKG_CONFIG_PATH;ENV;EXTERNAL_PROJECT_ARGS" ${ARGN})

    set(_pkg_config_path "${ARG_PKG_CONFIG_PATH}")
    if(NOT _pkg_config_path)
        set(_pkg_config_path "${ARG_INSTALL_DIR}/lib/pkgconfig:${ARG_INSTALL_DIR}/share/pkgconfig")
    endif()

    set(_env ${NDK_ENV_CMD}
        "PKG_CONFIG_PATH=${_pkg_config_path}"
        "PKG_CONFIG_LIBDIR=${ARG_INSTALL_DIR}/lib/pkgconfig"
        "PKG_CONFIG_SYSROOT_DIR="
        ${ARG_ENV}
    )

    ExternalProject_Add(${TARGET_NAME}
        SOURCE_DIR      "${ARG_SOURCE_DIR}"
        BINARY_DIR      "${ARG_BINARY_DIR}"
        INSTALL_DIR     "${ARG_INSTALL_DIR}"
        CONFIGURE_COMMAND ${_env}
            <SOURCE_DIR>/configure
                --host=${NDK_HOST} --prefix=<INSTALL_DIR>
                ${ARG_CONFIGURE_ARGS}
        BUILD_COMMAND     ${_env} make -j${NJOBS}
        INSTALL_COMMAND   ${_env} make install
        DEPENDS           ${ARG_DEPENDS}
        ${ARG_EXTERNAL_PROJECT_ARGS}
        LOG_CONFIGURE TRUE
        LOG_BUILD TRUE
    )
endfunction()

# ─── Helper to write pkg-config files ────────────────────────────────────────
function(write_pkg_config)
    cmake_parse_arguments(ARG "" "OUTPUT;NAME;DESCRIPTION;VERSION;PREFIX" "LIBS;CFLAGS" ${ARGN})
    
    set(_content "prefix=${ARG_PREFIX}\n")
    string(APPEND _content "libdir=\${prefix}/lib\n")
    string(APPEND _content "includedir=\${prefix}/include\n\n")
    string(APPEND _content "Name: ${ARG_NAME}\n")
    string(APPEND _content "Description: ${ARG_DESCRIPTION}\n")
    string(APPEND _content "Version: ${ARG_VERSION}\n")
    
    set(_libs_str "")
    foreach(lib IN LISTS ARG_LIBS)
        set(_libs_str "${_libs_str} ${lib}")
    endforeach()
    if(_libs_str)
        string(APPEND _content "Libs: -L\${libdir}${_libs_str}\n")
    endif()

    set(_cflags_str "")
    foreach(flag IN LISTS ARG_CFLAGS)
        set(_cflags_str "${_cflags_str} ${flag}")
    endforeach()
    string(APPEND _content "Cflags: -I\${includedir}${_cflags_str}\n")

    file(WRITE "${ARG_OUTPUT}" "${_content}")
endfunction()
