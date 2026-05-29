#ifndef DBMS_AUTH_PERMISSIONS_HPP
#define DBMS_AUTH_PERMISSIONS_HPP

#include "account_store.hpp"
#include "auth_types.hpp"
#include "../parser.hpp"

namespace dbms::auth {

class PermissionChecker {
public:
    explicit PermissionChecker(AccountStore& store) : store_(store) {}

    bool is_superuser(const std::string& username) const { return store_.is_superuser(username); }

    bool can(const std::string& username, const std::string& db, const std::string& table, Privilege priv) const {
        if (store_.is_superuser(username)) {
            return true;
        }

        const DbPermissions* perms = store_.db_perms(db);
        PrivilegeMask mask = 0;
        if (perms) {
            mask |= perms->default_user_mask;
            mask |= perms->default_group_mask;
            auto ug = perms->user_grants.find(username);
            if (ug != perms->user_grants.end()) {
                mask |= ug->second;
            }
            for (const auto& g : store_.user_groups(username)) {
                auto gg = perms->group_grants.find(g);
                if (gg != perms->group_grants.end()) {
                    mask |= gg->second;
                }
            }
            if (!table.empty()) {
                auto tit = perms->table_grants.find(table);
                if (tit != perms->table_grants.end()) {
                    auto tu = tit->second.user_grants.find(username);
                    if (tu != tit->second.user_grants.end()) {
                        mask |= tu->second;
                    }
                    for (const auto& g : store_.user_groups(username)) {
                        auto tg = tit->second.group_grants.find(g);
                        if (tg != tit->second.group_grants.end()) {
                            mask |= tg->second;
                        }
                    }
                }
            }
        }
        return has_privilege(mask, priv);
    }

    bool check_query(const std::string& username, const SQLParser::ParsedQuery& q,
                     const std::string& current_db) const {
        if (store_.is_superuser(username)) {
            return true;
        }

        switch (q.type) {
            case SQLParser::ParsedQuery::SELECT_OP:
                return can(username, current_db, q.table_name, Privilege::READ);
            case SQLParser::ParsedQuery::INSERT_OP:
            case SQLParser::ParsedQuery::UPDATE_OP:
            case SQLParser::ParsedQuery::DELETE_OP:
                return can(username, current_db, q.table_name, Privilege::WRITE);
            case SQLParser::ParsedQuery::CREATE_TABLE:
                return can(username, current_db, "", Privilege::CREATE_TABLE);
            case SQLParser::ParsedQuery::DROP_TABLE:
                return can(username, current_db, q.table_name, Privilege::DROP_TABLE);
            case SQLParser::ParsedQuery::DROP_DB:
                return can(username, q.database_name, "", Privilege::DROP_DB);
            case SQLParser::ParsedQuery::USE:
                return can(username, q.database_name, "", Privilege::CONNECT);
            case SQLParser::ParsedQuery::CREATE_DB:
            case SQLParser::ParsedQuery::REVERT_OP:
                return can(username, current_db.empty() ? q.database_name : current_db, q.table_name,
                           Privilege::ADMIN);
            default:
                return false;
        }
    }

private:
    AccountStore& store_;
};

} // namespace dbms::auth

#endif
