# DBMS

Учебная СУБД с TCP-сервером, CLI-клиентом и SQL-подобным языком запросов.

## Требования

- **CMake** 3.14+
- **Компилятор C++17**: GCC, Clang или MSVC / MinGW-w64
- **Git** (для загрузки зависимости [nlohmann/json](https://github.com/nlohmann/json) через CMake FetchContent)

Данные по умолчанию сохраняются в каталог `./data`.

## Структура по платформам

| Платформа        | Исходники сервера/клиента   |
|------------------|-----------------------------|
| Windows          | `src/server.cpp`, `src/client.cpp` (Winsock) |
| macOS, Linux     | `src/macos/server.cpp`, `src/macos/client.cpp` (POSIX-сокеты) |

Платформа выбирается автоматически в `cmake/PlatformSources.cmake`.

---

## macOS

```bash
# Обычная сборка
cmake -S . -B build
cmake --build build

# С пресетом (минимальная версия macOS 11.0, см. cmake/macos.cmake)
cmake -S . -B build-macos -C cmake/macos.cmake
cmake --build build-macos
```

### Запуск

Терминал 1 — сервер (порт по умолчанию **8080**, лог `access.log`):

```bash
./build/server
# или: ./build/server 9000 my.log
```

Терминал 2 — клиент:

```bash
./build/client                    # интерактивный режим, localhost:8080
./build/client script.txt         # выполнить скрипт
./build/client -h 127.0.0.1 -p 8080   # другой хост/порт
```

---

## Linux

Сборка и запуск такие же, как на macOS (используются те же POSIX-исходники):

```bash
cmake -S . -B build
cmake --build build

./build/server
./build/client script.txt
```

---

## Windows

Нужен **MinGW-w64** (рекомендуется) или **MSVC** с CMake.

### MinGW (рекомендуется)

В **MSYS2 MinGW64** или аналоге:

```bash
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja git

cmake -S . -B build-win -G "MinGW Makefiles"
cmake --build build-win
```

Запуск:

```cmd
build-win\server.exe
build-win\client.exe script.txt
```

### MSVC

Из **x64 Native Tools Command Prompt for VS**:

```cmd
cmake -S . -B build-msvc -A x64
cmake --build build-msvc --config Release

build-msvc\Release\server.exe
build-msvc\Release\client.exe script.txt
```

Параметры сервера: `server.exe [порт] [файл_лога]` (по умолчанию `8080` и `access.log`).

---

## Пример сценария

В корне репозитория есть `script.txt`. Сначала запустите сервер, затем клиент:

```bash
./build/client script.txt        # macOS / Linux
build-win\client.exe script.txt  # Windows
```

В интерактивном режиме вводите SQL с `;` в конце строки; для выхода: `exit;`.

## Устранение неполадок

- **Порт занят** — укажите другой: `./build/server 9000`
- **Старая БД в `data/`** — удалите каталог для чистого старта: `rm -rf data` (macOS/Linux) или `rmdir /s data` (Windows)
- **Первая конфигурация CMake долго идёт** — скачивается nlohmann/json; нужен доступ в интернет
