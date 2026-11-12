add_library(zenith_video STATIC video.cpp)
target_include_directories(zenith_video PUBLIC include)
target_compile_options(zenith_video PRIVATE -Wall -Wextra -Wpedantic)
