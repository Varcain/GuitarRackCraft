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
# cmake/targets/fftw3.cmake — Build FFTW3 single-precision static library
# =============================================================================

set(FFTW_SRC "${THIRD_PARTY}/fftw3")

# Verify that build.sh has already generated the codelets and configure script.
if(NOT EXISTS "${FFTW_SRC}/configure")
    message(FATAL_ERROR
        "FFTW3 configure script not found. Run build.sh (not cmake directly) "
        "so that autoreconf and codelet generation happen first.")
endif()
if(NOT EXISTS "${FFTW_SRC}/dft/scalar/codelets/n1_2.c")
    message(FATAL_ERROR
        "FFTW3 codelets not found. Run build.sh (not cmake directly) "
        "so that codelet generation happens first. Requires: ocaml")
endif()

add_autotools_project(fftw3
    SOURCE_DIR "${FFTW_SRC}" BINARY_DIR "${FFTW3_BUILD_DIR}" INSTALL_DIR "${FFTW3_PREFIX}"
    CONFIGURE_ARGS --host=${NDK_HOST} --enable-float --enable-static --disable-shared --with-pic --disable-fortran --disable-mpi --disable-threads --disable-openmp --disable-doc "CC=${NDK_CC}" "CFLAGS=${NDK_CFLAGS_STR}" "LDFLAGS="
    EXTERNAL_PROJECT_ARGS BUILD_BYPRODUCTS "${FFTW3_PREFIX}/lib/libfftw3f.a"
)
watch_external_sources(fftw3 DIRECTORIES "${FFTW_SRC}/kernel" "${FFTW_SRC}/dft" "${FFTW_SRC}/rdft" "${FFTW_SRC}/api")
