cmake_minimum_required(VERSION 3.14)
project(DBMS VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MINGW)
    add_definitions(-D_WIN32_WINNT=0x0A00)
endif()

include(FetchContent)
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

set(COMMON_LIBS "")
if(WIN32)
    if(MINGW)
        list(APPEND COMMON_LIBS ws2_32 stdc++fs)
    else()
        list(APPEND COMMON_LIBS ws2_32)
    endif()
endif()

add_executable(server src/macos/server.cpp)
target_include_directories(server PRIVATE include)
target_link_libraries(server PRIVATE ${COMMON_LIBS} nlohmann_json::nlohmann_json)

add_executable(client src/macos/client.cpp)
target_include_directories(client PRIVATE include)
target_link_libraries(client PRIVATE ${COMMON_LIBS} nlohmann_json::nlohmann_json)