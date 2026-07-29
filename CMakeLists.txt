cmake_minimum_required(VERSION 3.20)
project(crossword_filler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Enable vectorization-friendly optimizations in Release builds.
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")

add_library(xfill
  src/grid.cpp
  src/dictionary.cpp
  src/solver.cpp
  src/heuristics.cpp
)
target_include_directories(xfill PUBLIC include)

add_executable(xfill_cli src/main.cpp)
target_link_libraries(xfill_cli PRIVATE xfill)

option(XFILL_BUILD_TESTS "Build unit tests" ON)
if(XFILL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests/archive_v1)
endif()
