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
| macOS, Linux     | `src/macos/server.cpp`, `src/macos/client.cpp` (POSIX-сокеты) |

Платформа выбирается автоматически в `cmake/PlatformSources.cmake`.

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

## Linux

Сборка и запуск такие же, как на macOS (используются те же POSIX-исходники):

```bash
cmake -S . -B build
cmake --build build

./build/server
./build/client script.txt
```

---

## Пример сценария

В корне репозитория есть `script.txt`. Сначала запустите сервер, затем клиент:

```bash
./build/client script.txt        # macOS / Linux
```

В интерактивном режиме вводите SQL с `;` в конце строки; для выхода: `exit;`.

## Аутентификация и RBAC

Подробности реализации: [docs/rbac-auth.md](docs/rbac-auth.md).

При первом запуске создаётся пользователь **`admin`** с паролем **`admin`**.

1. Войти: `LOGIN admin "admin";` — в ответе JWT.
2. Один раз за TCP-сессию: `SET TOKEN <jwt>;` (интерактивный клиент отправляет это автоматически после `LOGIN`).
3. Далее — обычные SQL-запросы (нужны права на операцию).

### Управление пользователями и группами (суперпользователь)

```sql
CREATE USER bob "password";
DROP USER bob;
CREATE GROUP editors;
DROP GROUP editors;
ADD USER bob TO GROUP editors;
REMOVE USER bob FROM GROUP editors;
```

### Права на базы и таблицы

```sql
GRANT READ, WRITE ON DATABASE mydb TO DEFAULT FOR USERS;
GRANT READ ON TABLE mydb.users TO USER alice;
GRANT CREATE_TABLE ON DATABASE mydb TO GROUP editors;
REVOKE WRITE ON TABLE mydb.users FROM USER bob;
SHOW GRANTS FOR USER alice ON DATABASE mydb;
```

Привилегии: `READ`, `WRITE`, `CREATE_TABLE`, `DROP_TABLE`, `DROP_DB`, `CONNECT`, `ADMIN`.

Служебные команды (`TELEMETRY`, `STATS`, `ROTATE`) требуют роль администратора (суперпользователь или группа `admin`).

## Устранение неполадок

- **Порт занят** — укажите другой: `./build/server 9000`
- **Старая БД в `data/`** — удалите каталог для чистого старта: `rm -rf data` (macOS/Linux)
- **Первая конфигурация CMake долго идёт** — скачивается nlohmann/json; нужен доступ в интернет
