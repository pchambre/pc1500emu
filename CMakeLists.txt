cmake_minimum_required(VERSION 3.16)
project(pc1500emu VERSION 1.3.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_subdirectory(src)
# pc1500runner: runs a machine-code program headlessly with a key script
# (used by sdcc-pc1500's LH5801 regression tests)
add_subdirectory(tools/runner)

enable_testing()
add_subdirectory(tests)
