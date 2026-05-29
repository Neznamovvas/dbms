#ifndef DBMS_LOGGER_HPP
#define DBMS_LOGGER_HPP

#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <mutex>
#include <thread>
#include <map>
#include <random>
#include <atomic>
// #include <nlohmann/json.hpp>
#include "../include/json.hpp"

using json = nlohmann::json;

namespace dbms {


enum class LogLevel {
    L_DEBUG,
    L_INFO,
    L_WARNING,
    L_ERROR
};


struct LogEntry {
    std::string query;           
    std::string client_id;       
    std::string handler_id;      
    std::string start_time;      
    std::string end_time;        
    long long duration_ms;       
    int status_code;             
    std::string error_message;   
    
    
    std::string to_csv() const {
        std::stringstream ss;
        ss << "\"" << escape_csv(query) << "\","
           << "\"" << escape_csv(client_id) << "\","
           << "\"" << escape_csv(handler_id) << "\","
           << "\"" << start_time << "\","
           << "\"" << end_time << "\","
           << duration_ms << ","
           << status_code << ","
           << "\"" << escape_csv(error_message) << "\"";
        return ss.str();
    }
    
    
    std::string to_json() const {
        json j;
        j["query"] = query;
        j["client_id"] = client_id;
        j["handler_id"] = handler_id;
        j["start_time"] = start_time;
        j["end_time"] = end_time;
        j["duration_ms"] = duration_ms;
        j["status_code"] = status_code;
        j["error_message"] = error_message;
        
        return j.dump();
    }
    
private:
    std::string escape_csv(const std::string& str) const {
        std::string result = str;
        size_t pos = 0;
        while ((pos = result.find('"', pos)) != std::string::npos) {
            result.replace(pos, 1, "\"\"");
            pos += 2;
        }
        return result;
    }
};


class IDGenerator {
private:
    static std::atomic<int> next_id_;
    static std::mt19937 rng_;
    
public:
    static std::string generate_client_id() {
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        
        std::stringstream ss;
        ss << "client_" << timestamp << "_" << (next_id_++);
        return ss.str();
    }
    
    static std::string generate_handler_id() {
        std::stringstream ss;
        ss << "handler_" << std::this_thread::get_id();
        return ss.str();
    }
};

std::atomic<int> IDGenerator::next_id_(0);
std::mt19937 IDGenerator::rng_(std::chrono::steady_clock::now().time_since_epoch().count());


class AccessLogger {
private:
    std::string log_file_;
    std::ofstream file_;
    mutable std::mutex mutex_;
    bool enabled_;
    LogLevel min_level_;
    
    
    std::map<int, int> status_counts_;
    long long total_queries_ = 0;
    long long total_duration_ms_ = 0;
    
    
    static std::string format_time(const std::chrono::system_clock::time_point& time, 
                                   const std::string& format = "%Y-%m-%d %H:%M:%S") {
        auto time_t = std::chrono::system_clock::to_time_t(time);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            time.time_since_epoch()) % 1000;
        
        std::tm tm;
        localtime_s(&tm, &time_t);
        
        std::stringstream ss;
        ss << std::put_time(&tm, format.c_str());
        ss << "." << std::setfill('0') << std::setw(3) << ms.count();
        
        return ss.str();
    }
    
public:
    AccessLogger(const std::string& log_file = "access.log", bool enabled = true, 
                 LogLevel min_level = LogLevel::L_INFO)
        : log_file_(log_file), enabled_(enabled), min_level_(min_level) {
        
        if (enabled_) {
            open_log_file();
            write_header();
        }
    }
    
    ~AccessLogger() {
        if (file_.is_open()) {
            write_footer();
            file_.close();
        }
    }
    
    void open_log_file() {
        file_.open(log_file_, std::ios::app);
        if (!file_.is_open()) {
            std::cerr << "Warning: Cannot open log file: " << log_file_ << std::endl;
            enabled_ = false;
        }
    }
    
    void write_header() {
        if (!enabled_ || !file_.is_open()) return;
        
        std::lock_guard<std::mutex> lock(mutex_);
        file_ << "# ========================================\n";
        file_ << "# DBMS Access Log\n";
        file_ << "# Format: CSV\n";
        file_ << "# Columns: query, client_id, handler_id, start_time, end_time, duration_ms, status_code, error_message\n";
        file_ << "# ========================================\n";
        file_ << "\"QUERY\",\"CLIENT_ID\",\"HANDLER_ID\",\"START_TIME\",\"END_TIME\",\"DURATION_MS\",\"STATUS_CODE\",\"ERROR_MESSAGE\"\n";
        file_.flush();
    }
    
    void write_footer() {
        if (!enabled_ || !file_.is_open()) return;
        
        std::lock_guard<std::mutex> lock(mutex_);
        file_ << "# ========================================\n";
        file_ << "# Statistics:\n";
        file_ << "# Total queries: " << total_queries_ << "\n";
        file_ << "# Average duration: " << (total_queries_ > 0 ? total_duration_ms_ / total_queries_ : 0) << " ms\n";
        file_ << "# Status codes:\n";
        for (const auto& [code, count] : status_counts_) {
            file_ << "#   " << code << ": " << count << "\n";
        }
        file_ << "# ========================================\n";
        file_.flush();
    }
    
    void log(const LogEntry& entry) {
        if (!enabled_) return;
        
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (file_.is_open()) {
            file_ << entry.to_csv() << "\n";
            file_.flush();
        }
        
        
        total_queries_++;
        total_duration_ms_ += entry.duration_ms;
        status_counts_[entry.status_code]++;
        
        
        std::cout << "[LOG] " << entry.to_json() << std::endl;
    }
    
    void log_sync(const std::string& query, const std::string& client_id,
                  const std::string& handler_id, int status_code,
                  const std::string& error_message = "") {
        
        LogEntry entry;
        entry.query = query;
        entry.client_id = client_id;
        entry.handler_id = handler_id;
        entry.status_code = status_code;
        entry.error_message = error_message;
        
        auto now = std::chrono::system_clock::now();
        entry.start_time = format_time(now);
        entry.end_time = format_time(now);
        entry.duration_ms = 0;
        
        log(entry);
    }
    
    
    class QueryLogger {
    private:
        AccessLogger* parent_;
        LogEntry entry_;
        std::chrono::system_clock::time_point start_time_;
        bool finished_ = false;

    public:
        QueryLogger(AccessLogger* parent, const std::string& query, 
                    const std::string& client_id, const std::string& handler_id)
            : parent_(parent) {
            entry_.query = query;
            entry_.client_id = client_id;
            entry_.handler_id = handler_id;
            start_time_ = std::chrono::system_clock::now();
            entry_.start_time = AccessLogger::format_time(start_time_);
        }
        
        ~QueryLogger() {
            if (!finished_) {  
                finish(500);
            }
        }
        
        void finish(int status_code, const std::string& error_message = "") {
            if (finished_) return;  
            finished_ = true; 
            auto end_time = std::chrono::system_clock::now();
            entry_.end_time = AccessLogger::format_time(end_time);
            entry_.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                end_time - start_time_).count();
            entry_.status_code = status_code;
            entry_.error_message = error_message;
            
            if (parent_) {
                parent_->log(entry_);
            }
        }
        
        
        void success() {
            finish(200);
        }
        
        
        void error(const std::string& message, int code = 400) {
            finish(code, message);
        }
    };
    
    
    std::string get_stats() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        json j;
        j["total_queries"] = total_queries_;
        j["avg_duration_ms"] = (total_queries_ > 0 ? total_duration_ms_ / total_queries_ : 0);
        
        json status_counts_json;
        for (const auto& [code, count] : status_counts_) {
            status_counts_json[std::to_string(code)] = count;
        }
        j["status_counts"] = status_counts_json;
        
        return j.dump();
    }
    
    
    void rotate() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (file_.is_open()) {
            file_.close();
        }
        
        auto now = std::chrono::system_clock::now();
        std::string new_name = log_file_ + "." + format_time(now, "%Y%m%d_%H%M%S");
        
        std::rename(log_file_.c_str(), new_name.c_str());
        
        open_log_file();
        write_header();
    }
};

} 

#endif