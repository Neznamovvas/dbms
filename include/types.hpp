#ifndef DBMS_TYPES_HPP
#define DBMS_TYPES_HPP

#include <string>
#include <vector>
#include <memory>
#include <map>
#include <chrono>
#include <variant>
#include <optional>
#include <stdexcept>
#include "string_pool.hpp"

namespace dbms {

struct NullType {};


using Value = std::variant<NullType, int, StringRef>;

enum class ColumnType { INT, STRING };

struct ColumnModifiers {
    bool not_null = false;
    bool indexed = false;
    std::optional<Value> default_value;
};

struct Column {
    std::string name;
    ColumnType type;
    ColumnModifiers modifiers;
};

struct Row {
    std::vector<Value> values;
    
    bool is_null(size_t col) const {
        return std::holds_alternative<NullType>(values[col]);
    }
    
    int get_int(size_t col) const {
        return std::get<int>(values[col]);
    }
    
    std::string get_string(size_t col) const {
        return *std::get<StringRef>(values[col]);
    }
    
    const StringRef& get_string_ref(size_t col) const {
        return std::get<StringRef>(values[col]);
    }
};

struct TableSchema {
    std::string name;
    std::vector<Column> columns;
    std::map<std::string, size_t> column_index;
    
    size_t get_column_index(const std::string& name) const {
        auto it = column_index.find(name);
        if (it == column_index.end()) {
            throw std::runtime_error("Column not found: " + name);
        }
        return it->second;
    }
    
    const Column& get_column(const std::string& name) const {
        return columns[get_column_index(name)];
    }
};

struct Condition {
    enum class Op {
        EQ, NEQ, LT, GT, LTE, GTE, BETWEEN, LIKE
    };
    
    Op op;
    std::string left_column;
    Value right_value;
    Value right_value2;
    
    std::unique_ptr<Condition> left;
    std::unique_ptr<Condition> right;
    bool is_and = true;
    
    bool is_leaf() const { return !left && !right; }
};

struct QueryResult {
    std::vector<std::string> column_names;
    std::vector<Row> rows;
    
    std::string to_json() const;
};

} 

#endif