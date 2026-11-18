add_library(zenith_core STATIC infantry.cpp)
target_include_directories(zenith_core PUBLIC include)
target_compile_options(zenith_core PRIVATE -Wall -Wextra -Wpedantic)
