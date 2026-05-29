cmake_minimum_required(VERSION 3.14)
project(DBMS VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Платформенные исходники (Windows vs POSIX/macOS)
include(cmake/PlatformSources.cmake)

if(MINGW)
    add_definitions(-D_WIN32_WINNT=0x0A00)
endif()

set(COMMON_LIBS "")

if(WIN32)
    if(MINGW)
        list(APPEND COMMON_LIBS ws2_32 stdc++fs)
    else()
        list(APPEND COMMON_LIBS ws2_32)
    endif()
endif()

# Сервер с логированием
add_executable(server ${DBMS_SERVER_SOURCE})
target_include_directories(server PRIVATE include)
target_link_libraries(server PRIVATE ${COMMON_LIBS})

# Клиент
add_executable(client ${DBMS_CLIENT_SOURCE})
target_link_libraries(client PRIVATE ${COMMON_LIBS})
