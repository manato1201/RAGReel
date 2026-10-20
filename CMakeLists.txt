cmake_minimum_required(VERSION 3.24)
project(VideoFactory LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# C++ exception handling with proper stack unwinding. With the vcpkg toolchain's flags the compile
# command had no /EHsc at all (MSVC warning C4530), so a `throw` skipped the destructors of the locals
# on the way out. That crashed the engine when a network call timed out inside httpCall(): the
# QNetworkAccessManager/QEventLoop of the failed call stayed alive on a dead stack frame and the next
# call's event loop delivered the reply to it (found 2026-10-19). Must be set for every target.
if(MSVC)
    add_compile_options(/EHsc)
endif()

# Needed at this (top) level, not just inside engine/CMakeLists.txt, for
# `ctest` run from build/ to see engine/'s registered tests (e.g.
# resource_budget_manager_test, IMPROVEMENT_PLAN.md Phase 1) -- CTest's
# top-level test aggregator file is only generated when enable_testing()
# has been called at the directory ctest itself is invoked from.
enable_testing()

add_subdirectory(engine)
