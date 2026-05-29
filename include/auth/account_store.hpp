#ifndef DBMS_AUTH_ACCOUNT_STORE_HPP
#define DBMS_AUTH_ACCOUNT_STORE_HPP

#include "auth_types.hpp"
#include "crypto.hpp"
#include "../json.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace dbms::auth {

using json = nlohmann::json;
namespace fs = std::filesystem;

struct UserAccount {
    std::string username;
    std::string salt_hex;
    std::string password_hash;
    uint32_t pbkdf2_iterations = kPbkdf2Iterations;
    std::vector<std::string> groups;
    bool is_superuser = false;
};

struct Group {
    std::string name;
};

struct TableGrants {
    std::unordered_map<std::string, PrivilegeMask> user_grants;
    std::unordered_map<std::string, PrivilegeMask> group_grants;
};

struct DbPermissions {
    PrivilegeMask default_user_mask = 0;
    PrivilegeMask default_group_mask = 0;
    std::unordered_map<std::string, PrivilegeMask> user_grants;
    std::unordered_map<std::string, PrivilegeMask> group_grants;
    std::unordered_map<std::string, TableGrants> table_grants;
};

class AccountStore {
public:
    explicit AccountStore(const std::string& auth_dir = "data/_auth")
        : auth_dir_(auth_dir),
          users_file_(auth_dir + "/users.json"),
          permissions_file_(auth_dir + "/permissions.json") {
        fs::create_directories(auth_dir_);
        load();
        if (users_.empty()) {
            create_user("admin", "admin", true);
            ensure_group("admin");
            add_user_to_group("admin", "admin");
            save();
        }
    }

    bool user_exists(const std::string& username) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return users_.count(username) > 0;
    }

    bool group_exists(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return groups_.count(name) > 0;
    }

    bool verify_password(const std::string& username, const std::string& password) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = users_.find(username);
        if (it == users_.end()) {
            return false;
        }
        return auth::verify_password(password, it->second.salt_hex, it->second.password_hash,
                                     it->second.pbkdf2_iterations);
    }

    bool is_superuser(const std::string& username) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = users_.find(username);
        if (it == users_.end()) {
            return false;
        }
        if (it->second.is_superuser) {
            return true;
        }
        for (const auto& g : it->second.groups) {
            if (g == "admin") {
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> user_groups(const std::string& username) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = users_.find(username);
        if (it == users_.end()) {
            return {};
        }
        return it->second.groups;
    }

    void create_user(const std::string& username, const std::string& password, bool superuser = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (users_.count(username)) {
            throw std::runtime_error("User already exists: " + username);
        }
        auto salt = random_bytes(kSaltLen);
        UserAccount acc;
        acc.username = username;
        acc.salt_hex = bytes_to_hex(salt.data(), salt.size());
        acc.password_hash = hash_password(password, salt, kPbkdf2Iterations);
        acc.pbkdf2_iterations = kPbkdf2Iterations;
        acc.is_superuser = superuser;
        users_[username] = std::move(acc);
        save_unlocked();
    }

    void drop_user(const std::string& username) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!users_.count(username)) {
            throw std::runtime_error("User not found: " + username);
        }
        users_.erase(username);
        for (auto& [db, perms] : db_permissions_) {
            perms.user_grants.erase(username);
            for (auto& [tbl, tg] : perms.table_grants) {
                tg.user_grants.erase(username);
            }
        }
        save_unlocked();
    }

    void ensure_group(const std::string& name) {
        if (!groups_.count(name)) {
            groups_[name] = Group{name};
        }
    }

    void create_group(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (groups_.count(name)) {
            throw std::runtime_error("Group already exists: " + name);
        }
        groups_[name] = Group{name};
        save_unlocked();
    }

    void drop_group(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!groups_.count(name)) {
            throw std::runtime_error("Group not found: " + name);
        }
        groups_.erase(name);
        for (auto& [_, user] : users_) {
            user.groups.erase(std::remove(user.groups.begin(), user.groups.end(), name), user.groups.end());
        }
        for (auto& [db, perms] : db_permissions_) {
            perms.group_grants.erase(name);
            for (auto& [tbl, tg] : perms.table_grants) {
                tg.group_grants.erase(name);
            }
        }
        save_unlocked();
    }

    void add_user_to_group(const std::string& username, const std::string& group) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!users_.count(username)) {
            throw std::runtime_error("User not found: " + username);
        }
        if (!groups_.count(group)) {
            throw std::runtime_error("Group not found: " + group);
        }
        auto& gs = users_[username].groups;
        if (std::find(gs.begin(), gs.end(), group) == gs.end()) {
            gs.push_back(group);
        }
        save_unlocked();
    }

    void remove_user_from_group(const std::string& username, const std::string& group) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!users_.count(username)) {
            throw std::runtime_error("User not found: " + username);
        }
        auto& gs = users_[username].groups;
        gs.erase(std::remove(gs.begin(), gs.end(), group), gs.end());
        save_unlocked();
    }

    DbPermissions& permissions_for_db(const std::string& db) {
        return db_permissions_[db];
    }

    const DbPermissions* db_perms(const std::string& db) const {
        auto it = db_permissions_.find(db);
        if (it == db_permissions_.end()) {
            return nullptr;
        }
        return &it->second;
    }

    DbPermissions& mutable_db_perms(const std::string& db) {
        return db_permissions_[db];
    }

    void save() {
        std::lock_guard<std::mutex> lock(mutex_);
        save_unlocked();
    }

    json show_grants(const std::string& username, const std::string& db) const {
        std::lock_guard<std::mutex> lock(mutex_);
        json result;
        result["user"] = username;
        result["database"] = db;
        if (!users_.count(username)) {
            result["error"] = "user not found";
            return result;
        }
        auto it = db_permissions_.find(db);
        if (it == db_permissions_.end()) {
            result["message"] = "no grants for database";
            return result;
        }
        const auto& p = it->second;
        result["default_user_mask"] = static_cast<int>(p.default_user_mask);
        result["default_group_mask"] = static_cast<int>(p.default_group_mask);
        if (p.user_grants.count(username)) {
            result["user_mask"] = static_cast<int>(p.user_grants.at(username));
        }
        json tables = json::object();
        for (const auto& [tbl, tg] : p.table_grants) {
            if (tg.user_grants.count(username)) {
                tables[tbl] = static_cast<int>(tg.user_grants.at(username));
            }
        }
        result["tables"] = tables;
        json groups_json = json::object();
        for (const auto& g : users_.at(username).groups) {
            if (p.group_grants.count(g)) {
                groups_json[g] = static_cast<int>(p.group_grants.at(g));
            }
        }
        result["groups"] = groups_json;
        return result;
    }

private:
    std::string auth_dir_;
    std::string users_file_;
    std::string permissions_file_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, UserAccount> users_;
    std::unordered_map<std::string, Group> groups_;
    std::unordered_map<std::string, DbPermissions> db_permissions_;

    void load() {
        load_users();
        load_permissions();
    }

    void load_users() {
        std::ifstream in(users_file_);
        if (!in) {
            return;
        }
        json j;
        in >> j;
        if (!j.contains("users")) {
            return;
        }
        for (auto& [name, u] : j["users"].items()) {
            UserAccount acc;
            acc.username = name;
            acc.salt_hex = u.value("salt", "");
            acc.password_hash = u.value("hash", "");
            acc.pbkdf2_iterations = u.value("iterations", kPbkdf2Iterations);
            acc.is_superuser = u.value("is_superuser", false);
            if (u.contains("groups")) {
                for (const auto& g : u["groups"]) {
                    acc.groups.push_back(g.get<std::string>());
                }
            }
            users_[name] = std::move(acc);
        }
        if (j.contains("groups")) {
            for (const auto& g : j["groups"]) {
                std::string gn = g.get<std::string>();
                groups_[gn] = Group{gn};
            }
        }
    }

    void load_permissions() {
        std::ifstream in(permissions_file_);
        if (!in) {
            return;
        }
        json j;
        in >> j;
        if (!j.contains("permissions")) {
            return;
        }
        for (auto& [db, pd] : j["permissions"].items()) {
            DbPermissions perms;
            perms.default_user_mask = static_cast<PrivilegeMask>(pd.value("default_user", 0));
            perms.default_group_mask = static_cast<PrivilegeMask>(pd.value("default_group", 0));
            if (pd.contains("users")) {
                for (auto& [u, m] : pd["users"].items()) {
                    perms.user_grants[u] = static_cast<PrivilegeMask>(m.get<int>());
                }
            }
            if (pd.contains("groups")) {
                for (auto& [g, m] : pd["groups"].items()) {
                    perms.group_grants[g] = static_cast<PrivilegeMask>(m.get<int>());
                }
            }
            if (pd.contains("tables")) {
                for (auto& [tbl, td] : pd["tables"].items()) {
                    TableGrants tg;
                    if (td.contains("users")) {
                        for (auto& [u, m] : td["users"].items()) {
                            tg.user_grants[u] = static_cast<PrivilegeMask>(m.get<int>());
                        }
                    }
                    if (td.contains("groups")) {
                        for (auto& [g, m] : td["groups"].items()) {
                            tg.group_grants[g] = static_cast<PrivilegeMask>(m.get<int>());
                        }
                    }
                    perms.table_grants[tbl] = std::move(tg);
                }
            }
            db_permissions_[db] = std::move(perms);
        }
    }

    void save_unlocked() {
        json users_j;
        users_j["groups"] = json::array();
        for (const auto& [name, _] : groups_) {
            users_j["groups"].push_back(name);
        }
        users_j["users"] = json::object();
        for (const auto& [name, u] : users_) {
            json ju;
            ju["salt"] = u.salt_hex;
            ju["hash"] = u.password_hash;
            ju["iterations"] = u.pbkdf2_iterations;
            ju["is_superuser"] = u.is_superuser;
            ju["groups"] = u.groups;
            users_j["users"][name] = ju;
        }
        write_atomic(users_file_, users_j.dump(2));

        json perm_j;
        perm_j["permissions"] = json::object();
        for (const auto& [db, p] : db_permissions_) {
            json pd;
            pd["default_user"] = static_cast<int>(p.default_user_mask);
            pd["default_group"] = static_cast<int>(p.default_group_mask);
            pd["users"] = json::object();
            for (const auto& [u, m] : p.user_grants) {
                pd["users"][u] = static_cast<int>(m);
            }
            pd["groups"] = json::object();
            for (const auto& [g, m] : p.group_grants) {
                pd["groups"][g] = static_cast<int>(m);
            }
            pd["tables"] = json::object();
            for (const auto& [tbl, tg] : p.table_grants) {
                json td;
                td["users"] = json::object();
                for (const auto& [u, m] : tg.user_grants) {
                    td["users"][u] = static_cast<int>(m);
                }
                td["groups"] = json::object();
                for (const auto& [g, m] : tg.group_grants) {
                    td["groups"][g] = static_cast<int>(m);
                }
                pd["tables"][tbl] = td;
            }
            perm_j["permissions"][db] = pd;
        }
        write_atomic(permissions_file_, perm_j.dump(2));
    }

    static void write_atomic(const std::string& path, const std::string& content) {
        const std::string tmp = path + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out) {
                throw std::runtime_error("Cannot write " + tmp);
            }
            out << content;
        }
        fs::rename(tmp, path);
    }
};

} // namespace dbms::auth

#endif
