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
# cmake/targets/x11_sysroot.cmake — Build X11/Cairo/Mesa dependency chain
# =============================================================================

set(_x11_dir    "${THIRD_PARTY}/x11")
set(_x11_cross  "${X11_BUILD_DIR}/android_cross.txt")
set(_x11_pkg    "${X11_SYSROOT}/lib/pkgconfig:${X11_SYSROOT}/share/pkgconfig")

# Helper: ensure autoreconf
set(_ensure_autotools_script "${X11_BUILD_DIR}/ensure_autotools.sh")
file(WRITE "${_ensure_autotools_script}" "#!/bin/bash\ndir=\"$1\"\nif [ ! -f \"$dir/configure\" ]; then cd \"$dir\" && ACLOCAL_PATH=\"${X11_SYSROOT}/share/aclocal\" autoreconf -fi; fi\n")

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
    EXTERNAL_PROJECT_ARGS BUILD_BYPRODUCTS "${X11_SYSROOT}/lib/libXau.so"
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
    EXTERNAL_PROJECT_ARGS BUILD_BYPRODUCTS "${X11_SYSROOT}/lib/libxcb.so"
)
ExternalProject_Add_Step(libxcb autoreconf COMMAND bash "${_ensure_autotools_script}" <SOURCE_DIR> DEPENDEES download DEPENDERS configure)

# ─── 4. libX11 ──────────────────────────────────────────────────────────────
set(_x11_strip_pthread_script "${X11_BUILD_DIR}/strip_pthread.sh")
file(WRITE "${_x11_strip_pthread_script}" "#!/bin/bash\nfind \"$1\" -name Makefile -type f -exec sed -i 's/-lpthread//g;s/XTHREADLIB = -lpthread/XTHREADLIB = /g;s/USE_THREAD_LIBS = -lpthread/USE_THREAD_LIBS = /g' {} \\;\n")

add_autotools_project(libX11
    SOURCE_DIR "${_x11_dir}/libX11" BINARY_DIR "${X11_BUILD_DIR}/libX11" INSTALL_DIR "${X11_SYSROOT}"
    DEPENDS xorgproto xtrans libxcb CONFIGURE_ARGS "PTHREAD_CFLAGS=" "PTHREAD_LIBS=" --enable-shared --disable-static --disable-xf86bigfont --disable-specs --disable-xkb --disable-loadable-i18n --disable-composecache --without-xmlto --without-fop --enable-malloc0returnsnull "CFLAGS=${NDK_CFLAGS_STR} -I${X11_SYSROOT}/include" "LDFLAGS=-L${X11_SYSROOT}/lib"
    EXTERNAL_PROJECT_ARGS BUILD_BYPRODUCTS "${X11_SYSROOT}/lib/libX11.so"
)
ExternalProject_Add_Step(libX11 autoreconf COMMAND bash "${_ensure_autotools_script}" <SOURCE_DIR> DEPENDEES download DEPENDERS configure)
ExternalProject_Add_Step(libX11 strip_pthread COMMAND bash "${_x11_strip_pthread_script}" <BINARY_DIR> DEPENDEES configure DEPENDERS build)

# ─── 4a. Runtime libs ────────────────────────────────────────────────────────
set(_x11_rt_stamp "${X11_BUILD_DIR}/x11_runtime_libs.stamp")
add_custom_command(
    OUTPUT "${_x11_rt_stamp}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${JNILIBS_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${X11_SYSROOT}/lib/libXau.so" "${JNILIBS_DIR}/libXau.so.6"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${X11_SYSROOT}/lib/libxcb.so" "${JNILIBS_DIR}/libxcb.so.1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${X11_SYSROOT}/lib/libX11.so" "${JNILIBS_DIR}/libX11.so.6"
    COMMAND ${NDK_STRIP} --strip-unneeded "${JNILIBS_DIR}/libXau.so.6" || true
    COMMAND ${NDK_STRIP} --strip-unneeded "${JNILIBS_DIR}/libxcb.so.1" || true
    COMMAND ${NDK_STRIP} --strip-unneeded "${JNILIBS_DIR}/libX11.so.6" || true
    COMMAND ${CMAKE_COMMAND} -E touch "${_x11_rt_stamp}"
    DEPENDS "${X11_SYSROOT}/lib/libXau.so" "${X11_SYSROOT}/lib/libxcb.so" "${X11_SYSROOT}/lib/libX11.so"
)
add_custom_target(x11_runtime_libs DEPENDS "${_x11_rt_stamp}")
add_dependencies(x11_runtime_libs libX11)

# ─── 4b & 4c. Xext & Xrender ────────────────────────────────────────────────
foreach(_lib Xext Xrender)
    add_autotools_project(lib${_lib}
        SOURCE_DIR "${_x11_dir}/lib${_lib}" BINARY_DIR "${X11_BUILD_DIR}/lib${_lib}" INSTALL_DIR "${X11_SYSROOT}"
        DEPENDS libX11 CONFIGURE_ARGS --enable-static --disable-shared --enable-malloc0returnsnull "CFLAGS=${NDK_CFLAGS_STR} -I${X11_SYSROOT}/include" "LDFLAGS=-L${X11_SYSROOT}/lib"
        EXTERNAL_PROJECT_ARGS BUILD_BYPRODUCTS "${X11_SYSROOT}/lib/lib${_lib}.a"
    )
    ExternalProject_Add_Step(lib${_lib} autoreconf COMMAND bash "${_ensure_autotools_script}" <SOURCE_DIR> DEPENDEES download DEPENDERS configure)
endforeach()

# ─── 5. pixman ──────────────────────────────────────────────────────────────
add_meson_project(pixman
    SOURCE_DIR "${_x11_dir}/pixman" BINARY_DIR "${X11_BUILD_DIR}/pixman" INSTALL_DIR "${X11_SYSROOT}"
    CROSS_FILE ${_x11_cross} MESON_ARGS -Dgtk=disabled -Dlibpng=disabled -Dtests=disabled -Da64-neon=disabled
    EXTERNAL_PROJECT_ARGS BUILD_BYPRODUCTS "${X11_SYSROOT}/lib/libpixman-1.a"
)

# ─── 6. libpng ──────────────────────────────────────────────────────────────
add_autotools_project(libpng
    SOURCE_DIR "${_x11_dir}/libpng" BINARY_DIR "${X11_BUILD_DIR}/libpng" INSTALL_DIR "${X11_SYSROOT}"
    CONFIGURE_ARGS --enable-static --disable-shared "CPPFLAGS=-I${X11_SYSROOT}/include" "CFLAGS=${NDK_CFLAGS_STR}" "LDFLAGS=-L${X11_SYSROOT}/lib"
    EXTERNAL_PROJECT_ARGS BUILD_BYPRODUCTS "${X11_SYSROOT}/lib/libpng.a"
)
ExternalProject_Add_Step(libpng autoreconf COMMAND bash "${_ensure_autotools_script}" <SOURCE_DIR> DEPENDEES download DEPENDERS configure)

# ─── 7. Cairo ───────────────────────────────────────────────────────────────
ExternalProject_Add(cairo
    SOURCE_DIR "${_x11_dir}/cairo" BINARY_DIR "${X11_BUILD_DIR}/cairo" INSTALL_DIR "${X11_SYSROOT}"
    CONFIGURE_COMMAND bash -c "[ -f '${X11_SYSROOT}/lib/pkgconfig/xrender.pc' ] && mv '${X11_SYSROOT}/lib/pkgconfig/xrender.pc' '${X11_SYSROOT}/lib/pkgconfig/xrender.pc.bak' || true"
    COMMAND ${CMAKE_COMMAND} -E env "PKG_CONFIG_PATH=${_x11_pkg}" "PKG_CONFIG_LIBDIR=${X11_SYSROOT}/lib/pkgconfig" meson setup <BINARY_DIR> <SOURCE_DIR> --prefix=<INSTALL_DIR> --cross-file ${_x11_cross} --default-library=static -Dxlib=enabled -Dxcb=disabled -Dpng=enabled -Dfreetype=disabled -Dfontconfig=disabled -Dglib=disabled -Dspectre=disabled -Dsymbol-lookup=disabled -Dtests=disabled
    BUILD_COMMAND ninja -C <BINARY_DIR> -j${NJOBS}
    INSTALL_COMMAND ninja -C <BINARY_DIR> install COMMAND bash -c "[ -f '${X11_SYSROOT}/lib/pkgconfig/xrender.pc.bak' ] && mv '${X11_SYSROOT}/lib/pkgconfig/xrender.pc.bak' '${X11_SYSROOT}/lib/pkgconfig/xrender.pc' || true"
    DEPENDS libX11 libXext libXrender pixman libpng
    BUILD_BYPRODUCTS "${X11_SYSROOT}/lib/libcairo.a"
    LOG_CONFIGURE TRUE LOG_BUILD TRUE
)

# ─── 8. Mesa deps ───────────────────────────────────────────────────────────
set(EXPAT_SRC "${THIRD_PARTY}/expat/expat")
ExternalProject_Add(expat
    SOURCE_DIR "${EXPAT_SRC}" BINARY_DIR "${X11_BUILD_DIR}/expat" INSTALL_DIR "${X11_SYSROOT}"
    CMAKE_ARGS -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE} -DANDROID_ABI=${ANDROID_ABI} -DANDROID_PLATFORM=${ANDROID_PLATFORM} -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR> -DBUILD_SHARED_LIBS=OFF -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_EXAMPLES=OFF -DEXPAT_BUILD_TESTS=OFF -DEXPAT_BUILD_DOCS=OFF -DEXPAT_SHARED_LIBS=OFF ${NDK_CCACHE_CMAKE_ARGS}
    BUILD_BYPRODUCTS "${X11_SYSROOT}/lib/libexpat.a"
    LOG_CONFIGURE TRUE LOG_BUILD TRUE
)

set(_zlib_stamp "${X11_BUILD_DIR}/zlib_sysroot.stamp")
add_custom_command(
    OUTPUT "${_zlib_stamp}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NDK_SYSROOT}/usr/lib/aarch64-linux-android/libz.a" "${X11_SYSROOT}/lib/libz.a"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NDK_SYSROOT}/usr/include/zlib.h" "${X11_SYSROOT}/include/zlib.h"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NDK_SYSROOT}/usr/include/zconf.h" "${X11_SYSROOT}/include/zconf.h"
    COMMAND ${CMAKE_COMMAND} -DOUTPUT=${X11_SYSROOT}/lib/pkgconfig/zlib.pc -DPREFIX=${X11_SYSROOT} -P "${CMAKE_BINARY_DIR}/scripts/WriteZlibPC.cmake"
    COMMAND ${CMAKE_COMMAND} -E touch "${_zlib_stamp}"
)
add_custom_target(zlib_sysroot DEPENDS "${_zlib_stamp}")
file(WRITE "${CMAKE_BINARY_DIR}/scripts/WriteZlibPC.cmake" "include(\"${PROJECT_ROOT}/cmake/modules/ExternalBuild.cmake\")\nwrite_pkg_config(OUTPUT \"\${OUTPUT}\" NAME zlib DESCRIPTION \"zlib compression library\" VERSION 1.2.13 PREFIX \"\${PREFIX}\" LIBS -lz)")

# ─── 9. Mesa ────────────────────────────────────────────────────────────────
ExternalProject_Add(mesa
    SOURCE_DIR "${THIRD_PARTY}/mesa" BINARY_DIR "${MESA_BUILD_DIR}" INSTALL_DIR "${X11_SYSROOT}"
    PATCH_COMMAND bash -c "sed -i 's/#if defined(__ANDROID__)/#if defined(__ANDROID__) \\&\\& !defined(MESA_FORCE_LINUX)/' <SOURCE_DIR>/src/util/detect_os.h 2>/dev/null || true"
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env "PKG_CONFIG_PATH=${_x11_pkg}" "PKG_CONFIG_LIBDIR=${X11_SYSROOT}/lib/pkgconfig" "PKG_CONFIG_SYSROOT_DIR=" meson setup <BINARY_DIR> <SOURCE_DIR> --cross-file "${MESA_BUILD_DIR}/mesa_cross.txt" --prefix=<INSTALL_DIR> --default-library=shared -Dplatforms=x11 -Dgallium-drivers=softpipe -Dvulkan-drivers= -Dglx=xlib -Degl=disabled -Dgbm=disabled -Dllvm=disabled -Dshared-glapi=enabled -Dgles1=disabled -Dgles2=disabled -Dosmesa=false -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dbuild-tests=false -Dxmlconfig=disabled -Dxlib-lease=disabled
    BUILD_COMMAND ninja -C <BINARY_DIR> -j${NJOBS}
    INSTALL_COMMAND ninja -C <BINARY_DIR> install
    DEPENDS libX11 libXext expat zlib_sysroot
    BUILD_BYPRODUCTS "${X11_SYSROOT}/lib/libGL.so" "${X11_SYSROOT}/lib/libglapi.so"
    LOG_CONFIGURE TRUE LOG_BUILD TRUE
)

set(_mesa_rt_stamp "${X11_BUILD_DIR}/mesa_runtime_libs.stamp")
add_custom_command(
    OUTPUT "${_mesa_rt_stamp}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${JNILIBS_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${X11_SYSROOT}/lib/libGL.so" "${JNILIBS_DIR}/libGL.so.1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${X11_SYSROOT}/lib/libglapi.so" "${JNILIBS_DIR}/libglapi.so.0"
    COMMAND ${NDK_STRIP} --strip-unneeded "${JNILIBS_DIR}/libGL.so.1" || true
    COMMAND ${NDK_STRIP} --strip-unneeded "${JNILIBS_DIR}/libglapi.so.0" || true
    COMMAND ${CMAKE_COMMAND} -E make_directory "${APP_ROOT}/assets/x11_libs/arm64-v8a"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${X11_SYSROOT}/lib/libGL.so" "${APP_ROOT}/assets/x11_libs/arm64-v8a/libGL.so.1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${X11_SYSROOT}/lib/libglapi.so" "${APP_ROOT}/assets/x11_libs/arm64-v8a/libglapi.so.0"
    COMMAND ${NDK_STRIP} --strip-unneeded "${APP_ROOT}/assets/x11_libs/arm64-v8a/libGL.so.1" || true
    COMMAND ${NDK_STRIP} --strip-unneeded "${APP_ROOT}/assets/x11_libs/arm64-v8a/libglapi.so.0" || true
    COMMAND ${CMAKE_COMMAND} -E touch "${_mesa_rt_stamp}"
)
add_custom_target(mesa_runtime_libs DEPENDS "${_mesa_rt_stamp}")
add_dependencies(mesa_runtime_libs mesa)

add_custom_target(x11_sysroot DEPENDS cairo mesa_runtime_libs x11_runtime_libs)
