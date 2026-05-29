#ifndef DBMS_AUTH_TYPES_HPP
#define DBMS_AUTH_TYPES_HPP

#include <chrono>
#include <cstdint>
#include <string>

namespace dbms::auth {

enum class Privilege : uint8_t {
    READ = 1 << 0,
    WRITE = 1 << 1,
    CREATE_TABLE = 1 << 2,
    DROP_TABLE = 1 << 3,
    DROP_DB = 1 << 4,
    CONNECT = 1 << 5,
    ADMIN = 1 << 6
};

using PrivilegeMask = uint8_t;

inline PrivilegeMask operator|(PrivilegeMask a, Privilege b) {
    return static_cast<PrivilegeMask>(a | static_cast<uint8_t>(b));
}

inline PrivilegeMask operator|(Privilege a, Privilege b) {
    return static_cast<PrivilegeMask>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline bool has_privilege(PrivilegeMask mask, Privilege p) {
    return (mask & static_cast<uint8_t>(p)) != 0;
}

inline PrivilegeMask parse_privilege_name(const std::string& name) {
    if (name == "read") {
        return static_cast<PrivilegeMask>(Privilege::READ);
    }
    if (name == "write") {
        return static_cast<PrivilegeMask>(Privilege::WRITE);
    }
    if (name == "create_table") {
        return static_cast<PrivilegeMask>(Privilege::CREATE_TABLE);
    }
    if (name == "drop_table") {
        return static_cast<PrivilegeMask>(Privilege::DROP_TABLE);
    }
    if (name == "drop_db") {
        return static_cast<PrivilegeMask>(Privilege::DROP_DB);
    }
    if (name == "connect") {
        return static_cast<PrivilegeMask>(Privilege::CONNECT);
    }
    if (name == "admin") {
        return static_cast<PrivilegeMask>(Privilege::ADMIN);
    }
    return 0;
}

struct ClientSession {
    std::string username;
    std::string jwt_token;
    bool token_set = false;
    bool authenticated = false;
};

} // namespace dbms::auth

#endif
