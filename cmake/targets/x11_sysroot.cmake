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
# cmake/targets/x11_sysroot.cmake — Build the X11/Cairo dependency chain
# =============================================================================

set(_x11_dir    "${THIRD_PARTY}/x11")
set(_x11_cross  "${X11_BUILD_DIR}/android_cross.txt")
set(_x11_pkg    "${X11_SYSROOT}/lib/pkgconfig:${X11_SYSROOT}/share/pkgconfig")

# Helper: ensure autoreconf
set(_ensure_autotools_script "${X11_BUILD_DIR}/ensure_autotools.sh")
grc_write_if_changed("${_ensure_autotools_script}" "#!/bin/bash\ndir=\"$1\"\nif [ ! -f \"$dir/configure\" ]; then cd \"$dir\" && ACLOCAL_PATH=\"${X11_SYSROOT}/share/aclocal\" autoreconf -fi; fi\n")

# ─── 0. util-macros ─────────────────────────────────────────────────────────
ExternalProject_Add(util_macros
    SOURCE_DIR      "${_x11_dir}/util-macros"
    BINARY_DIR      "${X11_BUILD_DIR}/util-macros"
    INSTALL_DIR     "${X11_SYSROOT}"
    CONFIGURE_COMMAND bash "${_ensure_autotools_script}" <SOURCE_DIR> COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR>
    BUILD_COMMAND make -j${NJOBS} INSTALL_COMMAND make install
    LOG_CONFIGURE TRUE LOG_BUILD TRUE
)

# ─── 1. xorgproto ───────────────────────────────────────────────────────────
add_meson_project(xorgproto
    SOURCE_DIR "${_x11_dir}/xorgproto" BINARY_DIR "${X11_BUILD_DIR}/xorgproto" INSTALL_DIR "${X11_SYSROOT}"
    CROSS_FILE ${_x11_cross} DEPENDS util_macros MESON_ARGS -Dlegacy=true
)

# ─── 1b. xtrans ─────────────────────────────────────────────────────────────
add_autotools_project(xtrans
    SOURCE_DIR "${_x11_dir}/xtrans" BINARY_DIR "${X11_BUILD_DIR}/xtrans" INSTALL_DIR "${X11_SYSROOT}"
    DEPENDS xorgproto CONFIGURE_ARGS --host=${NDK_HOST} "CFLAGS=${NDK_CFLAGS_STR} -I${X11_SYSROOT}/include" "LDFLAGS=-L${X11_SYSROOT}/lib"
)
ExternalProject_Add_Step(xtrans autoreconf COMMAND bash "${_ensure_autotools_script}" <SOURCE_DIR> DEPENDEES download DEPENDERS configure)

# ─── 2. libXau ──────────────────────────────────────────────────────────────
add_autotools_project(libXau
    SOURCE_DIR "${_x11_dir}/libXau" BINARY_DIR "${X11_BUILD_DIR}/libXau" INSTALL_DIR "${X11_SYSROOT}"
    DEPENDS xorgproto CONFIGURE_ARGS --enable-shared --disable-static "CFLAGS=${NDK_CFLAGS_STR} -I${X11_SYSROOT}/include" "LDFLAGS=-L${X11_SYSROOT}/lib"
    EXTERNAL_PROJECT_ARGS INSTALL_BYPRODUCTS "${X11_SYSROOT}/lib/libXau.so"
)
ExternalProject_Add_Step(libXau autoreconf COMMAND bash "${_ensure_autotools_script}" <SOURCE_DIR> DEPENDEES download DEPENDERS configure)

# ─── 3. xcb-proto ────────────────────────────────────────────────────────────
add_autotools_project(xcb_proto
    SOURCE_DIR "${_x11_dir}/xcb-proto" BINARY_DIR "${X11_BUILD_DIR}/xcb-proto" INSTALL_DIR "${X11_SYSROOT}"
    DEPENDS util_macros
)
ExternalProject_Add_Step(xcb_proto autoreconf COMMAND bash "${_ensure_autotools_script}" <SOURCE_DIR> DEPENDEES download DEPENDERS configure)

# ─── 3b. libxcb ─────────────────────────────────────────────────────────────
add_autotools_project(libxcb
    SOURCE_DIR "${_x11_dir}/libxcb" BINARY_DIR "${X11_BUILD_DIR}/libxcb" INSTALL_DIR "${X11_SYSROOT}"
    DEPENDS xcb_proto libXau CONFIGURE_ARGS --enable-shared --disable-static --disable-devel-docs --without-doxygen "CFLAGS=${NDK_CFLAGS_STR} -I${X11_SYSROOT}/include" "LDFLAGS=-L${X11_SYSROOT}/lib" "LIBS=-lXau" ENV "PYTHON=python3"
    EXTERNAL_PROJECT_ARGS INSTALL_BYPRODUCTS "${X11_SYSROOT}/lib/libxcb.so"
)
ExternalProject_Add_Step(libxcb autoreconf COMMAND bash "${_ensure_autotools_script}" <SOURCE_DIR> DEPENDEES download DEPENDERS configure)

# ─── 4. libX11 ──────────────────────────────────────────────────────────────
set(_x11_strip_pthread_script "${X11_BUILD_DIR}/strip_pthread.sh")
grc_write_if_changed("${_x11_strip_pthread_script}" "#!/bin/bash\nfind \"$1\" -name Makefile -type f -exec sed -i 's/-lpthread//g;s/XTHREADLIB = -lpthread/XTHREADLIB = /g;s/USE_THREAD_LIBS = -lpthread/USE_THREAD_LIBS = /g' {} \\;\n")

add_autotools_project(libX11
    SOURCE_DIR "${_x11_dir}/libX11" BINARY_DIR "${X11_BUILD_DIR}/libX11" INSTALL_DIR "${X11_SYSROOT}"
    DEPENDS xorgproto xtrans libxcb CONFIGURE_ARGS "PTHREAD_CFLAGS=" "PTHREAD_LIBS=" --enable-shared --disable-static --disable-xf86bigfont --disable-specs --disable-loadable-i18n --disable-composecache --without-xmlto --without-fop --enable-malloc0returnsnull "CFLAGS=${NDK_CFLAGS_STR} -I${X11_SYSROOT}/include" "LDFLAGS=-L${X11_SYSROOT}/lib"
    EXTERNAL_PROJECT_ARGS INSTALL_BYPRODUCTS "${X11_SYSROOT}/lib/libX11.so"
)
ExternalProject_Add_Step(libX11 autoreconf COMMAND bash "${_ensure_autotools_script}" <SOURCE_DIR> DEPENDEES download DEPENDERS configure)
ExternalProject_Add_Step(libX11 strip_pthread COMMAND bash "${_x11_strip_pthread_script}" <BINARY_DIR> DEPENDEES configure DEPENDERS build)

# ─── 4a. Runtime libs ────────────────────────────────────────────────────────
# The X11 client libraries the base APK ships: libXau/libxcb/libX11 and the
# extensions wine's winex11.drv dlopens. Staged as lib*.so - the only names
# Android extracts from an APK. Their SONAMEs are unversioned too;
# config/core-libs.txt keeps them in the base.
set(_x11_runtime_libs Xau xcb X11 Xext Xrender Xi Xfixes Xrandr Xcursor Xxf86vm Xdmcp)
set(_x11_rt_stamp "${X11_BUILD_DIR}/x11_runtime_libs.stamp")
set(_x11_rt_commands "")
set(_x11_rt_inputs "")
foreach(_lib IN LISTS _x11_runtime_libs)
    list(APPEND _x11_rt_commands
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${X11_SYSROOT}/lib/lib${_lib}.so" "${JNILIBS_DIR}/lib${_lib}.so"
        COMMAND ${NDK_STRIP} --strip-unneeded "${JNILIBS_DIR}/lib${_lib}.so")
    list(APPEND _x11_rt_inputs "${X11_SYSROOT}/lib/lib${_lib}.so")
endforeach()
add_custom_command(
    OUTPUT "${_x11_rt_stamp}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${JNILIBS_DIR}"
    ${_x11_rt_commands}
    COMMAND ${CMAKE_COMMAND} -E touch "${_x11_rt_stamp}"
    DEPENDS ${_x11_rt_inputs}
)
add_custom_target(x11_runtime_libs DEPENDS "${_x11_rt_stamp}")
add_dependencies(x11_runtime_libs libX11 libXext libXrender libXi libXfixes libXrandr libXcursor libXxf86vm libXdmcp)

# ─── 4b & 4c. Xext & Xrender ────────────────────────────────────────────────
# Built BOTH shared + static: cairo/the LV2 GUIs keep their static link, and
# wine's winex11.drv dlopens the shared .so (SONAME_LIBXEXT/XRENDER).
foreach(_lib Xext Xrender)
    add_autotools_project(lib${_lib}
        SOURCE_DIR "${_x11_dir}/lib${_lib}" BINARY_DIR "${X11_BUILD_DIR}/lib${_lib}" INSTALL_DIR "${X11_SYSROOT}"
        DEPENDS libX11 CONFIGURE_ARGS --enable-shared --enable-static --enable-malloc0returnsnull "CFLAGS=${NDK_CFLAGS_STR} -I${X11_SYSROOT}/include" "LDFLAGS=-L${X11_SYSROOT}/lib"
        EXTERNAL_PROJECT_ARGS INSTALL_BYPRODUCTS "${X11_SYSROOT}/lib/lib${_lib}.so"
    )
    ExternalProject_Add_Step(lib${_lib} autoreconf COMMAND bash "${_ensure_autotools_script}" <SOURCE_DIR> DEPENDEES download DEPENDERS configure)
endforeach()

# ─── 4d. X11 extensions wine's winex11.drv dlopens by SONAME ────────────────
# Xdmcp, Xfixes, Xi, Xrandr, Xcursor, Xxf86vm — source-built (shared) here so
# the wine X11 client stack is fully from-source (replaces Termux
# fetch-x11-libs.sh). Pinned to the versions wine was built against. Staged
# into jniLibs with libX11/libxcb so the wine subprocess resolves them.
set(_x11ext_args --enable-shared --disable-static --enable-malloc0returnsnull "CFLAGS=${NDK_CFLAGS_STR} -I${X11_SYSROOT}/include" "LDFLAGS=-L${X11_SYSROOT}/lib")
add_autotools_project(libXdmcp   SOURCE_DIR "${_x11_dir}/libXdmcp"   BINARY_DIR "${X11_BUILD_DIR}/libXdmcp"   INSTALL_DIR "${X11_SYSROOT}" DEPENDS xorgproto                  CONFIGURE_ARGS ${_x11ext_args} EXTERNAL_PROJECT_ARGS INSTALL_BYPRODUCTS "${X11_SYSROOT}/lib/libXdmcp.so")
add_autotools_project(libXfixes  SOURCE_DIR "${_x11_dir}/libXfixes"  BINARY_DIR "${X11_BUILD_DIR}/libXfixes"  INSTALL_DIR "${X11_SYSROOT}" DEPENDS libX11                     CONFIGURE_ARGS ${_x11ext_args} EXTERNAL_PROJECT_ARGS INSTALL_BYPRODUCTS "${X11_SYSROOT}/lib/libXfixes.so")
add_autotools_project(libXi      SOURCE_DIR "${_x11_dir}/libXi"      BINARY_DIR "${X11_BUILD_DIR}/libXi"      INSTALL_DIR "${X11_SYSROOT}" DEPENDS libX11 libXext            CONFIGURE_ARGS ${_x11ext_args} EXTERNAL_PROJECT_ARGS INSTALL_BYPRODUCTS "${X11_SYSROOT}/lib/libXi.so")
add_autotools_project(libXrandr  SOURCE_DIR "${_x11_dir}/libXrandr"  BINARY_DIR "${X11_BUILD_DIR}/libXrandr"  INSTALL_DIR "${X11_SYSROOT}" DEPENDS libX11 libXext libXrender  CONFIGURE_ARGS ${_x11ext_args} EXTERNAL_PROJECT_ARGS INSTALL_BYPRODUCTS "${X11_SYSROOT}/lib/libXrandr.so")
add_autotools_project(libXcursor SOURCE_DIR "${_x11_dir}/libXcursor" BINARY_DIR "${X11_BUILD_DIR}/libXcursor" INSTALL_DIR "${X11_SYSROOT}" DEPENDS libX11 libXfixes libXrender CONFIGURE_ARGS ${_x11ext_args} EXTERNAL_PROJECT_ARGS INSTALL_BYPRODUCTS "${X11_SYSROOT}/lib/libXcursor.so")
add_autotools_project(libXxf86vm SOURCE_DIR "${_x11_dir}/libXxf86vm" BINARY_DIR "${X11_BUILD_DIR}/libXxf86vm" INSTALL_DIR "${X11_SYSROOT}" DEPENDS libX11 libXext            CONFIGURE_ARGS ${_x11ext_args} EXTERNAL_PROJECT_ARGS INSTALL_BYPRODUCTS "${X11_SYSROOT}/lib/libXxf86vm.so")
foreach(_x11ext libXdmcp libXfixes libXi libXrandr libXcursor libXxf86vm)
    ExternalProject_Add_Step(${_x11ext} autoreconf COMMAND bash "${_ensure_autotools_script}" <SOURCE_DIR> DEPENDEES download DEPENDERS configure)
endforeach()

# ─── 5. pixman ──────────────────────────────────────────────────────────────
add_meson_project(pixman
    SOURCE_DIR "${_x11_dir}/pixman" BINARY_DIR "${X11_BUILD_DIR}/pixman" INSTALL_DIR "${X11_SYSROOT}"
    CROSS_FILE ${_x11_cross} MESON_ARGS -Dgtk=disabled -Dlibpng=disabled -Dtests=disabled -Da64-neon=disabled
    EXTERNAL_PROJECT_ARGS INSTALL_BYPRODUCTS "${X11_SYSROOT}/lib/libpixman-1.a"
)

# ─── 6. libpng ──────────────────────────────────────────────────────────────
add_autotools_project(libpng
    SOURCE_DIR "${_x11_dir}/libpng" BINARY_DIR "${X11_BUILD_DIR}/libpng" INSTALL_DIR "${X11_SYSROOT}"
    CONFIGURE_ARGS --enable-static --disable-shared "CPPFLAGS=-I${X11_SYSROOT}/include" "CFLAGS=${NDK_CFLAGS_STR}" "LDFLAGS=-L${X11_SYSROOT}/lib"
    EXTERNAL_PROJECT_ARGS INSTALL_BYPRODUCTS "${X11_SYSROOT}/lib/libpng.a"
)
ExternalProject_Add_Step(libpng autoreconf COMMAND bash "${_ensure_autotools_script}" <SOURCE_DIR> DEPENDEES download DEPENDERS configure)

# ─── 7. Cairo ───────────────────────────────────────────────────────────────
set(_cairo_configure_script "${X11_BUILD_DIR}/configure_cairo.sh")
grc_write_if_changed("${_cairo_configure_script}" "#!/bin/bash
set -euo pipefail
builddir=\"$1\"
srcdir=\"$2\"
prefix=\"$3\"
pcdir=\"$builddir/pkgconfig-no-xrender\"

rm -rf \"$pcdir\"
mkdir -p \"$pcdir\"
cp \"${X11_SYSROOT}/lib/pkgconfig/\"*.pc \"$pcdir/\" 2>/dev/null || true
cp \"${X11_SYSROOT}/share/pkgconfig/\"*.pc \"$pcdir/\" 2>/dev/null || true
rm -f \"$pcdir/xrender.pc\"

run_setup() {
  PKG_CONFIG_PATH=\"$pcdir\" \\
  PKG_CONFIG_LIBDIR=\"$pcdir\" \\
  PKG_CONFIG_SYSROOT_DIR= \\
  meson setup \"$@\" \\
    --prefix=\"$prefix\" \\
    --cross-file \"${_x11_cross}\" \\
    --default-library=static \\
    -Dxlib=enabled \\
    -Dxcb=disabled \\
    -Dpng=enabled \\
    -Dfreetype=disabled \\
    -Dfontconfig=disabled \\
    -Dglib=disabled \\
    -Dspectre=disabled \\
    -Dsymbol-lookup=disabled \\
    -Dtests=disabled
}

if [ -f \"$builddir/meson-private/coredata.dat\" ]; then
  run_setup --wipe \"$builddir\" \"$srcdir\"
else
  run_setup \"$builddir\" \"$srcdir\"
fi
")
ExternalProject_Add(cairo
    SOURCE_DIR "${_x11_dir}/cairo" BINARY_DIR "${X11_BUILD_DIR}/cairo" INSTALL_DIR "${X11_SYSROOT}"
    CONFIGURE_COMMAND bash "${_cairo_configure_script}" <BINARY_DIR> <SOURCE_DIR> <INSTALL_DIR>
    BUILD_COMMAND ninja -C <BINARY_DIR> -j${NJOBS}
    INSTALL_COMMAND ninja -C <BINARY_DIR> install
    DEPENDS libX11 libXext libXrender pixman libpng
    INSTALL_BYPRODUCTS "${X11_SYSROOT}/lib/libcairo.a"
    LOG_CONFIGURE TRUE LOG_BUILD TRUE
)

add_custom_target(x11_sysroot DEPENDS cairo x11_runtime_libs)

# ─── What a cairo/X11 plugin UI links ────────────────────────────────────────
# cairo and its static dependencies, the X11 client libraries the base APK
# ships, and the MIT-SHM stub. Consumers still add_dependencies(x11_sysroot).
add_library(grc_x11_ui_libs INTERFACE)
target_link_libraries(grc_x11_ui_libs INTERFACE
    "${X11_SYSROOT}/lib/libcairo.a"
    "${X11_SYSROOT}/lib/libpixman-1.a"
    "${X11_SYSROOT}/lib/libpng.a"
    -L"${X11_SYSROOT}/lib"
    X11 xcb Xau Xrender
    xshm_stub
    m z log dl
)
