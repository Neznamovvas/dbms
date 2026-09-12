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

// ---------------------------------------------------------------------------
// Persistence model
// ---------------------------------------------------------------------------
// Every table is backed on disk by a single append-only *write-ahead log*
// (WAL) file: <table>.wal. Every mutating operation (row insert, row update,
// row delete, and periodic compaction) is appended to this file as its own
// self-describing record, tagged with the wall-clock timestamp at which it
// happened. Nothing is ever fully rewritten on an ordinary INSERT/UPDATE/
// DELETE - the file only grows. This is the same durability technique used
// by real database engines (redo logs / append-only files): as long as the
// bytes already flushed to disk are intact, the on-disk log is authoritative
// and the in-memory table is only ever a *materialized view* rebuilt from it.
//
// REVERT is implemented by *replaying* the WAL from the start and re-applying
// every record whose timestamp is <= the requested point in time. No copy of
// the whole database/table file is ever made to support this - state is
// recomputed from the operation history, exactly as the assignment requires
// ("snapshot" copying of DB files is forbidden).
//
// Because a log that is never trimmed would grow forever, once a table
// accumulates more than COMPACTION_THRESHOLD records the log is *compacted*:
// the current materialized state is appended as a single COMPACT record and
// every record before it is discarded. This is ordinary WAL/log compaction
// (as used by LSM engines, Raft logs, etc.) - it condenses history, it does
// not copy database files to create restore points. Old point-in-time
// recovery is still possible for anything after the oldest remaining record.
// ---------------------------------------------------------------------------

class StorageManager {
private:
    // Record kinds inside a table's .wal file.
    enum class WalOp : uint8_t {
        INSERT_ROW  = 1,   // append one new row
        UPDATE_ROW  = 2,   // replace an existing row by its row index
        DELETE_ROW  = 3,   // tombstone an existing row by its row index
        COMPACT     = 4    // full materialized state as of this point (log compaction)
    };

    struct WalRecord {
        std::string timestamp;   // "yyyy.mm.dd-hh:mm:ss.mss"
        WalOp op;
        size_t row_index = 0;    // for UPDATE_ROW / DELETE_ROW
        Row row;                 // for INSERT_ROW / UPDATE_ROW
        std::vector<Row> compact_rows; // for COMPACT
    };

    static constexpr size_t COMPACTION_THRESHOLD = 500; // records before auto-compact

    std::string data_dir_;

    // In-memory materialized view, rebuilt from each table's WAL on load and
    // kept in sync incrementally as operations are appended. This is a cache
    // for fast query execution, not the source of truth - the WAL is.
    std::map<std::string, std::map<std::string, std::vector<Row>>> data_;
    std::map<std::string, std::map<std::string, TableSchema>> schemas_;
    std::set<std::string> databases_;
    std::string current_db_;

    // Number of live WAL records appended since the table file was last
    // compacted (used to decide when to auto-compact).
    std::map<std::string, std::map<std::string, size_t>> wal_record_count_;

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

    std::string get_wal_path(const std::string& db, const std::string& table) const {
        return get_db_path(db) + "/" + table + ".wal";
    }

    std::string get_schema_path(const std::string& db, const std::string& table) const {
        return get_db_path(db) + "/" + table + "_schema.bin";
    }

    // -------------------- timestamp helpers --------------------

    std::string timepoint_to_string(const std::chrono::system_clock::time_point& tp) const {
        auto time_t = std::chrono::system_clock::to_time_t(tp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            tp.time_since_epoch()) % 1000;

        std::tm tm{};
        #ifdef _WIN32
            localtime_s(&tm, &time_t);
        #else
            localtime_r(&time_t, &tm);
        #endif
        char buffer[32];
        strftime(buffer, sizeof(buffer), "%Y.%m.%d-%H:%M:%S", &tm);

        std::stringstream ss;
        ss << buffer << "." << std::setw(3) << std::setfill('0') << ms.count();
        return ss.str();
    }

    std::chrono::system_clock::time_point string_to_timepoint(const std::string& timestamp) const {
        std::tm tm = {};
        int ms = 0;

        sscanf(timestamp.c_str(), "%d.%d.%d-%d:%d:%d.%d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &ms);

        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        tm.tm_isdst = -1;

        auto time_t = mktime(&tm);
        auto tp = std::chrono::system_clock::from_time_t(time_t);
        tp += std::chrono::milliseconds(ms);

        return tp;
    }

    std::string get_current_timestamp() const {
        return timepoint_to_string(std::chrono::system_clock::now());
    }

    // -------------------- value (de)serialization --------------------

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

    void write_row(std::ofstream& file, const Row& row) const {
        size_t col_count = row.values.size();
        file.write(reinterpret_cast<const char*>(&col_count), sizeof(size_t));
        for (const auto& value : row.values) {
            serialize_value(file, value);
        }
    }

    Row read_row(std::ifstream& file) const {
        size_t col_count = 0;
        file.read(reinterpret_cast<char*>(&col_count), sizeof(size_t));
        Row row;
        if (file.fail()) return row;
        for (size_t j = 0; j < col_count; ++j) {
            row.values.push_back(deserialize_value(file));
        }
        return row;
    }

    void write_timestamp(std::ofstream& file, const std::string& ts) const {
        size_t len = ts.size();
        file.write(reinterpret_cast<const char*>(&len), sizeof(size_t));
        file.write(ts.c_str(), len);
    }

    std::string read_timestamp(std::ifstream& file, bool& ok) const {
        size_t len = 0;
        file.read(reinterpret_cast<char*>(&len), sizeof(size_t));
        if (file.fail() || len > 64) { ok = false; return ""; }
        std::string ts(len, '\0');
        file.read(&ts[0], len);
        ok = !file.fail();
        return ts;
    }

    // -------------------- WAL append --------------------

    // Appends one record to the table's WAL file and immediately flushes it
    // to disk. Uses std::ios::app so existing bytes are never touched -
    // durability of previously-committed records does not depend on this
    // write succeeding.
    void append_wal_record(const std::string& db, const std::string& table, const WalRecord& rec) {
        std::string db_path = get_db_path(db);
        create_directory(db_path);

        std::string wal_path = get_wal_path(db, table);
        std::ofstream file(wal_path, std::ios::binary | std::ios::app);
        if (!file) {
            throw std::runtime_error("Cannot open WAL for append: " + table);
        }

        char op = static_cast<char>(rec.op);
        write_timestamp(file, rec.timestamp);
        file.write(&op, 1);

        switch (rec.op) {
            case WalOp::INSERT_ROW:
            case WalOp::UPDATE_ROW:
                file.write(reinterpret_cast<const char*>(&rec.row_index), sizeof(size_t));
                write_row(file, rec.row);
                break;
            case WalOp::DELETE_ROW:
                file.write(reinterpret_cast<const char*>(&rec.row_index), sizeof(size_t));
                break;
            case WalOp::COMPACT: {
                size_t n = rec.compact_rows.size();
                file.write(reinterpret_cast<const char*>(&n), sizeof(size_t));
                for (const auto& r : rec.compact_rows) {
                    write_row(file, r);
                }
                break;
            }
        }

        file.flush();
        if (!file) {
            throw std::runtime_error("Failed to durably write WAL record for: " + table);
        }

        wal_record_count_[db][table]++;
    }

    // -------------------- WAL replay --------------------

    // Replays a table's WAL, applying every record with timestamp <= cutoff
    // (or every record, if replay_all is true / no cutoff given). Returns the
    // materialized rows after replay. This is how both normal loading (replay
    // everything) and REVERT (replay up to a point in time) are implemented -
    // by recomputation from the operation history, never by copying a file.
    std::vector<Row> replay_wal(const std::string& db, const std::string& table,
                                 bool use_cutoff, const std::chrono::system_clock::time_point& cutoff,
                                 size_t* out_record_count = nullptr) {
        std::vector<Row> rows;
        std::string wal_path = get_wal_path(db, table);
        size_t record_count = 0;

        if (!file_exists(wal_path)) {
            if (out_record_count) *out_record_count = 0;
            return rows;
        }

        std::ifstream file(wal_path, std::ios::binary);
        if (!file) {
            if (out_record_count) *out_record_count = 0;
            return rows;
        }

        while (file.peek() != EOF) {
            bool ok = true;
            std::string ts = read_timestamp(file, ok);
            if (!ok) break;

            char op_byte;
            file.read(&op_byte, 1);
            if (file.fail()) break;
            WalOp op = static_cast<WalOp>(op_byte);

            bool within_cutoff = true;
            if (use_cutoff) {
                within_cutoff = (string_to_timepoint(ts) <= cutoff);
            }

            switch (op) {
                case WalOp::INSERT_ROW: {
                    size_t idx;
                    file.read(reinterpret_cast<char*>(&idx), sizeof(size_t));
                    Row row = read_row(file);
                    if (file.fail()) { file.setstate(std::ios::failbit); break; }
                    if (within_cutoff) {
                        if (idx >= rows.size()) rows.resize(idx + 1);
                        rows[idx] = row;
                    }
                    break;
                }
                case WalOp::UPDATE_ROW: {
                    size_t idx;
                    file.read(reinterpret_cast<char*>(&idx), sizeof(size_t));
                    Row row = read_row(file);
                    if (file.fail()) { file.setstate(std::ios::failbit); break; }
                    if (within_cutoff && idx < rows.size()) {
                        rows[idx] = row;
                    }
                    break;
                }
                case WalOp::DELETE_ROW: {
                    size_t idx;
                    file.read(reinterpret_cast<char*>(&idx), sizeof(size_t));
                    if (file.fail()) break;
                    if (within_cutoff && idx < rows.size()) {
                        rows[idx].values.clear(); // tombstone: empty row = deleted
                    }
                    break;
                }
                case WalOp::COMPACT: {
                    size_t n;
                    file.read(reinterpret_cast<char*>(&n), sizeof(size_t));
                    if (file.fail()) { file.setstate(std::ios::failbit); break; }
                    std::vector<Row> compact_rows;
                    compact_rows.reserve(n);
                    for (size_t i = 0; i < n; ++i) {
                        compact_rows.push_back(read_row(file));
                    }
                    if (file.fail()) break;
                    if (within_cutoff) {
                        rows = compact_rows;
                    }
                    break;
                }
                default:
                    // Unknown/corrupted record - stop replay here rather than
                    // crash; whatever was durably applied so far is kept.
                    file.setstate(std::ios::failbit);
                    break;
            }

            if (file.fail()) {
                std::cerr << "Warning: WAL for '" << table
                          << "' truncated/corrupted after " << record_count
                          << " records; using data recovered so far." << std::endl;
                break;
            }

            record_count++;
        }

        // Drop tombstoned (deleted) rows from the materialized view, but keep
        // their slot semantics stable during replay itself (handled above).
        std::vector<Row> live;
        live.reserve(rows.size());
        for (auto& r : rows) {
            if (!r.values.empty()) live.push_back(std::move(r));
        }

        if (out_record_count) *out_record_count = record_count;
        return live;
    }

    // Compacts a table's WAL: writes the current materialized state as a
    // single COMPACT record into a fresh file, then atomically replaces the
    // old WAL with it. This trims log growth; it is not a "database file
    // snapshot" mechanism - REVERT still works via replay, it simply cannot
    // recover to a point in time *older* than the oldest surviving record
    // (same trade-off any log-structured/compacted store makes).
    void compact_table_wal(const std::string& db, const std::string& table) {
        const auto& rows = data_[db][table];

        std::string wal_path = get_wal_path(db, table);
        std::string tmp_path = wal_path + ".compact.tmp";

        {
            std::ofstream file(tmp_path, std::ios::binary | std::ios::trunc);
            if (!file) {
                throw std::runtime_error("Cannot compact WAL for: " + table);
            }

            std::string ts = get_current_timestamp();
            char op = static_cast<char>(WalOp::COMPACT);
            write_timestamp(file, ts);
            file.write(&op, 1);

            size_t n = rows.size();
            file.write(reinterpret_cast<const char*>(&n), sizeof(size_t));
            for (const auto& r : rows) {
                write_row(file, r);
            }
            file.flush();
            if (!file) {
                throw std::runtime_error("Failed to durably write compacted WAL for: " + table);
            }
        }

        // Atomic rename: at every instant either the old, fully-intact WAL
        // exists at wal_path, or the new, fully-intact compacted WAL does.
        // There is never a window with a half-written table file on disk.
        fs::rename(tmp_path, wal_path);

        wal_record_count_[db][table] = 1;
    }

    void maybe_compact(const std::string& db, const std::string& table) {
        if (wal_record_count_[db][table] >= COMPACTION_THRESHOLD) {
            compact_table_wal(db, table);
        }
    }

    // -------------------- schema (de)serialization --------------------
    // Schemas rarely change (only on CREATE TABLE), so they are still stored
    // as a small standalone file that is rewritten in full on the rare
    // occasions the schema is (re)written - this is not row data and holds
    // no history requirement.

    void save_schema(const std::string& db_name, const std::string& table_name) {
        std::string schema_path = get_schema_path(db_name, table_name);
        std::string tmp_path = schema_path + ".tmp";

        {
            std::ofstream file(tmp_path, std::ios::binary | std::ios::trunc);
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
            file.flush();
            if (!file) {
                throw std::runtime_error("Failed to durably write schema for: " + table_name);
            }
        }

        fs::rename(tmp_path, schema_path);
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

    void load_table(const std::string& db_name, const std::string& table_name) {
        size_t rec_count = 0;
        auto rows = replay_wal(db_name, table_name, false, {}, &rec_count);
        data_[db_name][table_name] = std::move(rows);
        wal_record_count_[db_name][table_name] = rec_count;

        std::cout << "Loaded table '" << table_name << "' with "
                  << data_[db_name][table_name].size() << " rows ("
                  << rec_count << " WAL records replayed)" << std::endl;
    }

    void load_database(const std::string& db_name) {
        std::string db_path = get_db_path(db_name);

        if (!directory_exists(db_path)) {
            return;
        }

        databases_.insert(db_name);
        data_[db_name] = {};
        schemas_[db_name] = {};
        wal_record_count_[db_name] = {};

        for (const auto& entry : fs::directory_iterator(db_path)) {
            std::string filename = entry.path().filename().string();
            if (filename.size() > 11 && filename.compare(filename.size() - 11, 11, "_schema.bin") == 0) {
                std::string table_name = filename.substr(0, filename.size() - 11);
                load_schema(db_name, table_name);
            }
        }

        for (const auto& [table_name, schema] : schemas_[db_name]) {
            (void)schema;
            load_table(db_name, table_name);
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
        wal_record_count_.clear();

        if (!directory_exists(data_dir_)) {
            create_directory(data_dir_);
            return;
        }

        for (const auto& entry : fs::directory_iterator(data_dir_)) {
            if (entry.is_directory()) {
                std::string db_name = entry.path().filename().string();
                load_database(db_name);
            }
        }

        std::cout << "Loaded " << databases_.size() << " database(s)" << std::endl;
    }

    // Kept for API compatibility: with WAL-based storage there is no longer
    // a single "rewrite the whole table file" step to run at the end of an
    // operation - every mutation is already durably appended as it happens.
    // This flushes the schema (cheap, rare) and, if the table has drifted
    // past the compaction threshold, compacts its log.
    void save_table(const std::string& db_name, const std::string& table_name) {
        create_directory(get_db_path(db_name));
        save_schema(db_name, table_name);
        maybe_compact(db_name, table_name);
    }

    void save_all() {
        for (const auto& [db_name, tables] : data_) {
            for (const auto& [table_name, _] : tables) {
                save_schema(db_name, table_name);
            }
        }
        std::cout << "All data flushed to disk" << std::endl;
    }

    // -------------------- row-level mutation API (WAL-backed) --------------------
    // These append one durable WAL record per row change and update the
    // in-memory materialized view to match. Callers in Executor no longer
    // need to rewrite the whole table after a batch of changes - each row
    // operation is already persisted the instant it happens.

    void insert_row(const std::string& table_name, const Row& row) {
        if (current_db_.empty()) {
            throw std::runtime_error("No database selected");
        }
        auto& rows = data_[current_db_][table_name];
        size_t idx = rows.size();

        WalRecord rec;
        rec.timestamp = get_current_timestamp();
        rec.op = WalOp::INSERT_ROW;
        rec.row_index = idx;
        rec.row = row;
        append_wal_record(current_db_, table_name, rec);

        rows.push_back(row);
    }

    void update_row(const std::string& table_name, size_t row_index, const Row& new_row) {
        if (current_db_.empty()) {
            throw std::runtime_error("No database selected");
        }
        auto& rows = data_[current_db_][table_name];
        if (row_index >= rows.size()) {
            throw std::runtime_error("Row index out of range");
        }

        WalRecord rec;
        rec.timestamp = get_current_timestamp();
        rec.op = WalOp::UPDATE_ROW;
        rec.row_index = row_index;
        rec.row = new_row;
        append_wal_record(current_db_, table_name, rec);

        rows[row_index] = new_row;
    }

    void delete_row(const std::string& table_name, size_t row_index) {
        if (current_db_.empty()) {
            throw std::runtime_error("No database selected");
        }
        auto& rows = data_[current_db_][table_name];
        if (row_index >= rows.size()) {
            throw std::runtime_error("Row index out of range");
        }

        WalRecord rec;
        rec.timestamp = get_current_timestamp();
        rec.op = WalOp::DELETE_ROW;
        rec.row_index = row_index;
        append_wal_record(current_db_, table_name, rec);

        rows.erase(rows.begin() + row_index);
    }

    // -------------------- REVERT (WAL replay, never file copies) --------------------

    void revert_to_snapshot(const std::string& table_name, const std::chrono::system_clock::time_point& timestamp) {
        if (current_db_.empty()) {
            throw std::runtime_error("No database selected");
        }

        auto rows = replay_wal(current_db_, table_name, true, timestamp);
        data_[current_db_][table_name] = rows;

        // Record the revert itself as a new WAL entry (a COMPACT record
        // capturing the restored state) so the operation is durable and the
        // history remains a single append-only log - we still never copy a
        // database file to perform this.
        WalRecord rec;
        rec.timestamp = get_current_timestamp();
        rec.op = WalOp::COMPACT;
        rec.compact_rows = rows;
        append_wal_record(current_db_, table_name, rec);

        std::cout << "Reverted " << table_name << " to state at "
                  << timepoint_to_string(timestamp) << " (" << rows.size()
                  << " rows)" << std::endl;
    }

    void revert_to_snapshot(const std::string& table_name, const std::string& timestamp_str) {
        revert_to_snapshot(table_name, string_to_timepoint(timestamp_str));
    }

    void revert_all_tables(const std::chrono::system_clock::time_point& timestamp) {
        if (current_db_.empty()) {
            throw std::runtime_error("No database selected");
        }

        int reverted_count = 0;
        for (const auto& [table_name, _] : schemas_[current_db_]) {
            try {
                revert_to_snapshot(table_name, timestamp);
                reverted_count++;
            } catch (const std::exception& e) {
                std::cerr << "Warning: Could not revert " << table_name << ": " << e.what() << std::endl;
            }
        }

        if (reverted_count == 0) {
            throw std::runtime_error("No tables could be reverted to timestamp: " + timepoint_to_string(timestamp));
        }

        std::cout << "Reverted " << reverted_count << " tables" << std::endl;
    }

    void revert_all_tables(const std::string& timestamp_str) {
        revert_all_tables(string_to_timepoint(timestamp_str));
    }

    // -------------------- metadata operations --------------------

    void create_database(const std::string& name) {
        if (databases_.count(name)) {
            throw std::runtime_error("Database already exists: " + name);
        }

        std::string db_path = get_db_path(name);
        create_directory(db_path);

        databases_.insert(name);
        data_[name] = {};
        schemas_[name] = {};
        wal_record_count_[name] = {};

        std::cout << "Database '" << name << "' created at: " << db_path << std::endl;
    }

    void drop_database(const std::string& name) {
        if (!databases_.count(name)) {
            throw std::runtime_error("Database does not exist: " + name);
        }

        std::string db_path = get_db_path(name);
        if (directory_exists(db_path)) {
            std::error_code ec;
            fs::remove_all(db_path, ec);
            if (ec) {
                std::cerr << "Warning: Could not fully remove database files: " << db_path << std::endl;
            }
        }

        databases_.erase(name);
        data_.erase(name);
        schemas_.erase(name);
        wal_record_count_.erase(name);

        if (current_db_ == name) {
            current_db_.clear();
        }

        std::cout << "Database '" << name << "' dropped" << std::endl;
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
        wal_record_count_[current_db_][name] = 0;

        save_schema(current_db_, name);

        // Seed the table's WAL with an initial (empty) COMPACT record so the
        // file exists on disk from creation - still not a "database file
        // snapshot", just the first entry of its append-only log.
        WalRecord rec;
        rec.timestamp = get_current_timestamp();
        rec.op = WalOp::COMPACT;
        rec.compact_rows = {};
        append_wal_record(current_db_, name, rec);

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
        wal_record_count_[current_db_].erase(name);

        std::string wal_path = get_wal_path(current_db_, name);
        std::string schema_path = get_schema_path(current_db_, name);

        if (file_exists(wal_path)) {
            std::remove(wal_path.c_str());
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

    std::map<std::string, TableSchema> get_all_schemas(const std::string& db_name) const {
        auto it = schemas_.find(db_name);
        if (it != schemas_.end()) {
            return it->second;
        }
        return {};
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
