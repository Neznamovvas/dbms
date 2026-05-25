#ifndef DBMS_STORAGE_HPP
#define DBMS_STORAGE_HPP

#include "types.hpp"
#include <fstream>
#include <set>
#include <map>
#include <cstring>
#include <sys/stat.h>
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define mkdir _mkdir
#else
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace dbms {

class StorageManager {
private:
    std::string data_dir_;
    std::map<std::string, std::map<std::string, std::vector<Row>>> data_;
    std::map<std::string, std::map<std::string, TableSchema>> schemas_;
    std::set<std::string> databases_;
    std::string current_db_;
    
    std::map<std::string, std::map<std::string, std::vector<std::pair<std::string, std::vector<Row>>>>> snapshots_;
    
    bool directory_exists(const std::string& path) const {
        struct stat st;
        return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    }
    
    bool file_exists(const std::string& path) const {
        struct stat st;
        return stat(path.c_str(), &st) == 0;
    }
    
    void create_directory(const std::string& path) {
        if (!directory_exists(path)) {
            #ifdef _WIN32
                mkdir(path.c_str());
            #else
                mkdir(path.c_str(), 0755);
            #endif
        }
    }
    
    std::string get_db_path(const std::string& db) const {
        return data_dir_ + "/" + db;
    }
    
    std::string get_table_path(const std::string& db, const std::string& table) const {
        return get_db_path(db) + "/" + table + ".bin";
    }
    
    std::string get_schema_path(const std::string& db, const std::string& table) const {
        return get_db_path(db) + "/" + table + "_schema.bin";
    }
    
    std::string get_snapshot_dir(const std::string& db, const std::string& table) const {
        return get_db_path(db) + "/snapshots/" + table;
    }
    
    std::string timepoint_to_string(const std::chrono::system_clock::time_point& tp) const {
        auto time_t = std::chrono::system_clock::to_time_t(tp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            tp.time_since_epoch()) % 1000;
        
        std::tm* tm = std::localtime(&time_t);
        char buffer[32];
        strftime(buffer, sizeof(buffer), "%Y.%m.%d-%H:%M:%S", tm);
        
        std::stringstream ss;
        ss << buffer << "." << std::setw(3) << std::setfill('0') << ms.count();
        return ss.str();
    }
    
    std::chrono::system_clock::time_point string_to_timepoint(const std::string& timestamp) const {
        std::tm tm = {};
        int ms;
        
        sscanf(timestamp.c_str(), "%d.%d.%d-%d:%d:%d.%d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &ms);
        
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        
        auto time_t = mktime(&tm);
        auto tp = std::chrono::system_clock::from_time_t(time_t);
        tp += std::chrono::milliseconds(ms);
        
        return tp;
    }
    
    std::string get_current_timestamp() const {
        return timepoint_to_string(std::chrono::system_clock::now());
    }
    
    void serialize_value(std::ofstream& file, const Value& value) const {
        if (std::holds_alternative<NullType>(value)) {
            char type = 0;
            file.write(&type, 1);
        } else if (std::holds_alternative<int>(value)) {
            char type = 1;
            file.write(&type, 1);
            int val = std::get<int>(value);
            file.write(reinterpret_cast<const char*>(&val), sizeof(int));
        } else if (std::holds_alternative<StringRef>(value)) {
            char type = 2;
            file.write(&type, 1);
            const auto& str = *std::get<StringRef>(value);
            size_t len = str.size();
            file.write(reinterpret_cast<const char*>(&len), sizeof(size_t));
            file.write(str.c_str(), len);
        }
    }
    
    Value deserialize_value(std::ifstream& file) const {
        char type;
        file.read(&type, 1);
        
        if (file.eof() || file.fail()) {
            return NullType{};
        }
        
        if (type == 0) {
            return NullType{};
        } else if (type == 1) {
            int val;
            file.read(reinterpret_cast<char*>(&val), sizeof(int));
            if (file.fail()) return NullType{};
            return val;
        } else if (type == 2) {
            size_t len;
            file.read(reinterpret_cast<char*>(&len), sizeof(size_t));
            if (file.fail() || len > 10000000) {
                return NullType{};
            }
            std::string str(len, '\0');
            file.read(&str[0], len);
            if (file.fail()) return NullType{};
            return make_string_ref(str);  
        }
        
        return NullType{};
    }
    
    void save_snapshot_to_disk(const std::string& db, const std::string& table, 
                                const std::string& timestamp, const std::vector<Row>& rows) {
        std::string snapshot_dir = get_snapshot_dir(db, table);
        create_directory(snapshot_dir);
        
        std::string snapshot_path = snapshot_dir + "/" + timestamp + ".snap";
        std::ofstream file(snapshot_path, std::ios::binary | std::ios::trunc);
        
        if (!file) {
            std::cerr << "Warning: Cannot save snapshot for " << table << " at " << timestamp << std::endl;
            return;
        }
        
        size_t row_count = rows.size();
        file.write(reinterpret_cast<const char*>(&row_count), sizeof(size_t));
        
        for (const auto& row : rows) {
            size_t col_count = row.values.size();
            file.write(reinterpret_cast<const char*>(&col_count), sizeof(size_t));
            for (const auto& value : row.values) {
                serialize_value(file, value);
            }
        }
        
        std::cout << "Snapshot saved: " << table << " @ " << timestamp << " (" << row_count << " rows)" << std::endl;
    }
    
    std::vector<Row> load_snapshot_from_disk(const std::string& db, const std::string& table, 
                                              const std::string& timestamp) {
        std::string snapshot_path = get_snapshot_dir(db, table) + "/" + timestamp + ".snap";
        
        if (!file_exists(snapshot_path)) {
            throw std::runtime_error("Snapshot not found: " + timestamp);
        }
        
        std::ifstream file(snapshot_path, std::ios::binary);
        if (!file) {
            throw std::runtime_error("Cannot load snapshot: " + timestamp);
        }
        
        std::vector<Row> rows;
        size_t row_count;
        file.read(reinterpret_cast<char*>(&row_count), sizeof(size_t));
        
        if (file.fail()) {
            throw std::runtime_error("Corrupted snapshot file");
        }
        
        for (size_t i = 0; i < row_count; ++i) {
            size_t col_count;
            file.read(reinterpret_cast<char*>(&col_count), sizeof(size_t));
            
            if (file.fail()) break;
            
            Row row;
            for (size_t j = 0; j < col_count; ++j) {
                row.values.push_back(deserialize_value(file));
            }
            rows.push_back(row);
        }
        
        return rows;
    }
    
    void load_snapshots_for_table(const std::string& db, const std::string& table) {
        std::string snapshot_dir = get_snapshot_dir(db, table);
        
        if (!directory_exists(snapshot_dir)) {
            return;
        }
        
        try {
            for (const auto& entry : fs::directory_iterator(snapshot_dir)) {
                if (entry.path().extension() == ".snap") {
                    std::string filename = entry.path().stem().string();
                    
                    if (filename.find('.') != std::string::npos) {
                        auto rows = load_snapshot_from_disk(db, table, filename);
                        snapshots_[db][table].push_back({filename, rows});
                    }
                }
            }
            
            std::sort(snapshots_[db][table].begin(), snapshots_[db][table].end(),
                [](const auto& a, const auto& b) {
                    return a.first < b.first;
                });
            
            std::cout << "Loaded " << snapshots_[db][table].size() << " snapshots for " << table << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Error loading snapshots for " << table << ": " << e.what() << std::endl;
        }
    }
    
public:
    StorageManager(const std::string& data_dir = "./data") : data_dir_(data_dir) {
        create_directory(data_dir_);
        load_all();
    }
    
    void load_all() {
        data_.clear();
        schemas_.clear();
        databases_.clear();
        snapshots_.clear();
        
        if (!directory_exists(data_dir_)) {
            create_directory(data_dir_);
            return;
        }
        
        #ifdef _WIN32
            std::string search_path = data_dir_ + "\\*";
            WIN32_FIND_DATAA find_data;
            HANDLE find_handle = FindFirstFileA(search_path.c_str(), &find_data);
            
            if (find_handle != INVALID_HANDLE_VALUE) {
                do {
                    if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        std::string db_name = find_data.cFileName;
                        if (db_name != "." && db_name != "..") {
                            load_database(db_name);
                        }
                    }
                } while (FindNextFileA(find_handle, &find_data));
                FindClose(find_handle);
            }
        #else
            for (const auto& entry : fs::directory_iterator(data_dir_)) {
                if (entry.is_directory()) {
                    std::string db_name = entry.path().filename().string();
                    load_database(db_name);
                }
            }
        #endif
        
        std::cout << "Loaded " << databases_.size() << " database(s)" << std::endl;
    }
    
    void load_database(const std::string& db_name) {
        std::string db_path = get_db_path(db_name);
        
        if (!directory_exists(db_path)) {
            return;
        }
        
        databases_.insert(db_name);
        data_[db_name] = {};
        schemas_[db_name] = {};
        snapshots_[db_name] = {};
        
        #ifdef _WIN32
            std::string search_path = db_path + "\\*_schema.bin";
            WIN32_FIND_DATAA find_data;
            HANDLE find_handle = FindFirstFileA(search_path.c_str(), &find_data);
            
            if (find_handle != INVALID_HANDLE_VALUE) {
                do {
                    std::string filename = find_data.cFileName;
                    std::string table_name = filename.substr(0, filename.find("_schema.bin"));
                    load_schema(db_name, table_name);
                } while (FindNextFileA(find_handle, &find_data));
                FindClose(find_handle);
            }
        #else
            for (const auto& entry : fs::directory_iterator(db_path)) {
                std::string filename = entry.path().filename().string();
                if (filename.find("_schema.bin") != std::string::npos) {
                    std::string table_name = filename.substr(0, filename.find("_schema.bin"));
                    load_schema(db_name, table_name);
                }
            }
        #endif
        
        #ifdef _WIN32
            search_path = db_path + "\\*.bin";
            find_handle = FindFirstFileA(search_path.c_str(), &find_data);
            
            if (find_handle != INVALID_HANDLE_VALUE) {
                do {
                    std::string filename = find_data.cFileName;
                    if (filename.find("_schema.bin") == std::string::npos) {
                        std::string table_name = filename.substr(0, filename.find(".bin"));
                        if (schemas_[db_name].find(table_name) != schemas_[db_name].end()) {
                            load_table(db_name, table_name);
                            load_snapshots_for_table(db_name, table_name);
                        }
                    }
                } while (FindNextFileA(find_handle, &find_data));
                FindClose(find_handle);
            }
        #endif
    }
    
    void load_table(const std::string& db_name, const std::string& table_name) {
        std::string table_path = get_table_path(db_name, table_name);
        
        if (!file_exists(table_path)) {
            return;
        }
        
        std::ifstream file(table_path, std::ios::binary);
        if (!file) {
            return;
        }
        
        size_t row_count;
        file.read(reinterpret_cast<char*>(&row_count), sizeof(size_t));
        
        if (file.fail()) {
            return;
        }
        
        auto& rows = data_[db_name][table_name];
        rows.clear();
        
        for (size_t i = 0; i < row_count; ++i) {
            size_t col_count;
            file.read(reinterpret_cast<char*>(&col_count), sizeof(size_t));
            
            if (file.fail()) break;
            
            Row row;
            for (size_t j = 0; j < col_count; ++j) {
                row.values.push_back(deserialize_value(file));
            }
            rows.push_back(row);
        }
        
        std::cout << "Loaded table '" << table_name << "' with " << rows.size() << " rows" << std::endl;
    }
    
    void load_schema(const std::string& db_name, const std::string& table_name) {
        std::string schema_path = get_schema_path(db_name, table_name);
        
        if (!file_exists(schema_path)) {
            return;
        }
        
        std::ifstream file(schema_path, std::ios::binary);
        if (!file) {
            return;
        }
        
        TableSchema schema;
        schema.name = table_name;
        
        size_t col_count;
        file.read(reinterpret_cast<char*>(&col_count), sizeof(size_t));
        
        if (file.fail()) {
            return;
        }
        
        for (size_t i = 0; i < col_count; ++i) {
            Column col;
            
            size_t name_len;
            file.read(reinterpret_cast<char*>(&name_len), sizeof(size_t));
            col.name.resize(name_len);
            file.read(&col.name[0], name_len);
            
            int type;
            file.read(reinterpret_cast<char*>(&type), sizeof(int));
            col.type = static_cast<ColumnType>(type);
            
            file.read(reinterpret_cast<char*>(&col.modifiers.not_null), sizeof(bool));
            file.read(reinterpret_cast<char*>(&col.modifiers.indexed), sizeof(bool));
            
            bool has_default;
            file.read(reinterpret_cast<char*>(&has_default), sizeof(bool));
            if (has_default) {
                col.modifiers.default_value = deserialize_value(file);
            }
            
            schema.columns.push_back(col);
            schema.column_index[col.name] = i;
        }
        
        schemas_[db_name][table_name] = schema;
    }
    
    void save_table(const std::string& db_name, const std::string& table_name) {
        std::string db_path = get_db_path(db_name);
        create_directory(db_path);
        
        std::string table_path = get_table_path(db_name, table_name);
        std::ofstream file(table_path, std::ios::binary | std::ios::trunc);
        
        if (!file) {
            throw std::runtime_error("Cannot save table: " + table_name);
        }
        
        const auto& rows = data_[db_name][table_name];
        size_t row_count = rows.size();
        file.write(reinterpret_cast<const char*>(&row_count), sizeof(size_t));
        
        for (const auto& row : rows) {
            size_t col_count = row.values.size();
            file.write(reinterpret_cast<const char*>(&col_count), sizeof(size_t));
            for (const auto& value : row.values) {
                serialize_value(file, value);
            }
        }
        
        save_schema(db_name, table_name);
    }
    
    void save_schema(const std::string& db_name, const std::string& table_name) {
        std::string schema_path = get_schema_path(db_name, table_name);
        std::ofstream file(schema_path, std::ios::binary | std::ios::trunc);
        
        if (!file) {
            throw std::runtime_error("Cannot save schema for table: " + table_name);
        }
        
        const auto& schema = schemas_[db_name][table_name];
        
        size_t col_count = schema.columns.size();
        file.write(reinterpret_cast<const char*>(&col_count), sizeof(size_t));
        
        for (const auto& col : schema.columns) {
            size_t name_len = col.name.size();
            file.write(reinterpret_cast<const char*>(&name_len), sizeof(size_t));
            file.write(col.name.c_str(), name_len);
            
            int type = static_cast<int>(col.type);
            file.write(reinterpret_cast<const char*>(&type), sizeof(int));
            
            file.write(reinterpret_cast<const char*>(&col.modifiers.not_null), sizeof(bool));
            file.write(reinterpret_cast<const char*>(&col.modifiers.indexed), sizeof(bool));
            
            bool has_default = col.modifiers.default_value.has_value();
            file.write(reinterpret_cast<const char*>(&has_default), sizeof(bool));
            if (has_default) {
                serialize_value(file, *col.modifiers.default_value);
            }
        }
    }
    
    void create_snapshot(const std::string& table_name) {
        if (current_db_.empty()) {
            throw std::runtime_error("No database selected");
        }
        
        auto timestamp = get_current_timestamp();
        const auto& rows = data_[current_db_][table_name];
        
        save_snapshot_to_disk(current_db_, table_name, timestamp, rows);
        
        snapshots_[current_db_][table_name].push_back({timestamp, rows});
        
        if (snapshots_[current_db_][table_name].size() > 10) {
            snapshots_[current_db_][table_name].erase(snapshots_[current_db_][table_name].begin());
        }
    }
    
    
    void revert_to_snapshot(const std::string& table_name, const std::chrono::system_clock::time_point& timestamp) {
        std::string timestamp_str = timepoint_to_string(timestamp);
        revert_to_snapshot(table_name, timestamp_str);
    }
    
    void revert_all_tables(const std::chrono::system_clock::time_point& timestamp) {
        std::string timestamp_str = timepoint_to_string(timestamp);
        revert_all_tables(timestamp_str);
    }
    
    void revert_to_snapshot(const std::string& table_name, const std::string& timestamp_str) {
        if (current_db_.empty()) {
            throw std::runtime_error("No database selected");
        }
        
        auto target_time = string_to_timepoint(timestamp_str);
        auto& snaps = snapshots_[current_db_][table_name];
        
        int best_index = -1;
        std::string best_timestamp;
        
        for (size_t i = 0; i < snaps.size(); ++i) {
            auto snap_time = string_to_timepoint(snaps[i].first);
            if (snap_time <= target_time) {
                best_index = i;
                best_timestamp = snaps[i].first;
            }
        }
        
        if (best_index == -1) {
            std::string snapshot_dir = get_snapshot_dir(current_db_, table_name);
            if (directory_exists(snapshot_dir)) {
                std::vector<std::string> snapshots_on_disk;
                
                for (const auto& entry : fs::directory_iterator(snapshot_dir)) {
                    if (entry.path().extension() == ".snap") {
                        std::string ts = entry.path().stem().string();
                        auto snap_time = string_to_timepoint(ts);
                        if (snap_time <= target_time) {
                            snapshots_on_disk.push_back(ts);
                        }
                    }
                }
                
                if (!snapshots_on_disk.empty()) {
                    std::sort(snapshots_on_disk.begin(), snapshots_on_disk.end());
                    best_timestamp = snapshots_on_disk.back();
                    
                    auto rows = load_snapshot_from_disk(current_db_, table_name, best_timestamp);
                    data_[current_db_][table_name] = rows;
                    save_table(current_db_, table_name);
                    
                    std::cout << "Reverted " << table_name << " to snapshot from disk: " << best_timestamp << std::endl;
                    return;
                }
            }
            
            throw std::runtime_error("No snapshot found before timestamp: " + timestamp_str);
        }
        
        data_[current_db_][table_name] = snaps[best_index].second;
        save_table(current_db_, table_name);
        
        std::cout << "Reverted " << table_name << " to snapshot: " << best_timestamp << std::endl;
    }
    
    void revert_all_tables(const std::string& timestamp_str) {
        if (current_db_.empty()) {
            throw std::runtime_error("No database selected");
        }
        
        int reverted_count = 0;
        
        for (const auto& [table_name, _] : schemas_[current_db_]) {
            try {
                revert_to_snapshot(table_name, timestamp_str);
                reverted_count++;
            } catch (const std::exception& e) {
                std::cerr << "Warning: Could not revert " << table_name << ": " << e.what() << std::endl;
            }
        }
        
        if (reverted_count == 0) {
            throw std::runtime_error("No tables could be reverted to timestamp: " + timestamp_str);
        }
        
        std::cout << "Reverted " << reverted_count << " tables to " << timestamp_str << std::endl;
    }
    
    std::map<std::string, TableSchema> get_all_schemas(const std::string& db_name) const {
        auto it = schemas_.find(db_name);
        if (it != schemas_.end()) {
            return it->second;
        }
        return {};
    }
    
    void list_snapshots(const std::string& table_name) {
        if (current_db_.empty()) {
            std::cout << "No database selected" << std::endl;
            return;
        }
        
        std::string snapshot_dir = get_snapshot_dir(current_db_, table_name);
        if (!directory_exists(snapshot_dir)) {
            std::cout << "No snapshots for table: " << table_name << std::endl;
            return;
        }
        
        std::cout << "Snapshots for " << table_name << ":" << std::endl;
        for (const auto& entry : fs::directory_iterator(snapshot_dir)) {
            if (entry.path().extension() == ".snap") {
                std::string ts = entry.path().stem().string();
                auto file_size = fs::file_size(entry.path());
                std::cout << "  - " << ts << " (" << file_size << " bytes)" << std::endl;
            }
        }
    }
    
    void create_database(const std::string& name) {
        if (databases_.count(name)) {
            throw std::runtime_error("Database already exists: " + name);
        }
        
        std::string db_path = get_db_path(name);
        create_directory(db_path);
        
        databases_.insert(name);
        data_[name] = {};
        schemas_[name] = {};
        snapshots_[name] = {};
        
        std::cout << "Database '" << name << "' created at: " << db_path << std::endl;
    }
    
    void drop_database(const std::string& name) {
        if (!databases_.count(name)) {
            throw std::runtime_error("Database does not exist: " + name);
        }
        
        std::string db_path = get_db_path(name);
        if (directory_exists(db_path)) {
            std::cout << "Warning: Database files not deleted from disk: " << db_path << std::endl;
        }
        
        databases_.erase(name);
        data_.erase(name);
        schemas_.erase(name);
        snapshots_.erase(name);
        
        if (current_db_ == name) {
            current_db_.clear();
        }
        
        std::cout << "Database '" << name << "' dropped from memory" << std::endl;
    }
    
    void use_database(const std::string& name) {
        if (!databases_.count(name)) {
            throw std::runtime_error("Database does not exist: " + name);
        }
        current_db_ = name;
        std::cout << "Now using database: " << current_db_ << std::endl;
    }
    
    void create_table(const std::string& name, const std::vector<Column>& columns) {
        if (current_db_.empty()) {
            throw std::runtime_error("No database selected. Use USE <database> first");
        }
        
        if (schemas_[current_db_].count(name)) {
            throw std::runtime_error("Table already exists: " + name);
        }
        
        TableSchema schema;
        schema.name = name;
        schema.columns = columns;
        
        for (size_t i = 0; i < columns.size(); ++i) {
            schema.column_index[columns[i].name] = i;
        }
        
        schemas_[current_db_][name] = schema;
        data_[current_db_][name] = {};
        
        save_schema(current_db_, name);
        save_table(current_db_, name);
        
        std::string snapshot_dir = get_snapshot_dir(current_db_, name);
        create_directory(snapshot_dir);
        
        create_snapshot(name);
        
        std::cout << "Table '" << name << "' created" << std::endl;
    }
    
    void drop_table(const std::string& name) {
        if (current_db_.empty()) {
            throw std::runtime_error("No database selected");
        }
        
        if (!schemas_[current_db_].count(name)) {
            throw std::runtime_error("Table does not exist: " + name);
        }
        
        schemas_[current_db_].erase(name);
        data_[current_db_].erase(name);
        
        std::string table_path = get_table_path(current_db_, name);
        std::string schema_path = get_schema_path(current_db_, name);
        
        if (file_exists(table_path)) {
            std::remove(table_path.c_str());
        }
        if (file_exists(schema_path)) {
            std::remove(schema_path.c_str());
        }
        
        std::cout << "Table '" << name << "' dropped" << std::endl;
    }
    
    const TableSchema& get_schema(const std::string& table_name) const {
        if (current_db_.empty()) {
            throw std::runtime_error("No database selected");
        }
        
        auto it = schemas_.find(current_db_);
        if (it == schemas_.end()) {
            throw std::runtime_error("Database not found");
        }
        
        auto it2 = it->second.find(table_name);
        if (it2 == it->second.end()) {
            throw std::runtime_error("Table not found: " + table_name);
        }
        
        return it2->second;
    }
    
    std::vector<Row>& get_table_data(const std::string& table_name) {
        if (current_db_.empty()) {
            throw std::runtime_error("No database selected");
        }
        return data_[current_db_][table_name];
    }
    
    const std::vector<Row>& get_table_data(const std::string& table_name) const {
        if (current_db_.empty()) {
            throw std::runtime_error("No database selected");
        }
        
        auto it = data_.find(current_db_);
        if (it == data_.end()) {
            throw std::runtime_error("Database not found");
        }
        
        auto it2 = it->second.find(table_name);
        if (it2 == it->second.end()) {
            throw std::runtime_error("Table not found");
        }
        
        return it2->second;
    }
    
    std::string get_current_db() const { return current_db_; }
    const std::set<std::string>& get_databases() const { return databases_; }
    
    void save_all() {
        for (const auto& [db_name, tables] : data_) {
            for (const auto& [table_name, _] : tables) {
                save_table(db_name, table_name);
            }
        }
        std::cout << "All data saved to disk" << std::endl;
    }
    
    void list_databases() const {
        std::cout << "Databases (" << databases_.size() << "): ";
        for (const auto& db : databases_) {
            std::cout << db << " ";
        }
        std::cout << std::endl;
    }
    
    void list_tables() const {
        if (current_db_.empty()) {
            std::cout << "No database selected" << std::endl;
            return;
        }
        
        std::cout << "Tables in '" << current_db_ << "': ";
        for (const auto& [table_name, _] : schemas_.at(current_db_)) {
            std::cout << table_name << " ";
        }
        std::cout << std::endl;
    }
};

}

#endif