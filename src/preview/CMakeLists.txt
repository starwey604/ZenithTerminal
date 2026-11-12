add_library(zenith_preview STATIC preview.cpp)
target_include_directories(zenith_preview PUBLIC include)
target_link_libraries(zenith_preview PRIVATE SDL3::SDL3-static)
target_compile_options(zenith_preview PRIVATE -Wall -Wextra -Wpedantic)
