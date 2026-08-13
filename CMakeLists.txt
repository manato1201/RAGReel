cmake_minimum_required(VERSION 3.24)
project(VideoFactory LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Needed at this (top) level, not just inside engine/CMakeLists.txt, for
# `ctest` run from build/ to see engine/'s registered tests (e.g.
# resource_budget_manager_test, IMPROVEMENT_PLAN.md Phase 1) -- CTest's
# top-level test aggregator file is only generated when enable_testing()
# has been called at the directory ctest itself is invoked from.
enable_testing()

add_subdirectory(engine)
