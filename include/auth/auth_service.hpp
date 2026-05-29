#ifndef DBMS_AUTH_SERVICE_HPP
#define DBMS_AUTH_SERVICE_HPP

#include "account_store.hpp"
#include "jwt.hpp"
#include "permissions.hpp"
#include "../parser.hpp"
#include "../json.hpp"
#include <algorithm>
#include <stdexcept>
#include <string>

namespace dbms::auth {

using json = nlohmann::json;

class AuthService {
public:
    AuthService(const std::string& auth_dir = "data/_auth")
        : store_(auth_dir), jwt_(auth_dir + "/jwt_secret"), checker_(store_) {}

    AccountStore& store() { return store_; }
    PermissionChecker& checker() { return checker_; }

    json login(const std::string& username, const std::string& password) {
        if (!store_.verify_password(username, password)) {
            throw std::runtime_error("Invalid username or password");
        }
        const std::string token = jwt_.issue_token(username);
        json j;
        j["token"] = token;
        j["expires_in"] = kJwtDefaultTtlSeconds;
        j["username"] = username;
        return j;
    }

    void set_token(ClientSession& session, const std::string& token) {
        auto user = jwt_.verify_token(token);
        if (!user) {
            throw std::runtime_error("Invalid or expired token");
        }
        if (!store_.user_exists(*user)) {
            throw std::runtime_error("User no longer exists");
        }
        session.username = *user;
        session.jwt_token = token;
        session.token_set = true;
        session.authenticated = true;
    }

    static bool require_auth(const ClientSession& session) {
        return session.authenticated && session.token_set;
    }

    void require_superuser(const ClientSession& session) const {
        if (!require_auth(session)) {
            throw std::runtime_error("Authentication required. Use LOGIN and SET TOKEN");
        }
        if (!checker_.is_superuser(session.username)) {
            throw std::runtime_error("Permission denied: admin required");
        }
    }

    json handle_auth_command(const SQLParser::ParsedQuery& q, ClientSession& session) {
        switch (q.type) {
            case SQLParser::ParsedQuery::LOGIN:
                return login(q.auth_username, q.auth_password);
            case SQLParser::ParsedQuery::SET_TOKEN:
                set_token(session, q.auth_token);
                return json{{"message", "Token accepted"}, {"username", session.username}};
            case SQLParser::ParsedQuery::CREATE_USER:
                require_superuser(session);
                store_.create_user(q.auth_username, q.auth_password);
                store_.save();
                return json{{"message", "User created"}, {"username", q.auth_username}};
            case SQLParser::ParsedQuery::DROP_USER:
                require_superuser(session);
                store_.drop_user(q.auth_username);
                return json{{"message", "User dropped"}};
            case SQLParser::ParsedQuery::CREATE_GROUP:
                require_superuser(session);
                store_.create_group(q.auth_group);
                store_.save();
                return json{{"message", "Group created"}, {"group", q.auth_group}};
            case SQLParser::ParsedQuery::DROP_GROUP:
                require_superuser(session);
                store_.drop_group(q.auth_group);
                return json{{"message", "Group dropped"}};
            case SQLParser::ParsedQuery::ADD_USER_TO_GROUP:
                require_superuser(session);
                store_.add_user_to_group(q.auth_username, q.auth_group);
                store_.save();
                return json{{"message", "User added to group"}};
            case SQLParser::ParsedQuery::REMOVE_USER_FROM_GROUP:
                require_superuser(session);
                store_.remove_user_from_group(q.auth_username, q.auth_group);
                store_.save();
                return json{{"message", "User removed from group"}};
            case SQLParser::ParsedQuery::GRANT:
                require_superuser(session);
                apply_grant(q);
                store_.save();
                return json{{"message", "Granted"}};
            case SQLParser::ParsedQuery::REVOKE:
                require_superuser(session);
                apply_revoke(q);
                store_.save();
                return json{{"message", "Revoked"}};
            case SQLParser::ParsedQuery::SHOW_GRANTS:
                if (!require_auth(session)) {
                    throw std::runtime_error("Authentication required");
                }
                return store_.show_grants(q.auth_username, q.database_name);
            default:
                throw std::runtime_error("Not an auth command");
        }
    }

    bool is_auth_command(SQLParser::ParsedQuery::Type t) const {
        switch (t) {
            case SQLParser::ParsedQuery::LOGIN:
            case SQLParser::ParsedQuery::SET_TOKEN:
            case SQLParser::ParsedQuery::CREATE_USER:
            case SQLParser::ParsedQuery::DROP_USER:
            case SQLParser::ParsedQuery::CREATE_GROUP:
            case SQLParser::ParsedQuery::DROP_GROUP:
            case SQLParser::ParsedQuery::ADD_USER_TO_GROUP:
            case SQLParser::ParsedQuery::REMOVE_USER_FROM_GROUP:
            case SQLParser::ParsedQuery::GRANT:
            case SQLParser::ParsedQuery::REVOKE:
            case SQLParser::ParsedQuery::SHOW_GRANTS:
                return true;
            default:
                return false;
        }
    }

private:
    AccountStore store_;
    JwtAuth jwt_;
    PermissionChecker checker_;

    void apply_grant(const SQLParser::ParsedQuery& q) {
        auto& perms = store_.mutable_db_perms(q.database_name);
        PrivilegeMask mask = q.grant_mask;
        if (q.grant_target == "default") {
            if (q.grant_principal == "users") {
                perms.default_user_mask |= mask;
            } else if (q.grant_principal == "groups") {
                perms.default_group_mask |= mask;
            }
            return;
        }
        if (q.grant_on_table) {
            auto& tg = perms.table_grants[q.table_name];
            if (q.grant_principal == "user") {
                tg.user_grants[q.grant_target] |= mask;
            } else {
                tg.group_grants[q.grant_target] |= mask;
            }
        } else if (q.grant_principal == "user") {
            perms.user_grants[q.grant_target] |= mask;
        } else {
            perms.group_grants[q.grant_target] |= mask;
        }
    }

    void apply_revoke(const SQLParser::ParsedQuery& q) {
        auto& perms = store_.mutable_db_perms(q.database_name);
        PrivilegeMask mask = q.grant_mask;
        if (q.grant_target == "default") {
            if (q.grant_principal == "users") {
                perms.default_user_mask &= ~mask;
            } else if (q.grant_principal == "groups") {
                perms.default_group_mask &= ~mask;
            }
            return;
        }
        if (q.grant_on_table) {
            auto it = perms.table_grants.find(q.table_name);
            if (it == perms.table_grants.end()) {
                return;
            }
            if (q.grant_principal == "user") {
                it->second.user_grants[q.grant_target] &= ~mask;
            } else {
                it->second.group_grants[q.grant_target] &= ~mask;
            }
        } else if (q.grant_principal == "user") {
            perms.user_grants[q.grant_target] &= ~mask;
        } else {
            perms.group_grants[q.grant_target] &= ~mask;
        }
    }
};

} // namespace dbms::auth

#endif
