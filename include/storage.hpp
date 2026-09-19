#ifndef DBMS_STORAGE_HPP
#define DBMS_STORAGE_HPP

#include "types.hpp"
#include "btree.hpp"
#include "index_file.hpp"

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
#include <mutex>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define mkdir _mkdir
#else
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace dbms {

inline thread_local std::string tls_current_db;

class StorageManager {
private:
    std::string data_dir_;
    std::map<std::string, std::map<std::string, std::vector<Row>>> data_;
    std::map<std::string, std::map<std::string, TableSchema>> schemas_;
    std::set<std::string> databases_;
    mutable std::mutex storage_mutex_;

    std::map<std::string, std::map<std::string,
             std::vector<std::pair<std::string, std::vector<Row>>>>> snapshots_;


    struct IndexTree {
        std::string path;
        std::shared_ptr<std::fstream> file;
        std::unique_ptr<DiskBPlusTree<int>>      int_tree;
        std::unique_ptr<DiskBPlusTree<IndexKey>> str_tree;
        bool is_int = false;
    };


    std::map<std::string, IndexTree> index_trees_;



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
            std::error_code ec;
            fs::create_directories(path, ec);
            if (ec) std::cerr << "Warning: cannot create " << path
                              << ": " << ec.message() << "\n";
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

    std::string get_index_path(const std::string& db,
                               const std::string& table,
                               const std::string& column) const {
        return get_db_path(db) + "/" + table + "__" + column + ".idx";
    }

    std::string get_snapshot_dir(const std::string& db, const std::string& table) const {
        return get_db_path(db) + "/snapshots/" + table;
    }

    std::string index_key(const std::string& db,
                          const std::string& table,
                          const std::string& column) const {
        return db + "/" + table + "/" + column;
    }



    std::string timepoint_to_string(const std::chrono::system_clock::time_point& tp) const {
        auto time_t = std::chrono::system_clock::to_time_t(tp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            tp.time_since_epoch()) % 1000;
        std::tm* tm = std::localtime(&time_t);
        char buffer[32];
        strftime(buffer, sizeof(buffer), "%Y.%m.%d-%H-%M-%S", tm);
        std::stringstream ss;
        ss << buffer << "." << std::setw(3) << std::setfill('0') << ms.count();
        return ss.str();
    }

    std::chrono::system_clock::time_point string_to_timepoint(const std::string& t) const {
        std::tm tm = {};
        int ms = 0;
        if (t.find(':') != std::string::npos) {
            sscanf(t.c_str(), "%d.%d.%d-%d:%d:%d.%d",
                   &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                   &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &ms);
        } else {
            sscanf(t.c_str(), "%d.%d.%d-%d-%d-%d.%d",
                   &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                   &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &ms);
        }
        tm.tm_year -= 1900; tm.tm_mon -= 1;
        auto tt = mktime(&tm);
        auto tp = std::chrono::system_clock::from_time_t(tt);
        tp += std::chrono::milliseconds(ms);
        return tp;
    }

    std::string get_current_timestamp() const {
        return timepoint_to_string(std::chrono::system_clock::now());
    }



    void serialize_value(std::ofstream& file, const Value& value) const {
        if (std::holds_alternative<NullType>(value)) {
            char t = 0; file.write(&t, 1);
        } else if (std::holds_alternative<int>(value)) {
            char t = 1; file.write(&t, 1);
            int v = std::get<int>(value);
            file.write(reinterpret_cast<const char*>(&v), sizeof(int));
        } else if (std::holds_alternative<StringRef>(value)) {
            char t = 2; file.write(&t, 1);
            const auto& s = *std::get<StringRef>(value);
            size_t len = s.size();
            file.write(reinterpret_cast<const char*>(&len), sizeof(size_t));
            file.write(s.c_str(), len);
        }
    }

    Value deserialize_value(std::ifstream& file) const {
        char t;
        file.read(&t, 1);
        if (file.eof() || file.fail()) return NullType{};
        if (t == 0) return NullType{};
        if (t == 1) {
            int v; file.read(reinterpret_cast<char*>(&v), sizeof(int));
            if (file.fail()) return NullType{};
            return v;
        }
        if (t == 2) {
            size_t len; file.read(reinterpret_cast<char*>(&len), sizeof(size_t));
            if (file.fail() || len > 10000000) return NullType{};
            std::string s(len, '\0');
            file.read(&s[0], len);
            if (file.fail()) return NullType{};
            return make_string_ref(s);
        }
        return NullType{};
    }



    void save_snapshot_to_disk(const std::string& db, const std::string& table,
                               const std::string& ts, const std::vector<Row>& rows) {
        std::string dir = get_snapshot_dir(db, table);
        create_directory(dir);
        std::string path = dir + "/" + ts + ".snap";
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file) return;
        size_t rc = rows.size();
        file.write(reinterpret_cast<const char*>(&rc), sizeof(size_t));
        for (const auto& row : rows) {
            size_t cc = row.values.size();
            file.write(reinterpret_cast<const char*>(&cc), sizeof(size_t));
            for (const auto& v : row.values) serialize_value(file, v);
        }
    }

    std::vector<Row> load_snapshot_from_disk(const std::string& db, const std::string& table,
                                             const std::string& ts) {
        std::string path = get_snapshot_dir(db, table) + "/" + ts + ".snap";
        if (!file_exists(path)) throw std::runtime_error("Snapshot not found: " + ts);
        std::ifstream file(path, std::ios::binary);
        if (!file) throw std::runtime_error("Cannot load snapshot: " + ts);
        std::vector<Row> rows;
        size_t rc; file.read(reinterpret_cast<char*>(&rc), sizeof(size_t));
        if (file.fail()) throw std::runtime_error("Corrupted snapshot");
        for (size_t i = 0; i < rc; ++i) {
            size_t cc; file.read(reinterpret_cast<char*>(&cc), sizeof(size_t));
            if (file.fail()) break;
            Row row;
            for (size_t j = 0; j < cc; ++j) row.values.push_back(deserialize_value(file));
            rows.push_back(row);
        }
        return rows;
    }

    void load_snapshots_for_table(const std::string& db, const std::string& table) {
        std::string dir = get_snapshot_dir(db, table);
        if (!directory_exists(dir)) return;
        try {
            for (const auto& e : fs::directory_iterator(dir)) {
                if (e.path().extension() == ".snap") {
                    std::string fn = e.path().stem().string();
                    if (fn.find('.') != std::string::npos) {
                        auto rows = load_snapshot_from_disk(db, table, fn);
                        snapshots_[db][table].push_back({fn, rows});
                    }
                }
            }
            std::sort(snapshots_[db][table].begin(), snapshots_[db][table].end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
        } catch (const std::exception& e) {
            std::cerr << "Error loading snapshots for " << table << ": " << e.what() << "\n";
        }
    }




    void open_index_file_for_column(const std::string& db,
                                    const std::string& table,
                                    const std::string& column,
                                    uint8_t key_type,
                                    bool create_if_missing) {
        std::string key = index_key(db, table, column);
        if (index_trees_.count(key)) return;

        std::string path = get_index_path(db, table, column);
        bool exists = file_exists(path);
        if (!exists && !create_if_missing) return;

        auto file = std::make_shared<std::fstream>();
        if (!exists) {
            file->open(path, std::ios::out | std::ios::binary);
            file->close();
        }
        file->open(path, std::ios::in | std::ios::out | std::ios::binary);
        if (!*file) throw std::runtime_error("Cannot open index file: " + path);
        file->clear();

        IndexTree it;
        it.path = path;
        it.file = file;
        it.is_int = (key_type == 0);

        if (!exists) {

            if (it.is_int) DiskBPlusTree<int>::create(*file, 0, key_type);
            else           DiskBPlusTree<IndexKey>::create(*file, 0, key_type);
            file->flush();
        }

        if (it.is_int) {
            it.int_tree = std::make_unique<DiskBPlusTree<int>>(*file, 0);
        } else {
            it.str_tree = std::make_unique<DiskBPlusTree<IndexKey>>(*file, 0);
        }

        index_trees_[key] = std::move(it);
    }

    DiskBPlusTree<int>* get_int_index(const std::string& db,
                                      const std::string& table,
                                      const std::string& column) {
        std::string key = index_key(db, table, column);
        auto it = index_trees_.find(key);
        if (it == index_trees_.end()) {
            open_index_file_for_column(db, table, column, /*key_type=*/0, true);
            it = index_trees_.find(key);
            if (it == index_trees_.end()) throw std::runtime_error("Cannot open int index");
        }
        return it->second.int_tree.get();
    }

    DiskBPlusTree<IndexKey>* get_str_index(const std::string& db,
                                           const std::string& table,
                                           const std::string& column) {
        std::string key = index_key(db, table, column);
        auto it = index_trees_.find(key);
        if (it == index_trees_.end()) {
            open_index_file_for_column(db, table, column, /*key_type=*/1, true);
            it = index_trees_.find(key);
            if (it == index_trees_.end()) throw std::runtime_error("Cannot open str index");
        }
        return it->second.str_tree.get();
    }

    void flush_index_file(const std::string& db, const std::string& table) {
        std::string prefix = db + "/" + table + "/";
        for (auto& [key, tree] : index_trees_) {
            if (key.rfind(prefix, 0) != 0) continue;
            if (tree.int_tree) tree.int_tree->flush();
            if (tree.str_tree) tree.str_tree->flush();
            if (tree.file && tree.file->is_open()) tree.file->flush();
        }
    }

    void close_all_indexes_for_table(const std::string& db, const std::string& table) {
        std::string prefix = db + "/" + table + "/";
        for (auto it = index_trees_.begin(); it != index_trees_.end(); ) {
            if (it->first.rfind(prefix, 0) == 0) {
                if (it->second.int_tree) it->second.int_tree->flush();
                if (it->second.str_tree) it->second.str_tree->flush();
                if (it->second.file && it->second.file->is_open()) {
                    it->second.file->flush();
                    it->second.file->close();
                }
                it = index_trees_.erase(it);
            } else ++it;
        }
    }

    void remove_all_index_files(const std::string& db, const std::string& table) {

        close_all_indexes_for_table(db, table);

        std::string db_path = get_db_path(db);
        if (!directory_exists(db_path)) return;
        std::string prefix = table + "__";
        for (const auto& e : fs::directory_iterator(db_path)) {
            std::string fn = e.path().filename().string();
            if (fn.rfind(prefix, 0) == 0 &&
                fn.size() > 4 &&
                fn.substr(fn.size() - 4) == ".idx") {
                std::error_code ec;
                fs::remove(e.path(), ec);
            }
        }
    }

    void rebuild_indexes_from_data(const std::string& db, const std::string& table) {
        auto& schema = schemas_[db][table];
        auto& rows   = data_[db][table];

        remove_all_index_files(db, table);

        for (size_t col = 0; col < schema.columns.size(); ++col) {
            const auto& c = schema.columns[col];
            if (!c.modifiers.indexed) continue;

            uint8_t kt = (c.type == ColumnType::INT) ? 0 : 1;
            open_index_file_for_column(db, table, c.name, kt, /*create=*/true);

            if (c.type == ColumnType::INT) {
                auto* tree = get_int_index(db, table, c.name);
                for (size_t i = 0; i < rows.size(); ++i) {
                    if (!rows[i].is_null(col)) tree->insert(rows[i].get_int(col), i);
                }
            } else {
                auto* tree = get_str_index(db, table, c.name);
                for (size_t i = 0; i < rows.size(); ++i) {
                    if (!rows[i].is_null(col)) {
                        StringRef r = std::get<StringRef>(rows[i].values[col]);
                        tree->insert(IndexKey(r), i);
                    }
                }
            }
        }

        flush_index_file(db, table);
        std::cout << "Rebuilt indexes for " << table << "\n";
    }

public:


    DiskBPlusTree<int>* get_index_int(const std::string& table, const std::string& column) {
        if (tls_current_db.empty()) throw std::runtime_error("No database selected");
        return get_int_index(tls_current_db, table, column);
    }

    DiskBPlusTree<IndexKey>* get_index_str(const std::string& table, const std::string& column) {
        if (tls_current_db.empty()) throw std::runtime_error("No database selected");
        return get_str_index(tls_current_db, table, column);
    }

    void flush_indexes(const std::string& table) {
        if (tls_current_db.empty()) return;
        flush_index_file(tls_current_db, table);
    }

    void rebuild_indexes_public(const std::string& table) {
        if (tls_current_db.empty()) return;
        rebuild_indexes_from_data(tls_current_db, table);
    }



    StorageManager(const std::string& data_dir = "./data") : data_dir_(data_dir) {
        create_directory(data_dir_);
        load_all();
    }

    ~StorageManager() {
        for (auto& [key, tree] : index_trees_) {
            if (tree.file && tree.file->is_open()) {
                if (tree.int_tree) tree.int_tree->flush();
                if (tree.str_tree) tree.str_tree->flush();
                tree.file->flush();
                tree.file->close();
            }
        }
    }



    void load_all() {
        data_.clear(); schemas_.clear(); databases_.clear(); snapshots_.clear();

        if (!directory_exists(data_dir_)) { create_directory(data_dir_); return; }

        #ifdef _WIN32
            std::string sp = data_dir_ + "\\*";
            WIN32_FIND_DATAA fd;
            HANDLE fh = FindFirstFileA(sp.c_str(), &fd);
            if (fh != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        std::string n = fd.cFileName;
                        if (n != "." && n != "..") load_database(n);
                    }
                } while (FindNextFileA(fh, &fd));
                FindClose(fh);
            }
        #else
            for (const auto& e : fs::directory_iterator(data_dir_)) {
                if (e.is_directory()) load_database(e.path().filename().string());
            }
        #endif

        std::cout << "Loaded " << databases_.size() << " database(s)" << "\n";
    }

    void load_database(const std::string& db_name) {
        std::string db_path = get_db_path(db_name);
        if (!directory_exists(db_path)) return;

        databases_.insert(db_name);
        data_[db_name] = {}; schemas_[db_name] = {}; snapshots_[db_name] = {};


        #ifdef _WIN32
            std::string sp = db_path + "\\*_schema.bin";
            WIN32_FIND_DATAA fd;
            HANDLE fh = FindFirstFileA(sp.c_str(), &fd);
            if (fh != INVALID_HANDLE_VALUE) {
                do {
                    std::string fn = fd.cFileName;
                    load_schema(db_name, fn.substr(0, fn.find("_schema.bin")));
                } while (FindNextFileA(fh, &fd));
                FindClose(fh);
            }
        #else
            for (const auto& e : fs::directory_iterator(db_path)) {
                std::string fn = e.path().filename().string();
                if (fn.find("_schema.bin") != std::string::npos) {
                    load_schema(db_name, fn.substr(0, fn.find("_schema.bin")));
                }
            }
        #endif


        #ifdef _WIN32
            sp = db_path + "\\*.bin";
            fh = FindFirstFileA(sp.c_str(), &fd);
            if (fh != INVALID_HANDLE_VALUE) {
                do {
                    std::string fn = fd.cFileName;
                    if (fn.find("_schema.bin") == std::string::npos) {
                        std::string tn = fn.substr(0, fn.find(".bin"));
                        if (schemas_[db_name].count(tn)) {
                            load_table(db_name, tn);
                            load_snapshots_for_table(db_name, tn);



                            const auto& schema = schemas_[db_name][tn];
                            for (const auto& c : schema.columns) {
                                if (!c.modifiers.indexed) continue;
                                uint8_t kt = (c.type == ColumnType::INT) ? 0 : 1;
                                std::string p = get_index_path(db_name, tn, c.name);
                                if (file_exists(p)) {
                                    open_index_file_for_column(db_name, tn, c.name, kt, false);
                                } else {

                                    rebuild_indexes_from_data(db_name, tn);
                                    break;
                                }
                            }
                        }
                    }
                } while (FindNextFileA(fh, &fd));
                FindClose(fh);
            }
        #else
            for (const auto& e : fs::directory_iterator(db_path)) {
                std::string fn = e.path().filename().string();
                if (fn.size() > 4 && fn.substr(fn.size() - 4) == ".bin" &&
                    fn.find("_schema.bin") == std::string::npos) {
                    std::string tn = fn.substr(0, fn.size() - 4);
                    if (schemas_[db_name].count(tn)) {
                        load_table(db_name, tn);
                        load_snapshots_for_table(db_name, tn);

                        const auto& schema = schemas_[db_name][tn];
                        for (const auto& c : schema.columns) {
                            if (!c.modifiers.indexed) continue;
                            uint8_t kt = (c.type == ColumnType::INT) ? 0 : 1;
                            std::string p = get_index_path(db_name, tn, c.name);
                            if (file_exists(p)) {
                                open_index_file_for_column(db_name, tn, c.name, kt, false);
                            } else {
                                rebuild_indexes_from_data(db_name, tn);
                                break;
                            }
                        }
                    }
                }
            }
        #endif
    }

    void load_table(const std::string& db_name, const std::string& table_name) {
        std::string path = get_table_path(db_name, table_name);
        if (!file_exists(path)) return;

        std::ifstream file(path, std::ios::binary);
        if (!file) return;

        size_t rc;
        file.read(reinterpret_cast<char*>(&rc), sizeof(size_t));
        if (file.fail()) return;

        auto& rows = data_[db_name][table_name];
        rows.clear();

        for (size_t i = 0; i < rc; ++i) {
            size_t cc;
            file.read(reinterpret_cast<char*>(&cc), sizeof(size_t));
            if (file.fail()) break;
            Row row;
            for (size_t j = 0; j < cc; ++j) row.values.push_back(deserialize_value(file));
            rows.push_back(row);
        }
        std::cout << "Loaded table '" << table_name << "' with " << rows.size() << " rows" << "\n";
    }

    void load_schema(const std::string& db_name, const std::string& table_name) {
        std::string path = get_schema_path(db_name, table_name);
        if (!file_exists(path)) return;

        std::ifstream file(path, std::ios::binary);
        if (!file) return;

        TableSchema schema;
        schema.name = table_name;

        size_t cc;
        file.read(reinterpret_cast<char*>(&cc), sizeof(size_t));
        if (file.fail()) return;

        for (size_t i = 0; i < cc; ++i) {
            Column col;
            size_t nl;
            file.read(reinterpret_cast<char*>(&nl), sizeof(size_t));
            col.name.resize(nl);
            file.read(&col.name[0], nl);

            int t;
            file.read(reinterpret_cast<char*>(&t), sizeof(int));
            col.type = static_cast<ColumnType>(t);

            file.read(reinterpret_cast<char*>(&col.modifiers.not_null), sizeof(bool));
            file.read(reinterpret_cast<char*>(&col.modifiers.indexed), sizeof(bool));

            bool hd;
            file.read(reinterpret_cast<char*>(&hd), sizeof(bool));
            if (hd) col.modifiers.default_value = deserialize_value(file);

            schema.columns.push_back(col);
            schema.column_index[col.name] = i;
        }
        schemas_[db_name][table_name] = schema;
    }



    void save_table(const std::string& db_name, const std::string& table_name) {
        create_directory(get_db_path(db_name));
        std::string path = get_table_path(db_name, table_name);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file) throw std::runtime_error("Cannot save table: " + table_name);

        const auto& rows = data_[db_name][table_name];
        size_t rc = rows.size();
        file.write(reinterpret_cast<const char*>(&rc), sizeof(size_t));

        for (const auto& row : rows) {
            size_t cc = row.values.size();
            file.write(reinterpret_cast<const char*>(&cc), sizeof(size_t));
            for (const auto& v : row.values) serialize_value(file, v);
        }
        save_schema(db_name, table_name);
    }

    void save_schema(const std::string& db_name, const std::string& table_name) {
        std::string path = get_schema_path(db_name, table_name);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file) throw std::runtime_error("Cannot save schema for table: " + table_name);

        const auto& schema = schemas_[db_name][table_name];
        size_t cc = schema.columns.size();
        file.write(reinterpret_cast<const char*>(&cc), sizeof(size_t));

        for (const auto& col : schema.columns) {
            size_t nl = col.name.size();
            file.write(reinterpret_cast<const char*>(&nl), sizeof(size_t));
            file.write(col.name.c_str(), nl);

            int t = static_cast<int>(col.type);
            file.write(reinterpret_cast<const char*>(&t), sizeof(int));

            file.write(reinterpret_cast<const char*>(&col.modifiers.not_null), sizeof(bool));
            file.write(reinterpret_cast<const char*>(&col.modifiers.indexed), sizeof(bool));

            bool hd = col.modifiers.default_value.has_value();
            file.write(reinterpret_cast<const char*>(&hd), sizeof(bool));
            if (hd) serialize_value(file, *col.modifiers.default_value);
        }
    }



    void create_snapshot(const std::string& table_name) {
        if (tls_current_db.empty()) throw std::runtime_error("No database selected");
        auto ts = get_current_timestamp();
        const auto& rows = data_[tls_current_db][table_name];
        save_snapshot_to_disk(tls_current_db, table_name, ts, rows);
        snapshots_[tls_current_db][table_name].push_back({ts, rows});
        if (snapshots_[tls_current_db][table_name].size() > 10) {
            snapshots_[tls_current_db][table_name].erase(
                snapshots_[tls_current_db][table_name].begin());
        }
    }

    void revert_to_snapshot(const std::string& table_name,
                            const std::chrono::system_clock::time_point& tp) {
        revert_to_snapshot(table_name, timepoint_to_string(tp));
    }

    void revert_all_tables(const std::chrono::system_clock::time_point& tp) {
        revert_all_tables(timepoint_to_string(tp));
    }

    void revert_to_snapshot(const std::string& table_name, const std::string& ts_str) {
        if (tls_current_db.empty()) throw std::runtime_error("No database selected");
        auto target = string_to_timepoint(ts_str);
        auto& snaps = snapshots_[tls_current_db][table_name];

        int best = -1;
        std::string best_ts;
        for (size_t i = 0; i < snaps.size(); ++i) {
            auto t = string_to_timepoint(snaps[i].first);
            if (t <= target) { best = (int)i; best_ts = snaps[i].first; }
        }

        if (best == -1) {
            std::string dir = get_snapshot_dir(tls_current_db, table_name);
            if (directory_exists(dir)) {
                std::vector<std::string> on_disk;
                for (const auto& e : fs::directory_iterator(dir)) {
                    if (e.path().extension() == ".snap") {
                        std::string ts = e.path().stem().string();
                        if (string_to_timepoint(ts) <= target) on_disk.push_back(ts);
                    }
                }
                if (!on_disk.empty()) {
                    std::sort(on_disk.begin(), on_disk.end());
                    best_ts = on_disk.back();
                    auto rows = load_snapshot_from_disk(tls_current_db, table_name, best_ts);
                    data_[tls_current_db][table_name] = rows;
                    save_table(tls_current_db, table_name);
                    rebuild_indexes_public(table_name);
                    std::cout << "Reverted " << table_name << " to snapshot from disk: "
                              << best_ts << "\n";
                    return;
                }
            }
            throw std::runtime_error("No snapshot found before timestamp: " + ts_str);
        }

        data_[tls_current_db][table_name] = snaps[best].second;
        save_table(tls_current_db, table_name);
        rebuild_indexes_public(table_name);
        std::cout << "Reverted " << table_name << " to snapshot: " << best_ts << "\n";
    }

    void revert_all_tables(const std::string& ts_str) {
        if (tls_current_db.empty()) throw std::runtime_error("No database selected");
        int cnt = 0;
        for (const auto& [tn, _] : schemas_[tls_current_db]) {
            try { revert_to_snapshot(tn, ts_str); ++cnt; }
            catch (const std::exception& e) {
                std::cerr << "Warning: cannot revert " << tn << ": " << e.what() << "\n";
            }
        }
        if (cnt == 0) throw std::runtime_error("No tables reverted to " + ts_str);
        std::cout << "Reverted " << cnt << " tables to " << ts_str << "\n";
    }

    void list_snapshots(const std::string& table_name) {
        if (tls_current_db.empty()) { std::cout << "No database selected" << "\n"; return; }
        std::string dir = get_snapshot_dir(tls_current_db, table_name);
        if (!directory_exists(dir)) {
            std::cout << "No snapshots for " << table_name << "\n"; return;
        }
        std::cout << "Snapshots for " << table_name << ":\n";
        for (const auto& e : fs::directory_iterator(dir)) {
            if (e.path().extension() == ".snap") {
                std::cout << "  - " << e.path().stem().string()
                          << " (" << fs::file_size(e.path()) << " bytes)\n";
            }
        }
    }



    void create_database(const std::string& name) {
        std::lock_guard<std::mutex> lock(storage_mutex_);
        if (databases_.count(name)) throw std::runtime_error("Database already exists: " + name);
        std::string p = get_db_path(name);
        create_directory(p);
        databases_.insert(name);
        data_[name] = {}; schemas_[name] = {}; snapshots_[name] = {};
        std::cout << "Database '" << name << "' created at: " << p << "\n";
    }

    void drop_database(const std::string& name) {
        std::lock_guard<std::mutex> lock(storage_mutex_);
        if (!databases_.count(name)) throw std::runtime_error("Database does not exist: " + name);

        std::string prefix = name + "/";
        for (auto it = index_trees_.begin(); it != index_trees_.end(); ) {
            if (it->first.rfind(prefix, 0) == 0) {
                if (it->second.file && it->second.file->is_open()) {
                    it->second.file->flush();
                    it->second.file->close();
                }
                it = index_trees_.erase(it);
            } else ++it;
        }

        databases_.erase(name);
        data_.erase(name); schemas_.erase(name); snapshots_.erase(name);
        if (tls_current_db == name) tls_current_db.clear();
        std::cout << "Database '" << name << "' dropped from memory" << "\n";
    }

    void use_database(const std::string& name) {
        std::lock_guard<std::mutex> lock(storage_mutex_);
        if (!databases_.count(name)) throw std::runtime_error("Database does not exist: " + name);
        tls_current_db = name;
        std::cout << "Now using database: " << tls_current_db << "\n";
    }



    void create_table(const std::string& name, const std::vector<Column>& columns) {
        std::lock_guard<std::mutex> lock(storage_mutex_);
        if (tls_current_db.empty())
            throw std::runtime_error("No database selected. Use USE <database> first");
        if (schemas_[tls_current_db].count(name))
            throw std::runtime_error("Table already exists: " + name);

        TableSchema schema;
        schema.name = name;
        schema.columns = columns;
        for (size_t i = 0; i < columns.size(); ++i)
            schema.column_index[columns[i].name] = i;

        schemas_[tls_current_db][name] = schema;
        data_[tls_current_db][name] = {};

        save_schema(tls_current_db, name);
        save_table(tls_current_db, name);

        create_directory(get_snapshot_dir(tls_current_db, name));
        create_snapshot(name);

        for (const auto& c : columns) {
            if (!c.modifiers.indexed) continue;
            uint8_t kt = (c.type == ColumnType::INT) ? 0 : 1;
            open_index_file_for_column(tls_current_db, name, c.name, kt, /*create=*/true);
        }
        flush_index_file(tls_current_db, name);

        std::cout << "Table '" << name << "' created" << "\n";
    }

    void drop_table(const std::string& name) {
        std::lock_guard<std::mutex> lock(storage_mutex_);
        if (tls_current_db.empty()) throw std::runtime_error("No database selected");
        if (!schemas_[tls_current_db].count(name))
            throw std::runtime_error("Table does not exist: " + name);

        remove_all_index_files(tls_current_db, name);

        schemas_[tls_current_db].erase(name);
        data_[tls_current_db].erase(name);

        std::string tp = get_table_path(tls_current_db, name);
        std::string sp = get_schema_path(tls_current_db, name);
        if (file_exists(tp)) std::remove(tp.c_str());
        if (file_exists(sp)) std::remove(sp.c_str());

        std::cout << "Table '" << name << "' dropped" << "\n";
    }

    const TableSchema& get_schema(const std::string& table_name) const {
        if (tls_current_db.empty()) throw std::runtime_error("No database selected");
        auto it = schemas_.find(tls_current_db);
        if (it == schemas_.end()) throw std::runtime_error("Database not found");
        auto it2 = it->second.find(table_name);
        if (it2 == it->second.end()) throw std::runtime_error("Table not found: " + table_name);
        return it2->second;
    }

    std::vector<Row>& get_table_data(const std::string& table_name) {
        std::lock_guard<std::mutex> lock(storage_mutex_);
        if (tls_current_db.empty()) throw std::runtime_error("No database selected");
        return data_[tls_current_db][table_name];
    }

    const std::vector<Row>& get_table_data(const std::string& table_name) const {
        if (tls_current_db.empty()) throw std::runtime_error("No database selected");
        auto it = data_.find(tls_current_db);
        if (it == data_.end()) throw std::runtime_error("Database not found");
        auto it2 = it->second.find(table_name);
        if (it2 == it->second.end()) throw std::runtime_error("Table not found");
        return it2->second;
    }

    std::string get_current_db() const { return tls_current_db; }
    const std::set<std::string>& get_databases() const { return databases_; }

    void save_all() {
        for (const auto& [db, tables] : data_) {
            for (const auto& [tn, _] : tables) save_table(db, tn);
        }
        for (auto& [key, tree] : index_trees_) {
            if (tree.int_tree) tree.int_tree->flush();
            if (tree.str_tree) tree.str_tree->flush();
            if (tree.file && tree.file->is_open()) tree.file->flush();
        }
        std::cout << "All data saved to disk" << "\n";
    }

    void list_databases() const {
        std::cout << "Databases (" << databases_.size() << "): ";
        for (const auto& db : databases_) std::cout << db << " ";
        std::cout << "\n";
    }

    void list_tables() const {
        if (tls_current_db.empty()) { std::cout << "No database selected" << "\n"; return; }
        std::cout << "Tables in '" << tls_current_db << "': ";
        for (const auto& [tn, _] : schemas_.at(tls_current_db)) std::cout << tn << " ";
        std::cout << "\n";
    }
};

} // namespace dbms

#endif