#ifndef DBMS_EXECUTOR_HPP
#define DBMS_EXECUTOR_HPP

#include "storage.hpp"
#include "parser.hpp"
#include "string_pool.hpp"
#include "auth/auth_types.hpp"
#include "auth/permissions.hpp"

#include <regex>
#include <map>
#include <set>
#include <string>
#include <memory>
#include <algorithm>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace dbms {

class Executor {
private:
    StorageManager storage_;



    DiskBPlusTree<int>* get_int_idx(const std::string& table, const std::string& col) {
        return storage_.get_index_int(table, col);
    }

    DiskBPlusTree<IndexKey>* get_str_idx(const std::string& table, const std::string& col) {
        return storage_.get_index_str(table, col);
    }



    bool evaluate_condition(const Row& row, const Condition& cond, const TableSchema& schema) {
        if (!cond.is_leaf()) {
            bool left_result  = cond.left  ? evaluate_condition(row, *cond.left,  schema) : true;
            bool right_result = cond.right ? evaluate_condition(row, *cond.right, schema) : true;
            return cond.is_and ? (left_result && right_result) : (left_result || right_result);
        }

        size_t col_idx = schema.get_column_index(cond.left_column);
        const Value& row_val = row.values[col_idx];

        auto compare = [&]() -> int {
            if (std::holds_alternative<NullType>(row_val)) return -2;

            if (std::holds_alternative<int>(row_val) &&
                std::holds_alternative<int>(cond.right_value)) {
                int a = std::get<int>(row_val);
                int b = std::get<int>(cond.right_value);
                return (a < b) ? -1 : ((a > b) ? 1 : 0);
            }
            if (std::holds_alternative<StringRef>(row_val) &&
                std::holds_alternative<StringRef>(cond.right_value)) {
                const std::string& a = *std::get<StringRef>(row_val);
                const std::string& b = *std::get<StringRef>(cond.right_value);
                return (a < b) ? -1 : ((a > b) ? 1 : 0);
            }
            return -2;
        };

        switch (cond.op) {
            case Condition::Op::EQ:  return compare() == 0;
            case Condition::Op::NEQ: return compare() != 0;
            case Condition::Op::LT:  return compare() < 0;
            case Condition::Op::GT:  return compare() > 0;
            case Condition::Op::LTE: return compare() <= 0;
            case Condition::Op::GTE: return compare() >= 0;

            case Condition::Op::BETWEEN: {
                auto cmp1 = [&]() -> bool {
                    if (std::holds_alternative<int>(row_val) &&
                        std::holds_alternative<int>(cond.right_value)) {
                        return std::get<int>(row_val) >= std::get<int>(cond.right_value);
                    }
                    if (std::holds_alternative<StringRef>(row_val) &&
                        std::holds_alternative<StringRef>(cond.right_value)) {
                        return *std::get<StringRef>(row_val) >=
                               *std::get<StringRef>(cond.right_value);
                    }
                    return false;
                };
                auto cmp2 = [&]() -> bool {
                    if (std::holds_alternative<int>(row_val) &&
                        std::holds_alternative<int>(cond.right_value2)) {
                        return std::get<int>(row_val) < std::get<int>(cond.right_value2);
                    }
                    if (std::holds_alternative<StringRef>(row_val) &&
                        std::holds_alternative<StringRef>(cond.right_value2)) {
                        return *std::get<StringRef>(row_val) <
                               *std::get<StringRef>(cond.right_value2);
                    }
                    return false;
                };
                return cmp1() && cmp2();
            }

            case Condition::Op::LIKE: {
                if (std::holds_alternative<StringRef>(row_val) &&
                    std::holds_alternative<StringRef>(cond.right_value)) {
                    std::regex pattern(*std::get<StringRef>(cond.right_value));
                    return std::regex_match(*std::get<StringRef>(row_val), pattern);
                }
                return false;
            }
            default: return false;
        }
    }



    std::vector<uint64_t> optimize_with_index(const TableSchema& schema,
                                              const Condition* cond) {
        if (!cond || !cond->is_leaf()) return {};

        const Column& col = schema.get_column(cond->left_column);
        if (!col.modifiers.indexed) return {};

        try {
            if (cond->op == Condition::Op::EQ) {
                if (col.type == ColumnType::INT &&
                    std::holds_alternative<int>(cond->right_value)) {
                    auto* tree = get_int_idx(schema.name, col.name);
                    auto res = tree->find(std::get<int>(cond->right_value));
                    if (res) return { *res };
                } else if (col.type == ColumnType::STRING &&
                           std::holds_alternative<StringRef>(cond->right_value)) {
                    auto* tree = get_str_idx(schema.name, col.name);
                    IndexKey key(std::get<StringRef>(cond->right_value));
                    auto res = tree->find(key);
                    if (res) return { *res };
                }
            }
            else if (cond->op == Condition::Op::BETWEEN) {
                if (col.type == ColumnType::INT &&
                    std::holds_alternative<int>(cond->right_value) &&
                    std::holds_alternative<int>(cond->right_value2)) {
                    auto* tree = get_int_idx(schema.name, col.name);
                    return tree->range_find(std::get<int>(cond->right_value),
                                            std::get<int>(cond->right_value2));
                } else if (col.type == ColumnType::STRING &&
                           std::holds_alternative<StringRef>(cond->right_value) &&
                           std::holds_alternative<StringRef>(cond->right_value2)) {
                    auto* tree = get_str_idx(schema.name, col.name);
                    IndexKey from(std::get<StringRef>(cond->right_value));
                    IndexKey to(std::get<StringRef>(cond->right_value2));
                    return tree->range_find(from, to);
                }
            }
        } catch (const std::exception&) {
            return {};
        }

        return {};
    }



    QueryResult create_message_result(const std::string& msg) {
        QueryResult result;
        result.column_names = {"message"};
        Row row;
        row.values.push_back(make_string_ref(msg));
        result.rows.push_back(row);
        return result;
    }

    QueryResult create_string_pool_stats() {
        QueryResult result;
        result.column_names = {"stats"};
        Row row;
        row.values.push_back(make_string_ref(string_pool_stats_json()));
        result.rows.push_back(row);
        return result;
    }

public:
    Executor() : storage_() {}

    static std::string string_pool_stats_json() {
        size_t unique_strings = 0, bytes_saved = 0, total_refs = 0;
        get_string_pool().get_stats(unique_strings, bytes_saved, total_refs);

        json j;
        j["unique_strings"]     = static_cast<long long>(unique_strings);
        j["bytes_saved"]        = static_cast<long long>(bytes_saved);
        j["total_references"]   = static_cast<long long>(total_refs);
        j["memory_usage_bytes"] = static_cast<long long>(get_string_pool().memory_usage());
        j["avg_string_length"]  = unique_strings > 0
                                    ? static_cast<long long>(bytes_saved / unique_strings)
                                    : 0;
        return j.dump();
    }



    void check_permission(const std::string& query,
                          const auth::ClientSession& session,
                          const auth::PermissionChecker& checker) {
        const std::string cmd = admin_command_name(query);
        if (cmd == "STRING_STATS") return;

        SQLParser parser;
        auto parsed = parser.parse(query);

        switch (parsed.type) {
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
                return;
            default:
                break;
        }

        std::string current_db = storage_.get_current_db();
        if (!checker.check_query(session.username, parsed, current_db)) {
            throw std::runtime_error("Permission denied");
        }
    }



    QueryResult execute(const std::string& query) {
        const std::string cmd = admin_command_name(query);
        if (cmd == "STRING_STATS") {
            return create_string_pool_stats();
        }

        SQLParser parser;
        auto parsed = parser.parse(query);

        switch (parsed.type) {
            case SQLParser::ParsedQuery::CREATE_DB:
                execute_create_database(parsed);
                return create_message_result("Database created");

            case SQLParser::ParsedQuery::DROP_DB:
                execute_drop_database(parsed);
                return create_message_result("Database dropped");

            case SQLParser::ParsedQuery::USE:
                execute_use(parsed);
                return create_message_result("Using database: " + parsed.database_name);

            case SQLParser::ParsedQuery::CREATE_TABLE:
                execute_create_table(parsed);
                return create_message_result("Table created");

            case SQLParser::ParsedQuery::DROP_TABLE:
                execute_drop_table(parsed);
                return create_message_result("Table dropped");

            case SQLParser::ParsedQuery::INSERT_OP:
                execute_insert(parsed);
                return create_message_result(
                    std::to_string(parsed.values.size()) + " rows inserted");

            case SQLParser::ParsedQuery::SELECT_OP:
                return execute_select(parsed);

            case SQLParser::ParsedQuery::UPDATE_OP:
                return execute_update(parsed);

            case SQLParser::ParsedQuery::DELETE_OP:
                return execute_delete(parsed);

            case SQLParser::ParsedQuery::REVERT_OP:
                execute_revert(parsed);
                return create_message_result("Reverted to snapshot");

            default:
                throw std::runtime_error("Unknown query type");
        }
    }

    QueryResult execute(const std::string& query,
                        const auth::ClientSession& session,
                        const auth::PermissionChecker& checker) {
        check_permission(query, session, checker);
        return execute(query);
    }

private:


    void execute_create_database(const SQLParser::ParsedQuery& q) {
        storage_.create_database(q.database_name);
    }

    void execute_drop_database(const SQLParser::ParsedQuery& q) {
        storage_.drop_database(q.database_name);
    }

    void execute_use(const SQLParser::ParsedQuery& q) {
        storage_.use_database(q.database_name);
    }

    void execute_create_table(const SQLParser::ParsedQuery& q) {
        storage_.create_table(q.table_name, q.columns);
    }

    void execute_drop_table(const SQLParser::ParsedQuery& q) {
        storage_.drop_table(q.table_name);
    }



    void execute_insert(const SQLParser::ParsedQuery& q) {
        auto schema = storage_.get_schema(q.table_name);
        auto& rows  = storage_.get_table_data(q.table_name);

        for (const auto& row_values : q.values) {
            Row new_row;

            for (size_t i = 0; i < schema.columns.size(); ++i) {
                const auto& col = schema.columns[i];

                auto it = std::find(q.column_names.begin(),
                                    q.column_names.end(),
                                    col.name);
                if (it != q.column_names.end()) {
                    size_t idx = std::distance(q.column_names.begin(), it);
                    if (idx < row_values.size()) {
                        new_row.values.push_back(row_values[idx]);
                    } else {
                        new_row.values.push_back(NullType{});
                    }
                } else if (col.modifiers.default_value.has_value()) {
                    new_row.values.push_back(*col.modifiers.default_value);
                } else {
                    new_row.values.push_back(NullType{});
                }
            }

            for (size_t i = 0; i < schema.columns.size(); ++i) {
                if (schema.columns[i].modifiers.not_null &&
                    std::holds_alternative<NullType>(new_row.values[i])) {
                    throw std::runtime_error(
                        "NOT_NULL constraint violated for column: " +
                        schema.columns[i].name);
                }
            }

            size_t record_id = rows.size();
            rows.push_back(new_row);

            for (size_t i = 0; i < schema.columns.size(); ++i) {
                const auto& col = schema.columns[i];
                if (!col.modifiers.indexed) continue;
                if (std::holds_alternative<NullType>(new_row.values[i])) continue;

                try {
                    if (col.type == ColumnType::INT) {
                        int val = std::get<int>(new_row.values[i]);
                        get_int_idx(schema.name, col.name)->insert(val, record_id);
                    } else {
                        StringRef val = std::get<StringRef>(new_row.values[i]);
                        get_str_idx(schema.name, col.name)->insert(IndexKey(val), record_id);
                    }
                } catch (const std::exception& e) {
                    rows.pop_back();
                    std::string msg = e.what();
                    if (msg.find("UNIQUE") != std::string::npos) {
                        throw std::runtime_error(
                            "UNIQUE constraint violated for INDEXED column: " +
                            col.name);
                    }
                    throw;
                }
            }
        }

        storage_.save_table(storage_.get_current_db(), q.table_name);
        storage_.flush_indexes(q.table_name);
        storage_.create_snapshot(q.table_name);
    }



    QueryResult execute_select(const SQLParser::ParsedQuery& q) {
        auto schema = storage_.get_schema(q.table_name);
        const auto& rows = storage_.get_table_data(q.table_name);

        std::vector<uint64_t> idx_rows = optimize_with_index(schema, q.condition.get());

        if (!q.aggregate_func.empty()) {
            return execute_aggregate(q, schema, rows, idx_rows);
        }

        QueryResult result;

        if (q.select_all) {
            for (const auto& col : schema.columns) {
                result.column_names.push_back(col.name);
            }
        } else {
            for (size_t i = 0; i < q.select_columns.size(); ++i) {
                std::string alias = i < q.select_aliases.size()
                                    ? q.select_aliases[i]
                                    : q.select_columns[i];
                result.column_names.push_back(alias);
            }
        }

        if (!idx_rows.empty()) {
            for (uint64_t rid : idx_rows) {
                if (rid >= rows.size()) continue;
                const auto& row = rows[rid];

                if (q.condition && !evaluate_condition(row, *q.condition, schema)) continue;

                Row result_row;
                if (q.select_all) {
                    result_row = row;
                } else {
                    for (const auto& col_name : q.select_columns) {
                        size_t ci = schema.get_column_index(col_name);
                        result_row.values.push_back(row.values[ci]);
                    }
                }
                result.rows.push_back(result_row);
            }
        } else {
            for (const auto& row : rows) {
                if (q.condition && !evaluate_condition(row, *q.condition, schema)) continue;

                Row result_row;
                if (q.select_all) {
                    result_row = row;
                } else {
                    for (const auto& col_name : q.select_columns) {
                        size_t ci = schema.get_column_index(col_name);
                        result_row.values.push_back(row.values[ci]);
                    }
                }
                result.rows.push_back(result_row);
            }
        }

        return result;
    }



    QueryResult execute_aggregate(const SQLParser::ParsedQuery& q,
                                  const TableSchema& schema,
                                  const std::vector<Row>& rows,
                                  const std::vector<uint64_t>& idx_rows) {
        std::vector<const Row*> matched;

        if (!idx_rows.empty()) {
            for (uint64_t rid : idx_rows) {
                if (rid >= rows.size()) continue;
                if (q.condition && !evaluate_condition(rows[rid], *q.condition, schema))
                    continue;
                matched.push_back(&rows[rid]);
            }
        } else {
            for (const auto& row : rows) {
                if (q.condition && !evaluate_condition(row, *q.condition, schema))
                    continue;
                matched.push_back(&row);
            }
        }

        QueryResult agg;
        agg.column_names.push_back(q.aggregate_func + "(" + q.aggregate_col + ")");
        Row r;

        if (q.aggregate_func == "count") {
            r.values.push_back(static_cast<int>(matched.size()));
        }
        else if (q.aggregate_func == "sum") {
            if (q.aggregate_col == "*" || q.aggregate_col.empty()) {
                r.values.push_back(0);
            } else {
                size_t ci = schema.get_column_index(q.aggregate_col);
                long long sum = 0;
                bool has = false;
                for (const Row* row : matched) {
                    if (ci < row->values.size() &&
                        !std::holds_alternative<NullType>(row->values[ci]) &&
                        std::holds_alternative<int>(row->values[ci])) {
                        sum += std::get<int>(row->values[ci]);
                        has = true;
                    }
                }
                r.values.push_back(has ? static_cast<int>(sum) : 0);
            }
        }
        else if (q.aggregate_func == "avg") {
            if (q.aggregate_col.empty() || q.aggregate_col == "*") {
                r.values.push_back(0);
            } else {
                size_t ci = schema.get_column_index(q.aggregate_col);
                double sum = 0;
                int cnt = 0;
                for (const Row* row : matched) {
                    if (ci < row->values.size() &&
                        !std::holds_alternative<NullType>(row->values[ci]) &&
                        std::holds_alternative<int>(row->values[ci])) {
                        sum += std::get<int>(row->values[ci]);
                        ++cnt;
                    }
                }
                r.values.push_back(cnt > 0 ? static_cast<int>(sum / cnt) : 0);
            }
        }
        else {
            throw std::runtime_error("Unknown aggregate function: " + q.aggregate_func);
        }

        agg.rows.push_back(r);
        return agg;
    }



    QueryResult execute_update(const SQLParser::ParsedQuery& q) {
        auto schema = storage_.get_schema(q.table_name);
        auto& rows  = storage_.get_table_data(q.table_name);

        std::vector<uint64_t> idx_rows = optimize_with_index(schema, q.condition.get());
        int updated = 0;

        auto process_row = [&](Row& row) {
            for (const auto& upd : q.updates) {
                size_t ci = schema.get_column_index(upd.first);
                row.values[ci] = upd.second;
            }
            for (size_t j = 0; j < schema.columns.size(); ++j) {
                if (schema.columns[j].modifiers.not_null &&
                    std::holds_alternative<NullType>(row.values[j])) {
                    throw std::runtime_error(
                        "NOT_NULL constraint violated for column: " +
                        schema.columns[j].name);
                }
            }
            ++updated;
        };

        if (!idx_rows.empty()) {
            for (uint64_t rid : idx_rows) {
                if (rid >= rows.size()) continue;
                if (q.condition && !evaluate_condition(rows[rid], *q.condition, schema))
                    continue;
                process_row(rows[rid]);
            }
        } else {
            for (auto& row : rows) {
                if (q.condition && !evaluate_condition(row, *q.condition, schema))
                    continue;
                process_row(row);
            }
        }

        storage_.save_table(storage_.get_current_db(), q.table_name);
        storage_.rebuild_indexes_public(q.table_name);
        storage_.flush_indexes(q.table_name);
        storage_.create_snapshot(q.table_name);

        return create_message_result(std::to_string(updated) + " rows updated");
    }



    QueryResult execute_delete(const SQLParser::ParsedQuery& q) {
        auto schema = storage_.get_schema(q.table_name);
        auto& rows  = storage_.get_table_data(q.table_name);

        std::vector<uint64_t> idx_rows = optimize_with_index(schema, q.condition.get());
        std::set<uint64_t> idx_set(idx_rows.begin(), idx_rows.end());

        std::vector<Row> keep;
        int deleted = 0;

        for (size_t i = 0; i < rows.size(); ++i) {
            bool match = true;

            if (!idx_rows.empty() && !idx_set.count(i)) {
                match = false;
            } else if (q.condition && !evaluate_condition(rows[i], *q.condition, schema)) {
                match = false;
            }

            if (match) ++deleted;
            else       keep.push_back(rows[i]);
        }

        rows = std::move(keep);

        storage_.save_table(storage_.get_current_db(), q.table_name);
        storage_.rebuild_indexes_public(q.table_name);
        storage_.flush_indexes(q.table_name);
        storage_.create_snapshot(q.table_name);

        return create_message_result(std::to_string(deleted) + " rows deleted");
    }



    void execute_revert(const SQLParser::ParsedQuery& q) {
        if (q.revert_all) {
            storage_.revert_all_tables(q.timestamp);
        } else {
            storage_.revert_to_snapshot(q.table_name, q.timestamp);
        }
    }
};



inline std::string QueryResult::to_json() const {
    json j = json::array();

    for (const auto& row : rows) {
        json obj;
        for (size_t i = 0; i < column_names.size() && i < row.values.size(); ++i) {
            const auto& val = row.values[i];
            if (std::holds_alternative<NullType>(val)) {
                obj[column_names[i]] = nullptr;
            } else if (std::holds_alternative<int>(val)) {
                obj[column_names[i]] = std::get<int>(val);
            } else if (std::holds_alternative<StringRef>(val)) {
                obj[column_names[i]] = *std::get<StringRef>(val);
            }
        }
        j.push_back(obj);
    }

    return j.dump(2);
}

} // namespace dbms

#endif