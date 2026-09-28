cmake_minimum_required(VERSION 3.22.1)
project(vsthost CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Oboe — shared with :app via GuitarRackCraft's 3rd_party/oboe submodule.
set(OBOE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../3rd_party/oboe")
if(NOT EXISTS "${OBOE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Oboe sources not found at ${OBOE_DIR}. "
        "Run: git submodule update --init --recursive 3rd_party/oboe")
endif()
add_subdirectory("${OBOE_DIR}" "${CMAKE_BINARY_DIR}/oboe-build" EXCLUDE_FROM_ALL)

# In-process X11 server (renders to ANativeWindow via EGL/GLES2). The source
# tree is :app's - one server for LV2 UIs and wine editors; see its
# CMakeLists.txt.
set(GRC_X11_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../app/src/main/cpp/x11")
add_subdirectory("${GRC_X11_DIR}" "${CMAKE_BINARY_DIR}/x11-build")

add_library(vsthost SHARED
    # JNI surface (PE inspection + wineboot one-shot)
    jni_runtime.cpp
    jni_x11.cpp
    # Wine subprocess launcher + host/guest shm rings
    launcher/WineHostProcess.cpp
    ipc/SharedRing.cpp
    ipc/PickerChannel.cpp
    plugin/PluginUIGuard.cpp  # called by the X11 server
    util/PeExports.cpp
    # Phase 0 GPU-upgrade spike (cross-driver AHB+fence, throwaway diagnostic)
    ahbspike/AhbSpike.cpp
    # Phase 2 AHB side-channel synthetic client (throwaway diagnostic)
    ahbspike/AhbChannelTest.cpp
    # GuitarRackCraft IPlugin bridge (consumed by :app via prefab)
    vst/VstFactory.cpp
    vst/WineVstPlugin.cpp
)

target_include_directories(vsthost PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(vsthost
    x11_native_display
    oboe
    android
    log
    EGL
    GLESv2
    vulkan   # AhbSpike.cpp (Vulkan types/headers; Turnip loaded via dlopen at runtime)
)

target_compile_options(vsthost PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)
