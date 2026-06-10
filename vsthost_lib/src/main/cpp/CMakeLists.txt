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

add_library(vsthost SHARED
    # JNI surface (PE inspection + wineboot one-shot)
    jni_runtime.cpp
    jni_x11.cpp
    # Wine subprocess launcher + host/guest shm rings
    launcher/WineHostProcess.cpp
    ipc/SharedRing.cpp
    ipc/PickerChannel.cpp
    # In-process X11 server (renders to ANativeWindow via EGL/GLES2)
    x11/X11NativeDisplay.cpp
    x11/X11Worker.cpp
    x11/X11AtomStore.cpp
    x11/X11WindowManager.cpp
    x11/X11PixmapStore.cpp
    x11/X11ConnectionHandler.cpp
    x11/X11Framebuffer.cpp
    x11/X11PropertyStore.cpp
    x11/AhbTexture.cpp
    plugin/PluginUIGuard.cpp
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
