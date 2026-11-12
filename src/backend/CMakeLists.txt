# Reuse the pinned BUAA wire codec without bringing its BGR encoder/decoder into clients.
add_library(zenith_backend STATIC video_transport.cpp
    ${PROJECT_SOURCE_DIR}/3rdparty/BUAABackend/src/video/udp_header.cpp)
target_include_directories(zenith_backend PUBLIC include PRIVATE ${PROJECT_SOURCE_DIR}/3rdparty/BUAABackend/include)
target_link_libraries(zenith_backend PUBLIC zenith_video)
target_compile_options(zenith_backend PRIVATE -Wall -Wextra -Wpedantic)
if(BUILD_TESTING)
    add_executable(zenith_video_tests ${PROJECT_SOURCE_DIR}/tests/video_tests.cpp)
    target_link_libraries(zenith_video_tests PRIVATE zenith_backend)
    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME video-unit COMMAND zenith_video_tests)
    endif()
endif()
