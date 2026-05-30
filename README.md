# DBMS

Учебная СУБД с TCP-сервером, CLI-клиентом и SQL-подобным языком запросов.

## Требования

- **CMake** 3.14+
- **Компилятор C++17**: GCC, Clang
- **Git** (для загрузки зависимости [nlohmann/json](https://github.com/nlohmann/json) через CMake FetchContent)

Данные по умолчанию сохраняются в каталог `./data`.

---

## macOS

```bash
# Обычная сборка
cmake -S . -B build
cmake --build build
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

## Пример сценария

В корне репозитория есть `script.txt`. Сначала запустите сервер, затем клиент:

```bash
./build/client script.txt        # macOS / Linux
```

В интерактивном режиме вводите SQL с `;` в конце строки; для выхода: `exit;`.