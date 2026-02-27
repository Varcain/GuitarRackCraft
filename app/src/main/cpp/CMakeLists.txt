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

cmake_minimum_required(VERSION 3.22.1)

project("guitarrackcraft")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Oboe library (3rd_party submodule only; 4 levels up from app/src/main/cpp to project root)
set(OBOE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../3rd_party/oboe")
if(EXISTS "${OBOE_ROOT}/CMakeLists.txt")
    add_subdirectory(${OBOE_ROOT} oboe_build)
else()
    message(FATAL_ERROR "Oboe not found. Run: git submodule update --init --recursive 3rd_party/oboe")
endif()

# LV2 Libraries Configuration
# Set LV2_LIBS_DIR to the directory containing compiled LV2 libraries
# Example: set(LV2_LIBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/lv2")
set(LV2_LIBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/lv2" CACHE PATH "Directory containing LV2 libraries")

# Check if LV2 libraries are available
set(LV2_AVAILABLE FALSE)
if(EXISTS "${LV2_LIBS_DIR}/include/lilv-0/lilv/lilv.h" OR EXISTS "${LV2_LIBS_DIR}/include/lilv/lilv.h")
    set(LV2_AVAILABLE TRUE)
    message(STATUS "LV2 libraries found at: ${LV2_LIBS_DIR}")
    
    # Include LV2 headers (lilv.h includes "lv2/core/lv2.h" so need base include too)
    include_directories(${LV2_LIBS_DIR}/include)
    if(EXISTS "${LV2_LIBS_DIR}/include/lilv-0")
        include_directories(${LV2_LIBS_DIR}/include/lilv-0)
        include_directories(${LV2_LIBS_DIR}/include/serd-0)
        include_directories(${LV2_LIBS_DIR}/include/sord-0)
    endif()
    
    # LV2 libraries (versioned names from build_lv2_libs.sh: lilv-0, serd-0, sord-0, sratom-0, zix-0)
    # Prefer static libraries to avoid packaging .so.0 files
    set(LV2_LIB_DIR "${LV2_LIBS_DIR}/lib")
    # Try static library first, then shared
    set(LILV_LIB "")
    if(EXISTS "${LV2_LIB_DIR}/liblilv-0.a")
        set(LILV_LIB "${LV2_LIB_DIR}/liblilv-0.a")
    elseif(EXISTS "${LV2_LIB_DIR}/liblilv-0.so.0")
        set(LILV_LIB "${LV2_LIB_DIR}/liblilv-0.so.0")
    elseif(EXISTS "${LV2_LIB_DIR}/liblilv-0.so")
        set(LILV_LIB "${LV2_LIB_DIR}/liblilv-0.so")
    endif()
    if(NOT LILV_LIB)
        find_library(LILV_LIB NAMES lilv-0 lilv PATHS ${LV2_LIB_DIR} NO_DEFAULT_PATH)
    endif()
    find_library(SERD_LIB NAMES serd-0 serd PATHS ${LV2_LIB_DIR} NO_DEFAULT_PATH)
    find_library(SORD_LIB NAMES sord-0 sord PATHS ${LV2_LIB_DIR} NO_DEFAULT_PATH)
    find_library(SRATOM_LIB NAMES sratom-0 sratom PATHS ${LV2_LIB_DIR} NO_DEFAULT_PATH)
    find_library(ZIX_LIB NAMES zix-0 zix PATHS ${LV2_LIB_DIR} NO_DEFAULT_PATH)
    if(NOT SERD_LIB AND EXISTS "${LV2_LIB_DIR}/libserd-0.a")
        set(SERD_LIB "${LV2_LIB_DIR}/libserd-0.a")
    endif()
    if(NOT SORD_LIB AND EXISTS "${LV2_LIB_DIR}/libsord-0.a")
        set(SORD_LIB "${LV2_LIB_DIR}/libsord-0.a")
    endif()
    if(NOT SRATOM_LIB AND EXISTS "${LV2_LIB_DIR}/libsratom-0.a")
        set(SRATOM_LIB "${LV2_LIB_DIR}/libsratom-0.a")
    endif()
    if(NOT ZIX_LIB AND EXISTS "${LV2_LIB_DIR}/libzix-0.a")
        set(ZIX_LIB "${LV2_LIB_DIR}/libzix-0.a")
    endif()
    
    if(LILV_LIB AND SERD_LIB AND SORD_LIB)
        message(STATUS "Found LV2 libraries: lilv-0, serd-0, sord-0")
    else()
        message(WARNING "Some LV2 libraries not found. LV2 support will be disabled.")
        set(LV2_AVAILABLE FALSE)
    endif()
else()
    message(STATUS "LV2 libraries not found. LV2 backend will be disabled.")
    message(STATUS "To enable LV2 support, compile libraries and set LV2_LIBS_DIR")
endif()

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/plugin
    ${CMAKE_CURRENT_SOURCE_DIR}/plugin/lv2
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/jni
    ${CMAKE_CURRENT_SOURCE_DIR}/x11
    ${OBOE_ROOT}/include
)

# Plugin abstraction layer
add_library(plugin_abstraction STATIC
    plugin/PluginChain.cpp
    plugin/PluginRegistry.cpp
    plugin/PluginUIGuard.cpp
    plugin/PluginUIManager.cpp
    plugin/StateSerializer.cpp
)

# LV2 backend (conditional compilation)
if(LV2_AVAILABLE)
    add_library(lv2_backend STATIC
        plugin/lv2/LV2Plugin.cpp
        plugin/lv2/LV2PluginFactory.cpp
        plugin/lv2/LV2PluginUI.cpp
        plugin/lv2/LV2Utils.cpp
    )
    target_link_libraries(lv2_backend
        ${LILV_LIB}
        ${SERD_LIB}
        ${SORD_LIB}
        ${SRATOM_LIB}
        ${ZIX_LIB}
    )
    add_definitions(-DHAVE_LV2=1)
    message(STATUS "LV2 backend enabled")
else()
    # Create stub implementation when LV2 is not available
    add_library(lv2_backend STATIC
        plugin/lv2/LV2Plugin.cpp
        plugin/lv2/LV2PluginFactory.cpp
        plugin/lv2/LV2PluginUI.cpp
        plugin/lv2/LV2Utils.cpp
    )
    add_definitions(-DHAVE_LV2=0)
    message(STATUS "LV2 backend disabled (stub mode)")
endif()

# Shared utilities
add_library(utils STATIC
    utils/WavIO.cpp
)

# Audio engine
add_library(audio_engine STATIC
    engine/AudioEngine.cpp
    engine/AudioRecorder.cpp
    engine/OfflineProcessor.cpp
)
target_link_libraries(audio_engine utils)

# XShm stub - used when plugin UI .so references MIT-SHM (fallback to XPutImage)
add_library(xshm_stub STATIC
    xshm_stub.c
)

# X11 native display (EGL + minimal X11 server, ANativeWindow)
add_library(x11_native_display STATIC
    x11/X11NativeDisplay.cpp
    x11/X11Worker.cpp
)
target_link_libraries(x11_native_display
    log
    android
    EGL
    GLESv2
)

add_library(guitarrackcraft SHARED
    jni/NativeBridge.cpp
)

target_link_libraries(guitarrackcraft
    plugin_abstraction
    audio_engine
    oboe
    lv2_backend
    x11_native_display
    xshm_stub
    log
    android
)

# Link LV2 libraries if available
if(LV2_AVAILABLE)
    target_link_libraries(guitarrackcraft
        ${LILV_LIB}
        ${SERD_LIB}
        ${SORD_LIB}
        ${SRATOM_LIB}
        ${ZIX_LIB}
    )
endif()
