cmake_minimum_required(VERSION 3.14)
project(DBMS VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MINGW)
    add_definitions(-D_WIN32_WINNT=0x0A00)
endif()

# nlohmann/json
include(FetchContent)
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

set(COMMON_LIBS nlohmann_json::nlohmann_json)

if(MINGW)
    set(COMMON_LIBS ${COMMON_LIBS} ws2_32 stdc++fs)
endif()

# Сервер с логированием
add_executable(server src/server.cpp)
target_include_directories(server PRIVATE include)
target_link_libraries(server ${COMMON_LIBS})

# Клиент
add_executable(client src/client.cpp)
target_link_libraries(client ${COMMON_LIBS})
