#ifndef DBMS_EXECUTOR_HPP
#define DBMS_EXECUTOR_HPP

#include "storage.hpp"
#include "btree.hpp"
#include "parser.hpp"
#include "string_pool.hpp"
#include <regex>
#include <nlohmann/json.hpp>

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
                auto& idx = int_indices_[current_db][schema.name + "_" + col.name];
                auto res = idx->find(std::get<int>(cond->right_value));
                if (res) return {*res};
            } else if (col.type == ColumnType::STRING && std::holds_alternative<StringRef>(cond->right_value)) {
                auto& idx = str_indices_[current_db][schema.name + "_" + col.name];
                auto res = idx->find(*std::get<StringRef>(cond->right_value));
                if (res) return {*res};
            }
        }
        else if (cond->op == Condition::Op::BETWEEN) {
            if (col.type == ColumnType::INT && std::holds_alternative<int>(cond->right_value) && std::holds_alternative<int>(cond->right_value2)) {
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
    Executor() : storage_() {
        // storage_'s constructor already replayed every table's WAL from
        // disk into memory (load_all()), but that only rebuilds row data -
        // in-memory B-tree indices are a query-time cache and are not
        // persisted themselves (per the assignment: indices must reference
        // the data, not duplicate it). So on every startup we rebuild the
        // index for each INDEXED column of every already-existing table from
        // the data that was just loaded, exactly as CREATE TABLE does for a
        // brand new one.
        for (const auto& db : storage_.get_databases()) {
            auto schemas = storage_.get_all_schemas(db);
            std::string prev_db = storage_.get_current_db();
            for (const auto& [table_name, schema] : schemas) {
                for (const auto& col : schema.columns) {
                    if (col.modifiers.indexed) {
                        // rebuild_index() reads storage_.get_current_db(),
                        // so make sure the right database is selected while
                        // we rebuild indices that belong to it.
                        if (storage_.get_current_db() != db) {
                            storage_.use_database(db);
                        }
                        rebuild_index(table_name, col);
                    }
                }
            }
        }
    }
    
    QueryResult execute(const std::string& query) {
        
        
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
            
            // insert_row() durably appends this row to the table's on-disk
            // WAL before returning - the row is never only "in RAM".
            size_t record_id = storage_.get_table_data(query.table_name).size();
            storage_.insert_row(query.table_name, new_row);
            
            
            for (size_t i = 0; i < schema.columns.size(); ++i) {
                const auto& col = schema.columns[i];
                if (col.modifiers.indexed) {
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
        
        // No full-table rewrite needed: each row above was already flushed
        // to disk individually. save_table() here only flushes the (rarely
        // changing) schema and triggers WAL compaction if the log has grown
        // past the threshold.
        storage_.save_table(storage_.get_current_db(), query.table_name);
    }
    
    QueryResult execute_select(const SQLParser::ParsedQuery& query) {
        auto schema = storage_.get_schema(query.table_name);
        const auto& rows = storage_.get_table_data(query.table_name);
        
        
        std::vector<size_t> row_indices = optimize_with_index(schema, query.condition.get());
        
        QueryResult result;
        
        // For an aggregate query (SELECT SUM(col)/COUNT(...)/AVG(col) FROM ...)
        // the parser does not put anything into select_columns - the target
        // column lives in query.aggregate_col instead. apply_aggregate()
        // below needs the *full* matching rows (so it can read
        // aggregate_col out of them by index), not the column-projected
        // result_row built below, which would be empty for an aggregate
        // query and reading past its bounds is undefined behavior (this was
        // the cause of the segfault on SELECT SUM(age) FROM users).
        bool is_aggregate = !query.aggregate_func.empty();
        
        if (query.select_all) {
            for (const auto& col : schema.columns) {
                result.column_names.push_back(col.name);
            }
        } else if (!is_aggregate) {
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
            
            if (query.select_all || is_aggregate) {
                // Keep the full row so apply_aggregate() can look up
                // aggregate_col by its schema index below.
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
        
        
        if (is_aggregate) {
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
                if (col_idx >= row.values.size()) continue;
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
                if (col_idx >= row.values.size()) continue;
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
        
        std::vector<size_t> row_indices = optimize_with_index(schema, query.condition.get());
        int updated_count = 0;
        
        // Snapshot the current row count/values up front: update_row() below
        // durably persists each change as it happens, so we iterate over a
        // stable copy of "which rows currently match" first.
        size_t row_count = storage_.get_table_data(query.table_name).size();
        
        for (size_t i = 0; i < row_count; ++i) {
            if (!row_indices.empty() && std::find(row_indices.begin(), row_indices.end(), i) == row_indices.end()) {
                continue;
            }
            
            Row updated_row = storage_.get_table_data(query.table_name)[i];
            
            if (query.condition && !evaluate_condition(updated_row, *query.condition, schema)) {
                continue;
            }
            
            
            for (const auto& update : query.updates) {
                size_t col_idx = schema.get_column_index(update.first);
                updated_row.values[col_idx] = update.second;
            }
            
            
            for (size_t j = 0; j < schema.columns.size(); ++j) {
                if (schema.columns[j].modifiers.not_null && std::holds_alternative<NullType>(updated_row.values[j])) {
                    throw std::runtime_error("NOT_NULL constraint violated for column: " + schema.columns[j].name);
                }
            }
            
            // Durably persist this single row change to the WAL immediately.
            storage_.update_row(query.table_name, i, updated_row);
            
            updated_count++;
        }
        
        
        for (const auto& col : schema.columns) {
            if (col.modifiers.indexed) {
                rebuild_index(query.table_name, col);
            }
        }
        
        storage_.save_table(storage_.get_current_db(), query.table_name);
        
        return create_message_result(std::to_string(updated_count) + " rows updated");
    }
    
    QueryResult execute_delete(const SQLParser::ParsedQuery& query) {
        auto schema = storage_.get_schema(query.table_name);
        
        std::vector<size_t> row_indices = optimize_with_index(schema, query.condition.get());
        
        int deleted_count = 0;
        
        // Delete from the end towards the start so that earlier indices
        // (which update_row/delete_row address positionally) stay valid as
        // rows are removed one at a time.
        size_t row_count = storage_.get_table_data(query.table_name).size();
        for (size_t rev = 0; rev < row_count; ++rev) {
            size_t i = row_count - 1 - rev;
            
            if (!row_indices.empty() && std::find(row_indices.begin(), row_indices.end(), i) == row_indices.end()) {
                continue;
            }
            
            const Row& row = storage_.get_table_data(query.table_name)[i];
            
            if (query.condition && !evaluate_condition(row, *query.condition, schema)) {
                continue;
            }
            
            // Durably persist this row deletion to the WAL immediately.
            storage_.delete_row(query.table_name, i);
            deleted_count++;
        }
        
        
        for (const auto& col : schema.columns) {
            if (col.modifiers.indexed) {
                rebuild_index(query.table_name, col);
            }
        }
        
        storage_.save_table(storage_.get_current_db(), query.table_name);
        
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