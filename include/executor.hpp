#ifndef DBMS_EXECUTOR_HPP
#define DBMS_EXECUTOR_HPP

#include "storage.hpp"
#include "btree.hpp"
#include "parser.hpp"
#include "string_pool.hpp"
#include "auth/auth_types.hpp"
#include "auth/permissions.hpp"
#include <regex>
// #include <nlohmann/json.hpp>
#include "../include/json.hpp"

using json = nlohmann::json;

namespace dbms {

class Executor {
private:
    StorageManager storage_;
    std::map<std::string, std::map<std::string, std::unique_ptr<BTreeIndex<int>>>> int_indices_;
    std::map<std::string, std::map<std::string, std::unique_ptr<BTreeIndex<std::string>>>> str_indices_;
    
    void rebuild_index(const std::string& table_name, const Column& col) {
        if (!col.modifiers.indexed) return;

        const auto& rows = storage_.get_table_data(table_name);
        auto current_db = storage_.get_current_db();
        auto schema = storage_.get_schema(table_name);
        size_t col_idx = schema.get_column_index(col.name);
        
        if (col.type == ColumnType::INT) {
            auto& idx = int_indices_[current_db][table_name + "_" + col.name];
            idx = std::make_unique<BTreeIndex<int>>();
            
            for (size_t i = 0; i < rows.size(); ++i) {
                if (!rows[i].is_null(col_idx)) {
                    idx->insert(rows[i].get_int(col_idx), i);
                }
            }
        } else {
            auto& idx = str_indices_[current_db][table_name + "_" + col.name];
            idx = std::make_unique<BTreeIndex<std::string>>();
            
            for (size_t i = 0; i < rows.size(); ++i) {
                if (!rows[i].is_null(col_idx)) {
                    idx->insert(rows[i].get_string(col_idx), i);
                }
            }
        }
    }

    void ensure_index(const std::string& table_name, const Column& col) {
        if (!col.modifiers.indexed) return;

        const auto current_db = storage_.get_current_db();
        const std::string key = table_name + "_" + col.name;

        if (col.type == ColumnType::INT) {
            if (!int_indices_[current_db][key]) {
                rebuild_index(table_name, col);
            }
        } else if (!str_indices_[current_db][key]) {
            rebuild_index(table_name, col);
        }
    }
    
    bool evaluate_condition(const Row& row, const Condition& cond, const TableSchema& schema) {
        if (!cond.is_leaf()) {
            bool left_result = cond.left ? evaluate_condition(row, *cond.left, schema) : true;
            bool right_result = cond.right ? evaluate_condition(row, *cond.right, schema) : true;
            
            return cond.is_and ? (left_result && right_result) : (left_result || right_result);
        }
        
        size_t col_idx = schema.get_column_index(cond.left_column);
        const Value& row_val = row.values[col_idx];
        
        auto compare = [&]() -> int {
            if (std::holds_alternative<NullType>(row_val)) {
                return -2;
            }
            
            if (std::holds_alternative<int>(row_val) && std::holds_alternative<int>(cond.right_value)) {
                int a = std::get<int>(row_val);
                int b = std::get<int>(cond.right_value);
                return (a < b) ? -1 : ((a > b) ? 1 : 0);
            }
            
            if (std::holds_alternative<StringRef>(row_val) && std::holds_alternative<StringRef>(cond.right_value)) {
                const std::string& a = *std::get<StringRef>(row_val);
                const std::string& b = *std::get<StringRef>(cond.right_value);
                return (a < b) ? -1 : ((a > b) ? 1 : 0);
            }
            
            return -2;
        };
        
        switch (cond.op) {
            case Condition::Op::EQ:
                return compare() == 0;
            case Condition::Op::NEQ:
                return compare() != 0;
            case Condition::Op::LT:
                return compare() < 0;
            case Condition::Op::GT:
                return compare() > 0;
            case Condition::Op::LTE:
                return compare() <= 0;
            case Condition::Op::GTE:
                return compare() >= 0;
            case Condition::Op::BETWEEN: {
                auto cmp1 = [&]() -> bool {
                    if (std::holds_alternative<int>(row_val) && std::holds_alternative<int>(cond.right_value)) {
                        return std::get<int>(row_val) >= std::get<int>(cond.right_value);
                    }
                    if (std::holds_alternative<StringRef>(row_val) && std::holds_alternative<StringRef>(cond.right_value)) {
                        return *std::get<StringRef>(row_val) >= *std::get<StringRef>(cond.right_value);
                    }
                    return false;
                };
                auto cmp2 = [&]() -> bool {
                    if (std::holds_alternative<int>(row_val) && std::holds_alternative<int>(cond.right_value2)) {
                        return std::get<int>(row_val) < std::get<int>(cond.right_value2);
                    }
                    if (std::holds_alternative<StringRef>(row_val) && std::holds_alternative<StringRef>(cond.right_value2)) {
                        return *std::get<StringRef>(row_val) < *std::get<StringRef>(cond.right_value2);
                    }
                    return false;
                };
                return cmp1() && cmp2();
            }
            case Condition::Op::LIKE: {
                if (std::holds_alternative<StringRef>(row_val) && std::holds_alternative<StringRef>(cond.right_value)) {
                    std::regex pattern(*std::get<StringRef>(cond.right_value));
                    return std::regex_match(*std::get<StringRef>(row_val), pattern);
                }
                return false;
            }
            default:
                return false;
        }
    }
    
    std::vector<size_t> optimize_with_index(const TableSchema& schema, const Condition* cond) {
        if (!cond || !cond->is_leaf()) {
            return {};
        }
        
        auto current_db = storage_.get_current_db();
        const Column& col = schema.get_column(cond->left_column);
        
        if (!col.modifiers.indexed) {
            return {};
        }
        
        if (cond->op == Condition::Op::EQ) {
            if (col.type == ColumnType::INT && std::holds_alternative<int>(cond->right_value)) {
                ensure_index(schema.name, col);
                auto& idx = int_indices_[current_db][schema.name + "_" + col.name];
                auto res = idx->find(std::get<int>(cond->right_value));
                if (res) return {*res};
            } else if (col.type == ColumnType::STRING && std::holds_alternative<StringRef>(cond->right_value)) {
                ensure_index(schema.name, col);
                auto& idx = str_indices_[current_db][schema.name + "_" + col.name];
                auto res = idx->find(*std::get<StringRef>(cond->right_value));
                if (res) return {*res};
            }
        }
        else if (cond->op == Condition::Op::BETWEEN) {
            if (col.type == ColumnType::INT && std::holds_alternative<int>(cond->right_value) && std::holds_alternative<int>(cond->right_value2)) {
                ensure_index(schema.name, col);
                auto& idx = int_indices_[current_db][schema.name + "_" + col.name];
                return idx->range_find(std::get<int>(cond->right_value), std::get<int>(cond->right_value2));
            }
            else if (col.type == ColumnType::STRING && std::holds_alternative<StringRef>(cond->right_value) && std::holds_alternative<StringRef>(cond->right_value2)) {
                auto& idx = str_indices_[current_db][schema.name + "_" + col.name];
                return idx->range_find(*std::get<StringRef>(cond->right_value), *std::get<StringRef>(cond->right_value2));
            }
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
    
    
public:
    Executor() : storage_() {}
    
    QueryResult execute(const std::string& query, const auth::ClientSession& session,
                        auth::PermissionChecker& permissions) {
        SQLParser parser;
        auto parsed = parser.parse(query);

        if (parsed.type == SQLParser::ParsedQuery::UNKNOWN) {
            throw std::runtime_error("Unknown query");
        }

        if (session.username != "__internal__" &&
            !permissions.check_query(session.username, parsed, storage_.get_current_db())) {
            throw std::runtime_error("Permission denied");
        }

        return execute_parsed(parsed);
    }

    QueryResult execute(const std::string& query) {
        auth::ClientSession session;
        session.username = "__internal__";
        session.authenticated = true;
        session.token_set = true;
        auth::AccountStore store("data/_auth");
        auth::PermissionChecker permissions(store);
        return execute(query, session, permissions);
    }

    QueryResult execute_parsed(const SQLParser::ParsedQuery& parsed) {
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
                return create_message_result(std::to_string(parsed.values.size()) + " rows inserted");
                
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

private:
    void execute_create_database(const SQLParser::ParsedQuery& query) {
        storage_.create_database(query.database_name);
    }
    
    void execute_drop_database(const SQLParser::ParsedQuery& query) {
        storage_.drop_database(query.database_name);
    }
    
    void execute_use(const SQLParser::ParsedQuery& query) {
        storage_.use_database(query.database_name);
    }
    
    void execute_create_table(const SQLParser::ParsedQuery& query) {
        storage_.create_table(query.table_name, query.columns);
        
        for (const auto& col : query.columns) {
            if (col.modifiers.indexed) {
                rebuild_index(query.table_name, col);
            }
        }
    }
    
    void execute_drop_table(const SQLParser::ParsedQuery& query) {
        storage_.drop_table(query.table_name);
        
        auto current_db = storage_.get_current_db();
        int_indices_[current_db].erase(query.table_name);
        str_indices_[current_db].erase(query.table_name);
    }
    
    void execute_insert(const SQLParser::ParsedQuery& query) {
        auto schema = storage_.get_schema(query.table_name);
        auto& rows = storage_.get_table_data(query.table_name);
        
        for (const auto& row_values : query.values) {
            Row new_row;
            
            for (size_t i = 0; i < schema.columns.size(); ++i) {
                const auto& col = schema.columns[i];
                
                auto it = std::find(query.column_names.begin(), query.column_names.end(), col.name);
                if (it != query.column_names.end()) {
                    size_t idx = std::distance(query.column_names.begin(), it);
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
                if (schema.columns[i].modifiers.not_null && std::holds_alternative<NullType>(new_row.values[i])) {
                    throw std::runtime_error("NOT_NULL constraint violated for column: " + schema.columns[i].name);
                }
            }
            
            size_t record_id = rows.size();
            rows.push_back(new_row);
            
            
            for (size_t i = 0; i < schema.columns.size(); ++i) {
                const auto& col = schema.columns[i];
                if (col.modifiers.indexed) {
                    ensure_index(schema.name, col);
                    auto current_db = storage_.get_current_db();
                    
                    if (col.type == ColumnType::INT && !std::holds_alternative<NullType>(new_row.values[i])) {
                        int val = std::get<int>(new_row.values[i]);
                        int_indices_[current_db][schema.name + "_" + col.name]->insert(val, record_id);
                    } else if (col.type == ColumnType::STRING && !std::holds_alternative<NullType>(new_row.values[i])) {
                        const std::string& val = *std::get<StringRef>(new_row.values[i]);
                        str_indices_[current_db][schema.name + "_" + col.name]->insert(val, record_id);
                    }
                }
            }
        }
        
        storage_.save_table(storage_.get_current_db(), query.table_name);
        storage_.create_snapshot(query.table_name);
    }
    
    QueryResult execute_select(const SQLParser::ParsedQuery& query) {
        auto schema = storage_.get_schema(query.table_name);
        const auto& rows = storage_.get_table_data(query.table_name);
        
        
        std::vector<size_t> row_indices = optimize_with_index(schema, query.condition.get());
        
        QueryResult result;
        
        
        if (query.select_all) {
            for (const auto& col : schema.columns) {
                result.column_names.push_back(col.name);
            }
        } else {
            for (size_t i = 0; i < query.select_columns.size(); ++i) {
                std::string alias = i < query.select_aliases.size() ? query.select_aliases[i] : query.select_columns[i];
                result.column_names.push_back(alias);
            }
        }
        
        
        for (size_t i = 0; i < rows.size(); ++i) {
            if (!row_indices.empty() && std::find(row_indices.begin(), row_indices.end(), i) == row_indices.end()) {
                continue;
            }
            
            if (query.condition && !evaluate_condition(rows[i], *query.condition, schema)) {
                continue;
            }
            
            Row result_row;
            
            // Агрегаты (SUM/AVG) читают values по индексу схемы — нужна полная строка
            if (query.select_all || !query.aggregate_func.empty()) {
                result_row = rows[i];
            } else {
                for (const auto& col_name : query.select_columns) {
                    
                    if (col_name == "sum" || col_name == "count" || col_name == "avg") {
                        continue;
                    }
                    
                    size_t col_idx = schema.get_column_index(col_name);
                    result_row.values.push_back(rows[i].values[col_idx]);
                }
            }
            
            result.rows.push_back(result_row);
        }
        
        
        if (!query.aggregate_func.empty()) {
            result = apply_aggregate(result, query, schema);
        }
        
        return result;
    }
    
    QueryResult apply_aggregate(QueryResult& current, const SQLParser::ParsedQuery& query, const TableSchema& schema) {
        QueryResult agg_result;
        
        if (query.aggregate_func == "count") {
            agg_result.column_names.push_back(query.aggregate_func + "(" + query.aggregate_col + ")");
            Row row;
            row.values.push_back(static_cast<int>(current.rows.size()));
            agg_result.rows.push_back(row);
        }
        else if (query.aggregate_func == "sum") {
            agg_result.column_names.push_back(query.aggregate_func + "(" + query.aggregate_col + ")");
            
            size_t col_idx = schema.get_column_index(query.aggregate_col);
            long long sum = 0;
            bool has_values = false;
            
            for (const auto& row : current.rows) {
                if (!std::holds_alternative<NullType>(row.values[col_idx]) && 
                    std::holds_alternative<int>(row.values[col_idx])) {
                    sum += std::get<int>(row.values[col_idx]);
                    has_values = true;
                }
            }
            
            Row row;
            row.values.push_back(has_values ? static_cast<int>(sum) : 0);
            agg_result.rows.push_back(row);
        }
        else if (query.aggregate_func == "avg") {
            agg_result.column_names.push_back(query.aggregate_func + "(" + query.aggregate_col + ")");
            
            size_t col_idx = schema.get_column_index(query.aggregate_col);
            double sum = 0;
            int count = 0;
            
            for (const auto& row : current.rows) {
                if (!std::holds_alternative<NullType>(row.values[col_idx]) && 
                    std::holds_alternative<int>(row.values[col_idx])) {
                    sum += std::get<int>(row.values[col_idx]);
                    count++;
                }
            }
            
            Row row;
            int avg_result = count > 0 ? static_cast<int>(sum / count) : 0;
            row.values.push_back(avg_result);
            agg_result.rows.push_back(row);
        }
        
        return agg_result;
    }
    
    QueryResult execute_update(const SQLParser::ParsedQuery& query) {
        auto schema = storage_.get_schema(query.table_name);
        auto& rows = storage_.get_table_data(query.table_name);
        
        std::vector<size_t> row_indices = optimize_with_index(schema, query.condition.get());
        int updated_count = 0;
        
        for (size_t i = 0; i < rows.size(); ++i) {
            if (!row_indices.empty() && std::find(row_indices.begin(), row_indices.end(), i) == row_indices.end()) {
                continue;
            }
            
            if (query.condition && !evaluate_condition(rows[i], *query.condition, schema)) {
                continue;
            }
            
            
            for (const auto& update : query.updates) {
                size_t col_idx = schema.get_column_index(update.first);
                rows[i].values[col_idx] = update.second;
            }
            
            
            for (size_t j = 0; j < schema.columns.size(); ++j) {
                if (schema.columns[j].modifiers.not_null && std::holds_alternative<NullType>(rows[i].values[j])) {
                    throw std::runtime_error("NOT_NULL constraint violated for column: " + schema.columns[j].name);
                }
            }
            
            updated_count++;
        }
        
        
        for (const auto& col : schema.columns) {
            if (col.modifiers.indexed) {
                rebuild_index(query.table_name, col);
            }
        }
        
        storage_.save_table(storage_.get_current_db(), query.table_name);
        storage_.create_snapshot(query.table_name);
        
        return create_message_result(std::to_string(updated_count) + " rows updated");
    }
    
    QueryResult execute_delete(const SQLParser::ParsedQuery& query) {
        auto schema = storage_.get_schema(query.table_name);
        auto& rows = storage_.get_table_data(query.table_name);
        
        std::vector<size_t> row_indices = optimize_with_index(schema, query.condition.get());
        
        std::vector<Row> new_rows;
        int deleted_count = 0;
        
        for (size_t i = 0; i < rows.size(); ++i) {
            if (!row_indices.empty() && std::find(row_indices.begin(), row_indices.end(), i) == row_indices.end()) {
                new_rows.push_back(rows[i]);
                continue;
            }
            
            if (query.condition && !evaluate_condition(rows[i], *query.condition, schema)) {
                new_rows.push_back(rows[i]);
            } else {
                deleted_count++;
            }
        }
        
        rows = new_rows;
        
        
        for (const auto& col : schema.columns) {
            if (col.modifiers.indexed) {
                rebuild_index(query.table_name, col);
            }
        }
        
        storage_.save_table(storage_.get_current_db(), query.table_name);
        storage_.create_snapshot(query.table_name);
        
        return create_message_result(std::to_string(deleted_count) + " rows deleted");
    }
    
    void execute_revert(const SQLParser::ParsedQuery& query) {
        if (query.revert_all) {
            storage_.revert_all_tables(query.timestamp);
            
            auto current_db = storage_.get_current_db();
            auto all_schemas = storage_.get_all_schemas(current_db);
            
            for (const auto& [table_name, schema] : all_schemas) {
                for (const auto& col : schema.columns) {
                    if (col.modifiers.indexed) {
                        rebuild_index(table_name, col);
                    }
                }
            }
        } else {
            storage_.revert_to_snapshot(query.table_name, query.timestamp);
            
            auto schema = storage_.get_schema(query.table_name);
            for (const auto& col : schema.columns) {
                if (col.modifiers.indexed) {
                    rebuild_index(query.table_name, col);
                }
            }
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

} 

#endif