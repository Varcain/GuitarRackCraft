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

# In-process X11 server (EGL/GLES2 onto an ANativeWindow). One source tree,
# built into both native libraries:
#   :app         add_subdirectory(x11)                 LV2 plugin UIs
#   :vsthost_lib add_subdirectory(<this dir> <bindir>) wine VST editors
# In the full flavor both libraries are loaded into one process, each with
# its own copy of this code (and its own display registry). Hidden
# visibility keeps either copy from binding to the other's symbols.

add_library(x11_native_display STATIC
    X11NativeDisplay.cpp
    X11Worker.cpp
    X11AtomStore.cpp
    X11WindowManager.cpp
    X11PixmapStore.cpp
    X11ConnectionHandler.cpp
    X11Framebuffer.cpp
    X11PropertyStore.cpp
    AhbTexture.cpp
    # The "is a plugin UI being created" guard this server consults; built
    # here so each consumer gets it with the server.
    ../plugin/PluginUIGuard.cpp
)
# Consumers include the headers as "x11/X11NativeDisplay.h".
target_include_directories(x11_native_display PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
)
set_target_properties(x11_native_display PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
target_compile_options(x11_native_display PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)
target_link_libraries(x11_native_display PUBLIC
    log
    android
    EGL
    GLESv2
)
