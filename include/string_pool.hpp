#ifndef DBMS_STRING_POOL_HPP
#define DBMS_STRING_POOL_HPP

#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>

namespace dbms {


class StringPool {
private:
    std::unordered_map<std::string, std::shared_ptr<const std::string>> pool_;
    mutable std::mutex mutex_;
    size_t total_bytes_saved_ = 0;
    size_t total_strings_stored_ = 0;
    
public:
    
    std::shared_ptr<const std::string> intern(const std::string& str) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = pool_.find(str);
        if (it != pool_.end()) {
            return it->second;
        }
        
        
        auto interned = std::make_shared<const std::string>(str);
        pool_[str] = interned;
        
        total_strings_stored_++;
        total_bytes_saved_ += str.size();
        
        return interned;
    }
    
    
    std::shared_ptr<const std::string> intern(std::string&& str) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = pool_.find(str);
        if (it != pool_.end()) {
            return it->second;
        }
        
        auto interned = std::make_shared<const std::string>(std::move(str));
        pool_[*interned] = interned;
        
        total_strings_stored_++;
        total_bytes_saved_ += interned->size();
        
        return interned;
    }
    
    
    void get_stats(size_t& unique_strings, size_t& bytes_saved, size_t& total_refs) const {
        std::lock_guard<std::mutex> lock(mutex_);
        unique_strings = pool_.size();
        bytes_saved = total_bytes_saved_;
        total_refs = total_strings_stored_;
    }
    
    
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        pool_.clear();
        total_bytes_saved_ = 0;
        total_strings_stored_ = 0;
    }
    
    
    size_t memory_usage() const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t total = 0;
        for (const auto& [key, ptr] : pool_) {
            total += key.size() + sizeof(ptr);
        }
        return total;
    }
};


inline StringPool& get_string_pool() {
    static StringPool pool;
    return pool;
}


using StringRef = std::shared_ptr<const std::string>;


inline StringRef make_string_ref(const std::string& str) {
    return get_string_pool().intern(str);
}

inline StringRef make_string_ref(std::string&& str) {
    return get_string_pool().intern(std::move(str));
}

} 

#endif