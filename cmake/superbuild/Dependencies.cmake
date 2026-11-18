include(ExternalProject)
include(CTest)

set(ZT_BUILD_JOBS 2 CACHE STRING "Parallel jobs within each dependency build")
set(ZT_STAGING_PREFIX "${CMAKE_BINARY_DIR}/staging" CACHE PATH "Target dependency installation prefix")
set(ZT_HOST_PREFIX "${CMAKE_BINARY_DIR}/host" CACHE PATH "Build-machine tools installation prefix")
find_program(ZT_HOST_CC NAMES clang REQUIRED)
find_program(ZT_HOST_CXX NAMES clang++ REQUIRED)

foreach(_dep protobuf paho-mqtt-c paho-mqtt-cpp SDL)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/3rdparty/${_dep}/CMakeLists.txt")
        message(FATAL_ERROR "Missing ${_dep} source. Run: git submodule update --init --recursive")
    endif()
endforeach()

set(_common
    -DCMAKE_BUILD_TYPE:STRING=Release
    -DCMAKE_INSTALL_LIBDIR:STRING=lib
    -DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON
    -DCMAKE_POLICY_VERSION_MINIMUM:STRING=3.5
    -DBUILD_SHARED_LIBS:BOOL=OFF)
set(_target ${_common}
    -DCMAKE_INSTALL_PREFIX:PATH=${ZT_STAGING_PREFIX}
    -DCMAKE_PREFIX_PATH:PATH=${ZT_STAGING_PREFIX}
    -DZT_STAGING_PREFIX:PATH=${ZT_STAGING_PREFIX})
if(CMAKE_TOOLCHAIN_FILE)
    list(APPEND _target -DCMAKE_TOOLCHAIN_FILE:FILEPATH=${CMAKE_TOOLCHAIN_FILE}
        -DZT_RK_SDK_ROOT:PATH=${ZT_RK_SDK_ROOT})
else()
    list(APPEND _target -DCMAKE_C_COMPILER:FILEPATH=${ZT_HOST_CC}
        -DCMAKE_CXX_COMPILER:FILEPATH=${ZT_HOST_CXX})
endif()

# Host protoc deliberately receives no target toolchain or target prefix.
ExternalProject_Add(host-protobuf
    SOURCE_DIR "${CMAKE_SOURCE_DIR}/3rdparty/protobuf"
    BINARY_DIR "${CMAKE_BINARY_DIR}/deps/host-protobuf"
    DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
    CMAKE_ARGS ${_common} -DCMAKE_INSTALL_PREFIX:PATH=${ZT_HOST_PREFIX}
        -DCMAKE_C_COMPILER:FILEPATH=${ZT_HOST_CC}
        -DCMAKE_CXX_COMPILER:FILEPATH=${ZT_HOST_CXX}
        -Dprotobuf_BUILD_TESTS:BOOL=OFF -Dprotobuf_WITH_ZLIB:BOOL=OFF
        -Dprotobuf_BUILD_PROTOC_BINARIES:BOOL=ON
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel ${ZT_BUILD_JOBS}
    BUILD_ALWAYS TRUE)

function(zt_dependency name source previous)
    ExternalProject_Add(${name}
        SOURCE_DIR "${CMAKE_SOURCE_DIR}/3rdparty/${source}"
        BINARY_DIR "${CMAKE_BINARY_DIR}/deps/${name}"
        DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
        CMAKE_ARGS ${_target} ${ARGN}
        DEPENDS ${previous}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel ${ZT_BUILD_JOBS}
        BUILD_ALWAYS TRUE)
endfunction()

# Sequential packages keep the memory budget predictable alongside the BSP build.
zt_dependency(target-protobuf protobuf host-protobuf
    -Dprotobuf_BUILD_TESTS:BOOL=OFF -Dprotobuf_WITH_ZLIB:BOOL=OFF
    -Dprotobuf_BUILD_PROTOC_BINARIES:BOOL=OFF -Dprotobuf_BUILD_LIBPROTOC:BOOL=OFF)
zt_dependency(paho-c paho-mqtt-c target-protobuf
    -DPAHO_BUILD_SHARED:BOOL=OFF -DPAHO_BUILD_STATIC:BOOL=ON -DPAHO_WITH_SSL:BOOL=OFF
    -DPAHO_ENABLE_TESTING:BOOL=OFF -DPAHO_BUILD_SAMPLES:BOOL=OFF)
zt_dependency(paho-cpp paho-mqtt-cpp paho-c
    -DPAHO_BUILD_SHARED:BOOL=OFF -DPAHO_BUILD_STATIC:BOOL=ON -DPAHO_WITH_SSL:BOOL=OFF
    -DPAHO_WITH_MQTT_C:BOOL=OFF -DPAHO_BUILD_TESTS:BOOL=OFF -DPAHO_BUILD_EXAMPLES:BOOL=OFF)

# UI stays software/offscreen. Only desktop writeback preview enables Wayland/X11/OpenGL.
# On RK, SDL can discover evdev devices through the SDK's libudev without using KMSDRM.
set(_sdl_libudev OFF)
if(ZT_PLATFORM STREQUAL "rk3566")
    set(_sdl_libudev ON)
endif()
zt_dependency(sdl SDL paho-cpp
    -DSDL_SHARED:BOOL=OFF -DSDL_STATIC:BOOL=ON -DSDL_TESTS:BOOL=OFF
    -DSDL_TEST_LIBRARY:BOOL=OFF -DSDL_UNIX_CONSOLE_BUILD:BOOL=ON
    -DSDL_X11:BOOL=${ZT_ENABLE_PREVIEW} -DSDL_WAYLAND:BOOL=${ZT_ENABLE_PREVIEW} -DSDL_KMSDRM:BOOL=OFF
    -DSDL_OPENGL:BOOL=${ZT_ENABLE_PREVIEW} -DSDL_OPENGLES:BOOL=OFF -DSDL_VULKAN:BOOL=OFF
    -DSDL_GPU:BOOL=OFF -DSDL_AUDIO:BOOL=OFF -DSDL_CAMERA:BOOL=OFF
    -DSDL_DBUS:BOOL=OFF -DSDL_IBUS:BOOL=OFF -DSDL_LIBURING:BOOL=OFF
    -DSDL_JOYSTICK:BOOL=OFF -DSDL_HAPTIC:BOOL=OFF -DSDL_HIDAPI:BOOL=OFF
    -DSDL_LIBUDEV:BOOL=${_sdl_libudev})

ExternalProject_Add(zenith-app
    SOURCE_DIR "${CMAKE_SOURCE_DIR}"
    BINARY_DIR "${CMAKE_BINARY_DIR}/app"
    DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
    CMAKE_ARGS ${_target} -DZT_SUPERBUILD:BOOL=OFF -DZT_PLATFORM:STRING=${ZT_PLATFORM}
        -DZT_ENABLE_DRM:BOOL=${ZT_ENABLE_DRM} -DZT_ENABLE_PREVIEW:BOOL=${ZT_ENABLE_PREVIEW}
        -DZT_VIDEO_BACKEND:STRING=${ZT_VIDEO_BACKEND}
        -DZT_ENABLE_VIDEO_TRANSPORT:BOOL=${ZT_ENABLE_VIDEO_TRANSPORT}
        -DBUILD_TESTING:BOOL=${BUILD_TESTING}
        -DZT_HOST_PROTOC:FILEPATH=${ZT_HOST_PREFIX}/bin/protoc
        -DZT_OUTPUT_DIR:PATH=${CMAKE_BINARY_DIR}/bin
        -DCMAKE_INSTALL_PREFIX:PATH=${CMAKE_BINARY_DIR}/install
    DEPENDS sdl
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel ${ZT_BUILD_JOBS}
    BUILD_ALWAYS TRUE)
add_custom_target(ZenithTerminal DEPENDS zenith-app)
add_custom_target(zenith_terminal DEPENDS zenith-app)
if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME platform-smoke COMMAND "${CMAKE_BINARY_DIR}/bin/ZenithTerminal" --self-check)
    set_tests_properties(platform-smoke PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "SELF-CHECK PASS")
endif()

if(ZT_ENABLE_DRM)
    add_custom_target(zenith_drm_probe DEPENDS zenith-app)
    if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
        add_test(NAME drm-tests COMMAND ${CMAKE_CTEST_COMMAND}
            --test-dir "${CMAKE_BINARY_DIR}/app" --output-on-failure -R "^drm-" --no-tests=error)
        set_tests_properties(drm-tests PROPERTIES TIMEOUT 30 LABELS unit)
    endif()
endif()

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING AND
   (ZT_ENABLE_VIDEO_TRANSPORT OR ZT_VIDEO_BACKEND STREQUAL "vaapi"))
    add_test(NAME video-tests COMMAND ${CMAKE_CTEST_COMMAND}
        --test-dir "${CMAKE_BINARY_DIR}/app" --output-on-failure -R "^video-" --no-tests=error)
    set_tests_properties(video-tests PROPERTIES LABELS unit TIMEOUT 30)
endif()
