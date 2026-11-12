add_library(zenith_ui STATIC ui.cpp)
target_include_directories(zenith_ui PUBLIC include)
target_link_libraries(zenith_ui PRIVATE SDL3::SDL3-static)
target_compile_options(zenith_ui PRIVATE -Wall -Wextra -Wpedantic)
