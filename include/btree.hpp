#ifndef DBMS_BTREE_HPP
#define DBMS_BTREE_HPP

#include "index_file.hpp"
#include "string_pool.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace dbms {

struct IndexKey {
    StringRef ref;

    IndexKey() : ref(std::make_shared<const std::string>("")) {}
    IndexKey(const std::string& s) : ref(make_string_ref(s)) {}
    IndexKey(StringRef r) : ref(std::move(r)) {}

    const std::string& str() const { return *ref; }

    bool operator<(const IndexKey& o) const  { return str() <  o.str(); }
    bool operator>(const IndexKey& o) const  { return str() >  o.str(); }
    bool operator==(const IndexKey& o) const { return str() == o.str(); }
    bool operator!=(const IndexKey& o) const { return str() != o.str(); }
    bool operator<=(const IndexKey& o) const { return str() <= o.str(); }
    bool operator>=(const IndexKey& o) const { return str() >= o.str(); }
};


template<typename KeyType>
struct KeyCodec;

template<>
struct KeyCodec<int> {
    static void write(std::fstream& f, int v) {
        f.write(reinterpret_cast<const char*>(&v), sizeof(int));
    }
    static int read(std::fstream& f) {
        int v = 0;
        f.read(reinterpret_cast<char*>(&v), sizeof(int));
        return v;
    }
    static size_t serialized_size(const int&) { return sizeof(int); }
};

template<>
struct KeyCodec<IndexKey> {
    static void write(std::fstream& f, const IndexKey& k) {
        const std::string& s = k.str();
        uint32_t len = static_cast<uint32_t>(s.size());
        f.write(reinterpret_cast<const char*>(&len), sizeof(len));
        if (len) f.write(s.data(), len);
    }
    static IndexKey read(std::fstream& f) {
        uint32_t len = 0;
        f.read(reinterpret_cast<char*>(&len), sizeof(len));
        if (len > 1000000) {
            throw std::runtime_error("KeyCodec<IndexKey>: string too long");
        }
        std::string s(len, '\0');
        if (len) f.read(&s[0], len);
        return IndexKey(make_string_ref(s));
    }
    static size_t serialized_size(const IndexKey& k) {
        return sizeof(uint32_t) + k.str().size();
    }
};




template<typename KeyType>
struct DiskNode {
    bool                  is_leaf = true;
    uint64_t              parent  = 0;
    std::vector<KeyType>  keys;
    std::vector<uint64_t> values;
    std::vector<uint64_t> children;
    uint64_t              next = 0;
    uint64_t              prev = 0;

    uint64_t write_at(std::fstream& f, uint64_t offset) const {
        f.clear();
        f.seekp(static_cast<std::streamoff>(offset));

        uint8_t  leaf_flag = is_leaf ? 1 : 0;
        uint32_t nk        = static_cast<uint32_t>(keys.size());

        f.write(reinterpret_cast<const char*>(&leaf_flag), 1);
        f.write(reinterpret_cast<const char*>(&nk), sizeof(nk));
        f.write(reinterpret_cast<const char*>(&parent), sizeof(parent));

        for (const auto& k : keys) {
            KeyCodec<KeyType>::write(f, k);
        }

        if (is_leaf) {
            for (uint64_t v : values) {
                f.write(reinterpret_cast<const char*>(&v), sizeof(v));
            }
            f.write(reinterpret_cast<const char*>(&next), sizeof(next));
            f.write(reinterpret_cast<const char*>(&prev), sizeof(prev));
        } else {
            for (uint64_t c : children) {
                f.write(reinterpret_cast<const char*>(&c), sizeof(c));
            }
        }

        return offset;
    }

    static DiskNode read_at(std::fstream& f, uint64_t offset) {
        DiskNode n;
        f.clear();
        f.seekg(static_cast<std::streamoff>(offset));

        uint8_t  leaf_flag = 0;
        uint32_t nk        = 0;

        f.read(reinterpret_cast<char*>(&leaf_flag), 1);
        f.read(reinterpret_cast<char*>(&nk), sizeof(nk));
        f.read(reinterpret_cast<char*>(&n.parent), sizeof(n.parent));
        n.is_leaf = (leaf_flag != 0);

        if (nk > 1000) {
            throw std::runtime_error("DiskNode corrupted: too many keys");
        }

        n.keys.reserve(nk);
        for (uint32_t i = 0; i < nk; ++i) {
            n.keys.push_back(KeyCodec<KeyType>::read(f));
        }

        if (n.is_leaf) {
            n.values.reserve(nk);
            for (uint32_t i = 0; i < nk; ++i) {
                uint64_t v = 0;
                f.read(reinterpret_cast<char*>(&v), sizeof(v));
                n.values.push_back(v);
            }
            f.read(reinterpret_cast<char*>(&n.next), sizeof(n.next));
            f.read(reinterpret_cast<char*>(&n.prev), sizeof(n.prev));
        } else {
            n.children.reserve(nk + 1);
            for (uint32_t i = 0; i <= nk; ++i) {
                uint64_t c = 0;
                f.read(reinterpret_cast<char*>(&c), sizeof(c));
                n.children.push_back(c);
            }
        }

        return n;
    }

    uint64_t serialized_size() const {
        uint64_t sz = 1 + 4 + 8;
        for (const auto& k : keys) {
            sz += KeyCodec<KeyType>::serialized_size(k);
        }
        if (is_leaf) {
            sz += values.size() * sizeof(uint64_t);
            sz += sizeof(uint64_t) * 2;
        } else {
            sz += children.size() * sizeof(uint64_t);
        }
        return sz;
    }
};




template<typename KeyType>
class DiskBPlusTree {
public:
    static constexpr uint32_t ORDER    = 4;
    static constexpr uint32_t MAX_KEYS = 2 * ORDER - 1;
    static constexpr uint32_t MIN_KEYS = ORDER - 1;

    DiskBPlusTree(std::fstream& file, uint64_t tree_header_offset)
        : file_(file), header_offset_(tree_header_offset) {
        header_ = TreeHeader::read(file_, header_offset_);
        if (header_.order != ORDER) {
            throw std::runtime_error("ORDER mismatch in B+tree header");
        }
    }

    static uint64_t create(std::fstream& file, uint64_t header_off, uint8_t key_type) {
        TreeHeader h;
        h.key_type = key_type;
        h.order    = ORDER;
        h.max_keys = MAX_KEYS;
        h.min_keys = MIN_KEYS;

        uint64_t root_off = header_off + TreeHeader::SIZE;

        DiskNode<KeyType> root;
        root.is_leaf = true;
        root.parent  = 0;
        root.write_at(file, root_off);

        h.root_offset      = root_off;
        h.head_leaf_offset = root_off;
        h.tail_leaf_offset = root_off;
        h.node_count       = 1;
        h.key_count        = 0;
        h.free_offset      = root_off + root.serialized_size();

        h.write_to(file, header_off);

        file.flush();
        file.clear();
        return header_off;
    }



    void insert(const KeyType& key, uint64_t record_id) {
        uint64_t leaf_off = find_leaf_offset(key);
        DiskNode<KeyType> leaf = DiskNode<KeyType>::read_at(file_, leaf_off);

        auto it  = std::lower_bound(leaf.keys.begin(), leaf.keys.end(), key);
        size_t p = std::distance(leaf.keys.begin(), it);

        if (it != leaf.keys.end() && *it == key) {
            throw std::runtime_error("UNIQUE constraint violated in index");
        }

        leaf.keys.insert(it, key);
        leaf.values.insert(leaf.values.begin() + p, record_id);
        header_.key_count++;

        if (leaf.keys.size() > MAX_KEYS) {
            split_leaf(leaf_off, leaf);
        } else {
            leaf.write_at(file_, leaf_off);

            uint64_t end = leaf_off + leaf.serialized_size();
            if (end > header_.free_offset) {
                header_.free_offset = end;
            }
        }

        save_header();
    }

    std::optional<uint64_t> find(const KeyType& key) const {
        uint64_t leaf_off = find_leaf_offset(key);
        DiskNode<KeyType> leaf = DiskNode<KeyType>::read_at(file_, leaf_off);

        auto it = std::lower_bound(leaf.keys.begin(), leaf.keys.end(), key);
        if (it != leaf.keys.end() && *it == key) {
            size_t p = std::distance(leaf.keys.begin(), it);
            return leaf.values[p];
        }
        return std::nullopt;
    }

    std::vector<uint64_t> range_find(const KeyType& from, const KeyType& to) const {
        std::vector<uint64_t> out;
        uint64_t leaf_off = find_leaf_offset(from);

        while (leaf_off != 0) {
            DiskNode<KeyType> leaf = DiskNode<KeyType>::read_at(file_, leaf_off);
            for (size_t i = 0; i < leaf.keys.size(); ++i) {
                if (leaf.keys[i] >= to) return out;
                if (leaf.keys[i] >= from) out.push_back(leaf.values[i]);
            }
            if (leaf.next == 0) break;
            leaf_off = leaf.next;
        }
        return out;
    }

    uint64_t key_count()  const { return header_.key_count; }
    uint64_t node_count() const { return header_.node_count; }
    uint64_t free_offset() const { return header_.free_offset; }

    void flush() {
        save_header();
        file_.flush();
    }

private:
    std::fstream& file_;
    uint64_t      header_offset_;
    TreeHeader    header_;

    void save_header() {
        header_.write_to(file_, header_offset_);
    }

    uint64_t find_leaf_offset(const KeyType& key) const {
        uint64_t off = header_.root_offset;
        while (true) {
            DiskNode<KeyType> node = DiskNode<KeyType>::read_at(file_, off);
            if (node.is_leaf) return off;

            size_t i = 0;
            while (i < node.keys.size() && key >= node.keys[i]) ++i;
            off = node.children[i];
        }
    }

    uint64_t allocate_node() {
        uint64_t off = header_.free_offset;
        header_.node_count++;
        return off;
    }

    void split_leaf(uint64_t leaf_off, DiskNode<KeyType>& leaf) {
        size_t mid = leaf.keys.size() / 2;

        DiskNode<KeyType> right;
        right.is_leaf = true;
        right.parent  = leaf.parent;
        right.keys.assign(leaf.keys.begin() + mid, leaf.keys.end());
        right.values.assign(leaf.values.begin() + mid, leaf.values.end());

        leaf.keys.resize(mid);
        leaf.values.resize(mid);

        uint64_t right_off = allocate_node();
        right.write_at(file_, right_off);


        {
            uint64_t end = right_off + right.serialized_size();
            if (end > header_.free_offset) header_.free_offset = end;
        }

        right.prev = leaf_off;
        right.next = leaf.next;
        leaf.next  = right_off;

        if (right.next != 0) {
            DiskNode<KeyType> nxt = DiskNode<KeyType>::read_at(file_, right.next);
            nxt.prev = right_off;
            nxt.write_at(file_, right.next);
            uint64_t end = right.next + nxt.serialized_size();
            if (end > header_.free_offset) header_.free_offset = end;
        } else {
            header_.tail_leaf_offset = right_off;
        }

        leaf.write_at(file_, leaf_off);
        {
            uint64_t end = leaf_off + leaf.serialized_size();
            if (end > header_.free_offset) header_.free_offset = end;
        }

        const KeyType sep = right.keys.front();
        insert_into_parent(leaf.parent, leaf_off, sep, right_off);
    }

    void insert_into_parent(uint64_t parent_off, uint64_t left_off,
                            const KeyType& sep, uint64_t right_off) {
        if (parent_off == 0) {
            DiskNode<KeyType> new_root;
            new_root.is_leaf  = false;
            new_root.parent   = 0;
            new_root.keys.push_back(sep);
            new_root.children.push_back(left_off);
            new_root.children.push_back(right_off);

            uint64_t new_root_off = allocate_node();
            new_root.write_at(file_, new_root_off);

            update_parent(left_off, new_root_off);
            update_parent(right_off, new_root_off);

            header_.root_offset = new_root_off;
            {
                uint64_t end = new_root_off + new_root.serialized_size();
                if (end > header_.free_offset) header_.free_offset = end;
            }
            return;
        }

        DiskNode<KeyType> parent = DiskNode<KeyType>::read_at(file_, parent_off);

        size_t pos = 0;
        for (; pos < parent.children.size(); ++pos) {
            if (parent.children[pos] == left_off) break;
        }
        if (pos == parent.children.size()) {
            throw std::runtime_error("insert_into_parent: left child not found");
        }

        parent.keys.insert(parent.keys.begin() + pos, sep);
        parent.children.insert(parent.children.begin() + pos + 1, right_off);

        if (parent.keys.size() > MAX_KEYS) {
            parent.write_at(file_, parent_off);
            {
                uint64_t end = parent_off + parent.serialized_size();
                if (end > header_.free_offset) header_.free_offset = end;
            }
            split_internal(parent_off);
        } else {
            parent.write_at(file_, parent_off);
            uint64_t end = parent_off + parent.serialized_size();
            if (end > header_.free_offset) header_.free_offset = end;
        }
    }

    void split_internal(uint64_t node_off) {
        DiskNode<KeyType> node = DiskNode<KeyType>::read_at(file_, node_off);
        size_t mid = node.keys.size() / 2;
        const KeyType up = node.keys[mid];

        DiskNode<KeyType> right;
        right.is_leaf = false;
        right.parent  = node.parent;
        right.keys.assign(node.keys.begin() + mid + 1, node.keys.end());
        right.children.assign(node.children.begin() + mid + 1, node.children.end());

        node.keys.resize(mid);
        node.children.resize(mid + 1);

        uint64_t right_off = allocate_node();
        right.write_at(file_, right_off);
        {
            uint64_t end = right_off + right.serialized_size();
            if (end > header_.free_offset) header_.free_offset = end;
        }

        for (uint64_t c : right.children) {
            update_parent(c, right_off);
        }

        node.write_at(file_, node_off);
        {
            uint64_t end = node_off + node.serialized_size();
            if (end > header_.free_offset) header_.free_offset = end;
        }

        insert_into_parent(node.parent, node_off, up, right_off);
    }

    void update_parent(uint64_t node_off, uint64_t parent_off) {
        DiskNode<KeyType> n = DiskNode<KeyType>::read_at(file_, node_off);
        n.parent = parent_off;
        n.write_at(file_, node_off);
        uint64_t end = node_off + n.serialized_size();
        if (end > header_.free_offset) header_.free_offset = end;
    }
};

} // namespace dbms

#endif