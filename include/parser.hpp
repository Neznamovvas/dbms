#ifndef DBMS_PARSER_HPP
#define DBMS_PARSER_HPP

#include "types.hpp"
#include <string>
#include <vector>
#include <regex>
#include <cctype>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <chrono>
#include <iomanip>

namespace dbms {

class SQLParser {
private:
    std::string input_;
    size_t pos_ = 0;
    
    void skip_whitespace() {
        while (pos_ < input_.size() && std::isspace(input_[pos_])) {
            pos_++;
        }
    }
    
    std::string parse_identifier() {
        skip_whitespace();
        if (pos_ >= input_.size()) return "";
        
        std::string result;
        if (std::isalpha(input_[pos_]) || input_[pos_] == '_' || input_[pos_] == '*') {
            while (pos_ < input_.size() && (std::isalnum(input_[pos_]) || input_[pos_] == '_' || input_[pos_] == '*')) {
                result += std::tolower(input_[pos_++]);
            }
        }
        
        return result;
    }
    
    std::string parse_string_literal() {
        skip_whitespace();
        if (pos_ >= input_.size() || (input_[pos_] != '"' && input_[pos_] != '\'')) {
            return "";
        }
        
        char quote = input_[pos_++];
        std::string result;
        while (pos_ < input_.size() && input_[pos_] != quote) {
            result += input_[pos_++];
        }
        
        if (pos_ < input_.size() && input_[pos_] == quote) {
            pos_++;
        }
        
        return result;
    }
    
    std::chrono::system_clock::time_point parse_timestamp() {
        skip_whitespace();
        
        std::string ts_str;
        
        if (pos_ < input_.size() && (input_[pos_] == '"' || input_[pos_] == '\'')) {
            ts_str = parse_string_literal();
        } else {
            while (pos_ < input_.size() && 
                   (std::isdigit(input_[pos_]) || input_[pos_] == '.' || 
                    input_[pos_] == '-' || input_[pos_] == ':')) {
                ts_str += input_[pos_++];
            }
        }
        
        std::tm tm = {};
        int ms = 0;
        
        if (sscanf(ts_str.c_str(), "%d.%d.%d-%d:%d:%d.%d",
                   &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                   &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &ms) < 6) {
            throw std::runtime_error("Invalid timestamp format. Expected: yyyy.mm.dd-hh:mm:ss.msmsms");
        }
        
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        
        auto time = std::chrono::system_clock::from_time_t(std::mktime(&tm));
        time += std::chrono::milliseconds(ms);
        
        return time;
    }
        
    Value parse_value() {
        skip_whitespace();
        if (pos_ >= input_.size()) return NullType{};
        
        if (input_[pos_] == '"' || input_[pos_] == '\'') {
            std::string str = parse_string_literal();
            return make_string_ref(str);
        }
        
        if (std::isdigit(input_[pos_]) || input_[pos_] == '-') {
            std::string num;
            bool is_negative = (input_[pos_] == '-');
            if (is_negative) {
                num += input_[pos_++];
            }
            while (pos_ < input_.size() && std::isdigit(input_[pos_])) {
                num += input_[pos_++];
            }
            return std::stoi(num);
        }
        
        std::string word = parse_identifier();
        if (word == "null") {
            return NullType{};
        }
        
        return NullType{};
    }
    
public:
    struct ParsedQuery {
        enum Type { 
            CREATE_DB,
            DROP_DB,
            USE,
            CREATE_TABLE,
            DROP_TABLE,
            INSERT_OP,     
            SELECT_OP,     
            UPDATE_OP,     
            DELETE_OP,     
            REVERT_OP,
            STATS_CMD,
            ROTATE_CMD,     
            UNKNOWN 
        } type = UNKNOWN;
        
        std::string database_name;
        std::string table_name;
        std::vector<Column> columns;
        std::vector<std::string> column_names;
        std::vector<std::vector<Value>> values;
        std::vector<std::pair<std::string, Value>> updates;
        std::unique_ptr<Condition> condition;
        std::vector<std::string> select_columns;
        std::vector<std::string> select_aliases;
        std::chrono::system_clock::time_point timestamp;
        bool select_all = false;
        bool revert_all = false;
        
        std::string aggregate_func;
        std::string aggregate_col;
    };
    
    ParsedQuery parse(const std::string& query) {
        input_ = query;
        pos_ = 0;
        
        skip_whitespace();
        std::string command = parse_identifier();
        
        ParsedQuery result;
        
        if (command == "create") {
            std::string type = parse_identifier();
            if (type == "database") {
                result.type = ParsedQuery::CREATE_DB;
                result.database_name = parse_identifier();
            } else if (type == "table") {
                result.type = ParsedQuery::CREATE_TABLE;
                parse_create_table(result);
            }
        }
        else if (command == "drop") {
            std::string type = parse_identifier();
            if (type == "database") {
                result.type = ParsedQuery::DROP_DB;
                result.database_name = parse_identifier();
            } else if (type == "table") {
                result.type = ParsedQuery::DROP_TABLE;
                result.table_name = parse_identifier();
            }
        }
        else if (command == "use") {
            result.type = ParsedQuery::USE;
            result.database_name = parse_identifier();
        }
        else if (command == "insert") {
            result.type = ParsedQuery::INSERT_OP;
            parse_insert(result);
        }
        else if (command == "select") {
            result.type = ParsedQuery::SELECT_OP;
            parse_select(result);
        }
        else if (command == "update") {
            result.type = ParsedQuery::UPDATE_OP;
            parse_update(result);
        }
        else if (command == "delete") {
            result.type = ParsedQuery::DELETE_OP;
            parse_delete(result);
        }
        else if (command == "revert") {
            result.type = ParsedQuery::REVERT_OP;
            parse_revert(result);
        }
        else if (command == "stats") {
            result.type = ParsedQuery::STATS_CMD;
        }
        else if (command == "rotate") {
            result.type = ParsedQuery::ROTATE_CMD;
        }
        
        return result;
    }
    
private:
    void parse_create_table(ParsedQuery& result) {
        result.table_name = parse_identifier();
        skip_whitespace();
        
        if (pos_ < input_.size() && input_[pos_] == '(') {
            pos_++;
            
            while (pos_ < input_.size() && input_[pos_] != ')') {
                Column col;
                col.name = parse_identifier();
                skip_whitespace();
                
                std::string type = parse_identifier();
                if (type == "int") {
                    col.type = ColumnType::INT;
                } else if (type == "string") {
                    col.type = ColumnType::STRING;
                } else {
                    throw std::runtime_error("Unknown type: " + type);
                }
                
                while (true) {
                    skip_whitespace();
                    if (pos_ >= input_.size()) break;
                    if (input_[pos_] == ',' || input_[pos_] == ')') break;
                    
                    std::string mod = parse_identifier();
                    
                    if (mod == "not_null") {
                        col.modifiers.not_null = true;
                    }
                    else if (mod == "indexed") {
                        col.modifiers.indexed = true;
                        col.modifiers.not_null = true;
                    }
                    else if (mod == "default") {
                        col.modifiers.default_value = parse_value();
                    }
                    else {
                        if (!mod.empty()) {
                            pos_ -= mod.size();
                        }
                        break;
                    }
                }
                
                result.columns.push_back(col);
                
                skip_whitespace();
                if (pos_ < input_.size() && input_[pos_] == ',') {
                    pos_++;
                }
            }
            
            if (pos_ < input_.size() && input_[pos_] == ')') {
                pos_++;
            }
        }
    }
    
    void parse_insert(ParsedQuery& result) {
        parse_identifier();
        result.table_name = parse_identifier();
        skip_whitespace();
        
        if (pos_ < input_.size() && input_[pos_] == '(') {
            pos_++;
            while (pos_ < input_.size() && input_[pos_] != ')') {
                result.column_names.push_back(parse_identifier());
                skip_whitespace();
                if (pos_ < input_.size() && input_[pos_] == ',') {
                    pos_++;
                }
            }
            if (pos_ < input_.size() && input_[pos_] == ')') {
                pos_++;
            }
        }
        
        parse_identifier();
        
        while (pos_ < input_.size() && input_[pos_] != ';') {
            skip_whitespace();
            if (pos_ < input_.size() && input_[pos_] == '(') {
                pos_++;
                std::vector<Value> row_values;
                
                while (pos_ < input_.size() && input_[pos_] != ')') {
                    row_values.push_back(parse_value());
                    skip_whitespace();
                    if (pos_ < input_.size() && input_[pos_] == ',') {
                        pos_++;
                    }
                }
                
                if (pos_ < input_.size() && input_[pos_] == ')') {
                    pos_++;
                }
                
                result.values.push_back(row_values);
            }
            
            skip_whitespace();
            if (pos_ < input_.size() && input_[pos_] == ',') {
                pos_++;
            }
        }
    }
    
    void parse_select(ParsedQuery& result) {
        skip_whitespace();
        
        if (pos_ < input_.size() && input_[pos_] == '*') {
            result.select_all = true;
            pos_++;
        } else {
            while (pos_ < input_.size()) {
                skip_whitespace();
                if (pos_ >= input_.size()) break;
                
                size_t saved_pos = pos_;
                std::string next_word = parse_identifier();
                
                if (next_word == "from") {
                    pos_ = saved_pos;
                    break;
                }
                
                pos_ = saved_pos;
                std::string col = parse_identifier();
                
                if (col == "count" || col == "sum" || col == "avg") {
                    result.aggregate_func = col;
                    skip_whitespace();
                    
                    if (pos_ < input_.size() && input_[pos_] == '(') {
                        pos_++;
                        skip_whitespace();
                        
                        if (pos_ < input_.size() && input_[pos_] == '*') {
                            result.aggregate_col = "*";
                            pos_++;
                        } else {
                            result.aggregate_col = parse_identifier();
                        }
                        
                        skip_whitespace();
                        if (pos_ < input_.size() && input_[pos_] == ')') {
                            pos_++;
                        }
                    }
                } else {
                    result.select_columns.push_back(col);
                    
                    skip_whitespace();
                    std::string as_word = parse_identifier();
                    if (as_word == "as") {
                        result.select_aliases.push_back(parse_identifier());
                    } else {
                        if (!as_word.empty()) pos_ -= as_word.size();
                        result.select_aliases.push_back(col);
                    }
                }
                
                skip_whitespace();
                if (pos_ < input_.size() && input_[pos_] == ',') {
                    pos_++;
                    continue;
                } else {
                    break;
                }
            }
        }
        
        skip_whitespace();
        std::string from_keyword = parse_identifier();
        if (from_keyword == "from") {
            result.table_name = parse_identifier();
        }
        
        skip_whitespace();
        std::string where_keyword = parse_identifier();
        if (where_keyword == "where") {
            result.condition = parse_condition();
        }
    }
    
    void parse_update(ParsedQuery& result) {
        result.table_name = parse_identifier();
        parse_identifier();
        
        while (pos_ < input_.size()) {
            std::string col = parse_identifier();
            skip_whitespace();
            if (pos_ < input_.size() && input_[pos_] == '=') {
                pos_++;
            }
            Value val = parse_value();
            result.updates.push_back({col, val});
            
            skip_whitespace();
            if (pos_ < input_.size() && input_[pos_] == ',') {
                pos_++;
            } else {
                break;
            }
        }
        
        skip_whitespace();
        std::string where = parse_identifier();
        if (where == "where") {
            result.condition = parse_condition();
        }
    }
    
    void parse_delete(ParsedQuery& result) {
        parse_identifier();
        result.table_name = parse_identifier();
        
        skip_whitespace();
        std::string where = parse_identifier();
        if (where == "where") {
            result.condition = parse_condition();
        }
    }
    
    void parse_revert(ParsedQuery& result) {
        skip_whitespace();
        
        std::string target = parse_identifier();
        
        if (target == "*") {
            result.revert_all = true;
            result.table_name = "";
            result.timestamp = parse_timestamp();
        } else {
            result.revert_all = false;
            result.table_name = target;
            result.timestamp = parse_timestamp();
        }
    }
    
    std::unique_ptr<Condition> parse_condition() {
        auto cond = std::make_unique<Condition>();
        
        skip_whitespace();
        
        if (pos_ < input_.size() && input_[pos_] == '(') {
            pos_++;
            cond->left = parse_condition();
            skip_whitespace();
            if (pos_ < input_.size() && input_[pos_] == ')') {
                pos_++;
            }
            
            skip_whitespace();
            std::string logic = parse_identifier();
            if (logic == "and" || logic == "or") {
                cond->is_and = (logic == "and");
                cond->right = parse_condition();
            }
            return cond;
        }
        
        cond->left_column = parse_identifier();
        skip_whitespace();
        
        if (pos_ + 1 < input_.size() && input_[pos_] == '=' && input_[pos_+1] == '=') {
            cond->op = Condition::Op::EQ;
            pos_ += 2;
        } else if (pos_ + 1 < input_.size() && input_[pos_] == '!' && input_[pos_+1] == '=') {
            cond->op = Condition::Op::NEQ;
            pos_ += 2;
        } else if (input_[pos_] == '<') {
            if (pos_ + 1 < input_.size() && input_[pos_+1] == '=') {
                cond->op = Condition::Op::LTE;
                pos_ += 2;
            } else {
                cond->op = Condition::Op::LT;
                pos_ += 1;
            }
        } else if (input_[pos_] == '>') {
            if (pos_ + 1 < input_.size() && input_[pos_+1] == '=') {
                cond->op = Condition::Op::GTE;
                pos_ += 2;
            } else {
                cond->op = Condition::Op::GT;
                pos_ += 1;
            }
        }
        
        skip_whitespace();
        
        std::string next = parse_identifier();
        if (next == "between") {
            cond->op = Condition::Op::BETWEEN;
            cond->right_value = parse_value();
            skip_whitespace();
            parse_identifier();
            cond->right_value2 = parse_value();
        } else if (next == "like") {
            cond->op = Condition::Op::LIKE;
            cond->right_value = parse_value();
        } else {
            if (!next.empty()) pos_ -= next.size();
            cond->right_value = parse_value();
        }
        
        return cond;
    }
};

} 

#endif