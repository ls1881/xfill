include(FetchContent)
FetchContent_Declare(
  catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.5.4
)
FetchContent_MakeAvailable(catch2)

add_executable(xfill_tests
  test_grid.cpp
  test_dictionary.cpp
  test_solver.cpp
)
target_link_libraries(xfill_tests PRIVATE xfill Catch2::Catch2WithMain)

include(CTest)
include(Catch)
catch_discover_tests(xfill_tests)
