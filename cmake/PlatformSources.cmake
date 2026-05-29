# Выбор исходников сервера/клиента по платформе.
# Windows: src/server.cpp, src/client.cpp (Winsock)
# macOS и прочий POSIX: src/macos/*.cpp

if(WIN32)
    set(DBMS_SERVER_SOURCE src/server.cpp)
    set(DBMS_CLIENT_SOURCE src/client.cpp)
else()
    set(DBMS_SERVER_SOURCE src/macos/server.cpp)
    set(DBMS_CLIENT_SOURCE src/macos/client.cpp)
endif()
