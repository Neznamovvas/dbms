#ifndef DBMS_INDEX_FILE_HPP
#define DBMS_INDEX_FILE_HPP

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

namespace dbms {

struct TreeHeader {
    static constexpr const char* MAGIC = "BPLUSTRE";
    static constexpr uint32_t VERSION = 1;

    uint32_t version   = VERSION;
    uint8_t  key_type  = 0;
    uint8_t  reserved1 = 0;
    uint16_t reserved2 = 0;
    uint32_t order     = 4;
    uint32_t max_keys  = 7;
    uint32_t min_keys  = 3;
    uint64_t root_offset      = 0;
    uint64_t head_leaf_offset = 0;
    uint64_t tail_leaf_offset = 0;
    uint64_t node_count       = 0;
    uint64_t key_count        = 0;
    uint64_t free_offset      = 0;
    char     reserved3[24]    = {0};

    static constexpr size_t SIZE =
        8 + 4 + 1 + 1 + 2 + 4 + 4 + 4 +
        8 + 8 + 8 + 8 + 8 + 8 + 24;

    void write(std::fstream& f) const {
        f.write(MAGIC, 8);
        f.write(reinterpret_cast<const char*>(&version), sizeof(version));
        f.write(reinterpret_cast<const char*>(&key_type), 1);
        f.write(reinterpret_cast<const char*>(&reserved1), 1);
        f.write(reinterpret_cast<const char*>(&reserved2), sizeof(reserved2));
        f.write(reinterpret_cast<const char*>(&order), sizeof(order));
        f.write(reinterpret_cast<const char*>(&max_keys), sizeof(max_keys));
        f.write(reinterpret_cast<const char*>(&min_keys), sizeof(min_keys));
        f.write(reinterpret_cast<const char*>(&root_offset), sizeof(root_offset));
        f.write(reinterpret_cast<const char*>(&head_leaf_offset), sizeof(head_leaf_offset));
        f.write(reinterpret_cast<const char*>(&tail_leaf_offset), sizeof(tail_leaf_offset));
        f.write(reinterpret_cast<const char*>(&node_count), sizeof(node_count));
        f.write(reinterpret_cast<const char*>(&key_count), sizeof(key_count));
        f.write(reinterpret_cast<const char*>(&free_offset), sizeof(free_offset));
        f.write(reserved3, sizeof(reserved3));
    }

    static TreeHeader read(std::fstream& f, uint64_t offset) {
        TreeHeader h;
        f.clear();
        f.seekg(static_cast<std::streamoff>(offset));

        char magic[8] = {0};
        f.read(magic, 8);
        if (std::memcmp(magic, MAGIC, 8) != 0) {
            throw std::runtime_error("Invalid B+tree magic inside .idx");
        }

        f.read(reinterpret_cast<char*>(&h.version), sizeof(h.version));
        f.read(reinterpret_cast<char*>(&h.key_type), 1);
        f.read(reinterpret_cast<char*>(&h.reserved1), 1);
        f.read(reinterpret_cast<char*>(&h.reserved2), sizeof(h.reserved2));
        f.read(reinterpret_cast<char*>(&h.order), sizeof(h.order));
        f.read(reinterpret_cast<char*>(&h.max_keys), sizeof(h.max_keys));
        f.read(reinterpret_cast<char*>(&h.min_keys), sizeof(h.min_keys));
        f.read(reinterpret_cast<char*>(&h.root_offset), sizeof(h.root_offset));
        f.read(reinterpret_cast<char*>(&h.head_leaf_offset), sizeof(h.head_leaf_offset));
        f.read(reinterpret_cast<char*>(&h.tail_leaf_offset), sizeof(h.tail_leaf_offset));
        f.read(reinterpret_cast<char*>(&h.node_count), sizeof(h.node_count));
        f.read(reinterpret_cast<char*>(&h.key_count), sizeof(h.key_count));
        f.read(reinterpret_cast<char*>(&h.free_offset), sizeof(h.free_offset));
        f.read(h.reserved3, sizeof(h.reserved3));

        if (h.version != VERSION) {
            throw std::runtime_error("Unsupported B+tree version");
        }
        return h;
    }

    void write_to(std::fstream& f, uint64_t offset) const {
        f.clear();
        f.seekp(static_cast<std::streamoff>(offset));
        write(f);
    }
};

} // namespace dbms

#endif