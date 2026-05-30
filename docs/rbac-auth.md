# Подсистема аутентификации и RBAC

Документ описывает реализацию разграничения доступа и JWT-аутентификации в СУБД. 

Используется только стандартные библиотеки C++17.

## Обзор

| Компонент | Файл | Назначение |
|-----------|------|------------|
| Криптография | `include/auth/crypto.hpp` | SHA-256, HMAC-SHA256, PBKDF2, Base64URL, hex |
| JWT | `include/auth/jwt.hpp` | Выдача и проверка токенов HS256 |
| Типы и сессия | `include/auth/auth_types.hpp` | Привилегии, `ClientSession` |
| Аккаунты | `include/auth/account_store.hpp` | Пользователи, группы, права, JSON на диске |
| Авторизация | `include/auth/permissions.hpp` | Проверка effective privileges |
| Сервис | `include/auth/auth_service.hpp` | LOGIN, SET TOKEN, GRANT, CREATE USER, … |

Данные аккаунтов: `data/_auth/users.json`, `data/_auth/permissions.json`, секрет JWT: `data/_auth/jwt_secret`.

## Протокол клиента

1. **`LOGIN user "password";`** — без токена. Ответ: `{"token":"…","expires_in":…,"username":"…"}`.
2. **`SET TOKEN eyJ…;`** — **один раз за TCP-сессию** (повторный вызов заменяет токен). После успеха все SQL-команды авторизуются по JWT.
3. Остальные запросы требуют установленного токена (кроме `LOGIN`).

## Модель прав (RBAC)

### Привилегии (битовая маска `PrivilegeMask`, `uint8_t`)

| Бит | Имя | Операции |
|-----|-----|----------|
| 0 | `READ` | `SELECT` |
| 1 | `WRITE` | `INSERT`, `UPDATE`, `DELETE` |
| 2 | `CREATE_TABLE` | `CREATE TABLE` |
| 3 | `DROP_TABLE` | `DROP TABLE` |
| 4 | `DROP_DB` | `DROP DATABASE` |
| 5 | `CONNECT` | `USE database` |
| 6 | `ADMIN` | `CREATE DATABASE`, `REVERT`, служебные команды, управление пользователями и `GRANT`/`REVOKE` |

Суперпользователь (`is_superuser` или член группы `admin`) обходит проверки.

### Алгоритм проверки доступа

Вход: пользователь `u`, база `db`, опционально таблица `t`, требуемая привилегия `p`.

```
1. Если u — суперпользователь → ALLOW
2. mask ← 0
3. mask |= db_permissions[db].default_user_mask
4. mask |= db_permissions[db].default_group_mask
5. Для каждой группы g из u.groups:
       mask |= db_permissions[db].group_grants[g]
       если t задана: mask |= db_permissions[db].table_grants[t].group_grants[g]
6. mask |= db_permissions[db].user_grants[u]
       если t задана: mask |= db_permissions[db].table_grants[t].user_grants[u]
7. Если (mask & p) != 0 → ALLOW, иначе DENY
```

Сложность: **O(G)**, G — число групп пользователя. Поиск пользователя/группы — **O(1)** (`std::unordered_map`).

Право на операцию есть, если разрешают права по умолчанию для пользователей/групп, **хотя бы одна** группа пользователя, или **личная** выдача (`GRANT … TO USER`).

## Хранилище аккаунтов

### Структуры данных

```cpp
struct UserAccount {
    std::string username;
    std::string salt_hex;       // соль PBKDF2 (hex)
    std::string password_hash;  // производный ключ (hex), не plaintext
    uint32_t pbkdf2_iterations;
    std::vector<std::string> groups;  // членство только здесь
    bool is_superuser;
};

struct Group {
    std::string name;  // список members не хранится
};

struct TableGrants {
    std::unordered_map<std::string, PrivilegeMask> user_grants;
    std::unordered_map<std::string, PrivilegeMask> group_grants;
};

struct DbPermissions {
    PrivilegeMask default_user_mask;
    PrivilegeMask default_group_mask;
    std::unordered_map<std::string, PrivilegeMask> user_grants;
    std::unordered_map<std::string, PrivilegeMask> group_grants;
    std::unordered_map<std::string, TableGrants> table_grants;
};
```

### Алгоритм хэширования пароля

| Параметр | Значение |
|----------|----------|
| Алгоритм | **PBKDF2** с PRF **HMAC-SHA256** |
| Соль | 16 байт, `std::random_device` |
| Итерации | 100 000 (константа `kPbkdf2Iterations`) |
| Длина ключа | 32 байта |
| Формат в JSON | поля `salt`, `hash`, `iterations` (hex-строки) |

**Проверка:** пересчитать PBKDF2 с сохранённой солью и итерациями, сравнить с `password_hash` побайтно (защита от timing-атак через накопление XOR различий).

Пароли в открытом виде **не сохраняются**.

## JWT (HS256)

| Параметр | Значение |
|----------|----------|
| Алгоритм подписи | HMAC-SHA256 (`alg`: `HS256`) |
| Payload | `sub` (имя пользователя), `iat`, `exp` |
| Срок жизни по умолчанию | **30 суток** (`kJwtDefaultTtlSeconds`) |
| Секрет | 32 случайных байта в `data/_auth/jwt_secret` (hex) |

### Алгоритм выдачи

1. Проверить пароль через `AccountStore::verify_password`.
2. `header = {"alg":"HS256","typ":"JWT"}`.
3. `payload = {"sub":user,"iat":now,"exp":now+TTL}`.
4. `signature = HMAC-SHA256(secret, base64url(header) + "." + base64url(payload))`.
5. Токен = `base64url(header).base64url(payload).base64url(signature)`.

### Алгоритм проверки

1. Разбить на три части по `.`.
2. Проверить HMAC подписи (constant-time сравнение).
3. Декодировать payload JSON, проверить **`exp > now()`** (Unix time).
4. Вернуть `sub` как имя пользователя.

## Текущая БД и потоки

`StorageManager` использует **`thread_local std::string tls_current_db_`**: у каждого потока обработки клиента своя текущая БД после `USE`. Общий `Executor` остаётся один на сервер, конфликтов `USE` между клиентами нет.

Запись в `StorageManager` защищена **`std::mutex`** (`storage_mutex_`).

## Средства C++17

| Средство | Применение |
|----------|------------|
| `std::unordered_map`, `std::map` | Пользователи, группы, права |
| `std::vector`, `std::string` | Группы пользователя, членство |
| `std::optional` | Результат verify JWT, разбор GRANT |
| `enum class` + битовые `|`, `&` | `Privilege`, маски |
| `std::chrono::system_clock` | `iat`, `exp`, TTL |
| `std::random_device` | Соль, JWT secret |
| `thread_local` | Текущая БД на соединение |
| `std::mutex`, `std::lock_guard` | AccountStore, StorageManager |
| `std::filesystem` | Каталог `data/_auth/` |
| `nlohmann::json` | Сериализация аккаунтов (уже в проекте) |
| `std::fstream` | Атомарная запись через temp + rename |

## Команды управления доступом

См. также [README.md](../README.md#аутентификация-и-rbac).

```sql
LOGIN alice "secret";
SET TOKEN eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...;

CREATE USER bob "password";
DROP USER bob;
CREATE GROUP editors;
DROP GROUP editors;
ADD USER bob TO GROUP editors;
REMOVE USER bob FROM GROUP editors;

GRANT READ, WRITE ON DATABASE mydb TO DEFAULT FOR USERS;
GRANT READ ON TABLE mydb.users TO USER alice;
GRANT CREATE_TABLE ON DATABASE mydb TO GROUP editors;
REVOKE WRITE ON TABLE mydb.users FROM USER bob;

SHOW GRANTS FOR USER alice ON DATABASE mydb;
```

## Bootstrap

При первом запуске, если нет пользователей, создаётся **`admin`** / **`admin`** (суперпользователь). Пароль следует сменить в продакшене.

## Интеграция в сервер

```
recv query
  → parse
  → LOGIN / SET TOKEN → AuthService (без JWT)
  → CREATE USER, GRANT, … → JWT + ADMIN/superuser
  → SQL → JWT + PermissionChecker → Executor
  → TELEMETRY, … → JWT + ADMIN
```