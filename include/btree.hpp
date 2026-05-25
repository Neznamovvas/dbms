#ifndef DBMS_BTREE_HPP
#define DBMS_BTREE_HPP

#include <vector>
#include <memory>
#include <optional>
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace dbms {


template<typename KeyType>
struct BTreeNode {
    std::vector<KeyType> keys;
    std::vector<size_t> values; 
    std::vector<std::shared_ptr<BTreeNode<KeyType>>> children;
    bool is_leaf = true;
    std::weak_ptr<BTreeNode<KeyType>> parent;
    
    BTreeNode(bool leaf = true) : is_leaf(leaf) {}
    
    
    size_t find_position(const KeyType& key) const {
        return std::lower_bound(keys.begin(), keys.end(), key) - keys.begin();
    }
};


template<typename KeyType>
class BTreeIndex {
private:
    static constexpr size_t ORDER = 4; 
    static constexpr size_t MIN_KEYS = ORDER - 1;
    static constexpr size_t MAX_KEYS = 2 * ORDER - 1;
    
    std::shared_ptr<BTreeNode<KeyType>> root_;
    size_t size_ = 0;
    
    
    void split_child(std::shared_ptr<BTreeNode<KeyType>> parent, size_t child_index) {
        auto child = parent->children[child_index];
        auto new_node = std::make_shared<BTreeNode<KeyType>>(child->is_leaf);
        
        
        size_t mid = MIN_KEYS;
        
        
        new_node->keys.assign(child->keys.begin() + mid + 1, child->keys.end());
        child->keys.resize(mid);
        
        
        new_node->values.assign(child->values.begin() + mid + 1, child->values.end());
        child->values.resize(mid + 1);
        
        
        if (!child->is_leaf) {
            new_node->children.assign(child->children.begin() + mid + 1, child->children.end());
            child->children.resize(mid + 1);
            
            
            for (auto& c : new_node->children) {
                if (c) c->parent = new_node;
            }
        }
        
        
        parent->keys.insert(parent->keys.begin() + child_index, child->keys[mid]);
        parent->values.insert(parent->values.begin() + child_index, child->values[mid]);
        parent->children.insert(parent->children.begin() + child_index + 1, new_node);
        
        new_node->parent = parent;
        
        
        child->keys.pop_back();
        child->values.pop_back();
    }
    
    
    void insert_non_full(std::shared_ptr<BTreeNode<KeyType>> node, const KeyType& key, size_t record_id) {
        size_t pos = node->find_position(key);
        
        if (node->is_leaf) {
            node->keys.insert(node->keys.begin() + pos, key);
            node->values.insert(node->values.begin() + pos, record_id);
            size_++;
        } else {
            if (node->children[pos]->keys.size() == MAX_KEYS) {
                split_child(node, pos);
                if (key > node->keys[pos]) {
                    pos++;
                }
            }
            insert_non_full(node->children[pos], key, record_id);
        }
    }
    
    
    std::optional<size_t> search_node(const std::shared_ptr<BTreeNode<KeyType>>& node, const KeyType& key) const {
        size_t pos = node->find_position(key);
        
        if (pos < node->keys.size() && node->keys[pos] == key) {
            return node->values[pos];
        }
        
        if (node->is_leaf) {
            return std::nullopt;
        }
        
        return search_node(node->children[pos], key);
    }
    
    
    void range_search_node(const std::shared_ptr<BTreeNode<KeyType>>& node,
                          const KeyType& from, const KeyType& to,
                          std::vector<size_t>& results) const {
        size_t pos = node->find_position(from);
        
        if (node->is_leaf) {
            for (size_t i = pos; i < node->keys.size(); ++i) {
                if (node->keys[i] >= from && node->keys[i] < to) {
                    results.push_back(node->values[i]);
                } else if (node->keys[i] >= to) {
                    break;
                }
            }
        } else {
            
            for (size_t i = pos; i <= node->keys.size(); ++i) {
                range_search_node(node->children[i], from, to, results);
                if (i < node->keys.size() && node->keys[i] < to) {
                    results.push_back(node->values[i]);
                }
            }
        }
    }
    
public:
    BTreeIndex() {
        root_ = std::make_shared<BTreeNode<KeyType>>(true);
    }
    
    void insert(const KeyType& key, size_t record_id) {
        if (root_->keys.size() == MAX_KEYS) {
            auto new_root = std::make_shared<BTreeNode<KeyType>>(false);
            new_root->children.push_back(root_);
            root_->parent = new_root;
            split_child(new_root, 0);
            root_ = new_root;
        }
        insert_non_full(root_, key, record_id);
    }
    
    std::optional<size_t> find(const KeyType& key) const {
        return search_node(root_, key);
    }
    
    std::vector<size_t> range_find(const KeyType& from, const KeyType& to) const {
        std::vector<size_t> results;
        range_search_node(root_, from, to, results);
        return results;
    }
    
    size_t size() const { return size_; }
    
    
    void save(const std::string& filename) const {
        std::ofstream file(filename, std::ios::binary);
        if (!file) {
            throw std::runtime_error("Cannot save index to " + filename);
        }
           
        file.write(reinterpret_cast<const char*>(&size_), sizeof(size_));
        
        
    }
    
    
    void load(const std::string& filename) {
        std::ifstream file(filename, std::ios::binary);
        if (!file) {
            return; 
        }
        
        file.read(reinterpret_cast<char*>(&size_), sizeof(size_));
        
    }
};

} 

#endif