add_library(zenith_ui STATIC ui.cpp infantry_hud.cpp)
target_include_directories(zenith_ui PUBLIC include)
target_link_libraries(zenith_ui PUBLIC zenith_core PRIVATE SDL3::SDL3-static)
target_compile_options(zenith_ui PRIVATE -Wall -Wextra -Wpedantic)
