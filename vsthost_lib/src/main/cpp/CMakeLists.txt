cmake_minimum_required(VERSION 3.22.1)
project(vsthost CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The repository root (this file is vsthost_lib/src/main/cpp/CMakeLists.txt).
get_filename_component(GRC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)

# Oboe — shared with :app via GuitarRackCraft's 3rd_party/oboe submodule.
set(OBOE_DIR "${GRC_ROOT}/3rd_party/oboe")
if(NOT EXISTS "${OBOE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Oboe sources not found at ${OBOE_DIR}. "
        "Run: git submodule update --init --recursive 3rd_party/oboe")
endif()
add_subdirectory("${OBOE_DIR}" "${CMAKE_BINARY_DIR}/oboe-build" EXCLUDE_FROM_ALL)

# In-process X11 server (renders to ANativeWindow via EGL/GLES2). The source
# tree is :app's - one server for LV2 UIs and wine editors; see its
# CMakeLists.txt.
set(GRC_APP_CPP_DIR "${GRC_ROOT}/app/src/main/cpp")
add_subdirectory("${GRC_APP_CPP_DIR}/x11" "${CMAKE_BINARY_DIR}/x11-build")

add_library(vsthost SHARED
    # JNI surface (PE inspection + wineboot one-shot)
    jni_runtime.cpp
    jni_x11.cpp
    # Wine subprocess launcher + host/guest shm rings
    launcher/WineHostProcess.cpp
    ipc/SharedRing.cpp
    ipc/PickerChannel.cpp
    util/PeExports.cpp
    # GuitarRackCraft IPlugin bridge (consumed by :app via prefab)
    vst/VstFactory.cpp
    vst/WineVstPlugin.cpp
)

target_include_directories(vsthost PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"  # the public header :app gets via prefab
    "${GRC_APP_CPP_DIR}"               # plugin/IPlugin.h, plugin/IPluginFactory.h
    "${GRC_ROOT}/vsthost_lib/external" # shared_layout.h, shared with the wine-side hosts
)

target_link_libraries(vsthost
    x11_native_display
    oboe
    android
    log
    EGL
    GLESv2
)

# Developer diagnostics, driven by marker files from the fullDebug hooks and
# PluginSurface: the Phase 0 GPU-upgrade spike (cross-driver AHB + fence) and
# the Phase 2 AHB side-channel synthetic client. On by default in Debug builds
# only; without it their JNI entry points return false. (Chosen here rather than
# with a debug-only Gradle CMake argument: that makes AGP configure :app against
# vsthost_lib's header-only prefab publication, so :app fails to link.)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_vsthost_diagnostics_default ON)
else()
    set(_vsthost_diagnostics_default OFF)
endif()
option(VSTHOST_DEBUG_DIAGNOSTICS "Build the AhbSpike/AhbChannelTest diagnostics"
       ${_vsthost_diagnostics_default})
if(VSTHOST_DEBUG_DIAGNOSTICS)
    target_sources(vsthost PRIVATE
        ahbspike/AhbSpike.cpp
        ahbspike/AhbChannelTest.cpp
    )
    target_compile_definitions(vsthost PRIVATE VSTHOST_DEBUG_DIAGNOSTICS=1)
    target_link_libraries(vsthost vulkan)  # AhbSpike's Vulkan types; Turnip is dlopen'd
endif()

target_compile_options(vsthost PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)
