# Пресет CMake для сборки на macOS (подключается до project()).
#
#   cmake -S . -B build-macos -C cmake/macos.cmake
#   cmake --build build-macos
#
# Windows по-прежнему: cmake -S . -B build-win  (на MinGW/MSVC подхватятся src/server.cpp и src/client.cpp)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
set(CMAKE_CXX_STANDARD 17 CACHE STRING "")
set(CMAKE_CXX_STANDARD_REQUIRED ON CACHE BOOL "")

if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}" CACHE STRING "macOS target architectures")
endif()

message(STATUS "macOS preset: deployment target ${CMAKE_OSX_DEPLOYMENT_TARGET}, arch ${CMAKE_OSX_ARCHITECTURES}")
