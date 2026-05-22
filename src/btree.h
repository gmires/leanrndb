#pragma once

#include <cstdint>
#include <vector>
#include <functional>
#include "pager.h"

class BTree {
public:
    BTree(Pager* pager, uint32_t root_page_num);

    uint32_t root_page_num() const { return root_page_num_; }
    void set_root_page_num(uint32_t n) { root_page_num_ = n; }

    bool insert(const std::vector<uint8_t>& key, const std::vector<uint8_t>& value);
    bool find(const std::vector<uint8_t>& key, std::vector<uint8_t>& value_out);
    bool remove(const std::vector<uint8_t>& key);
    void scan_all(std::function<void(const std::vector<uint8_t>&,
                                     const std::vector<uint8_t>&)> cb);

    class Cursor {
    public:
        explicit Cursor(BTree* tree) : tree_(tree) {}
        bool seek(const std::vector<uint8_t>& key);
        bool next();
        std::vector<uint8_t> key();
        std::vector<uint8_t> value();
        bool valid() const { return valid_; }
    private:
        BTree* tree_;
        struct StackFrame { uint32_t page; int idx; };
        std::vector<StackFrame> stack_;
        bool valid_ = false;
    };

private:
    // Page header: is_leaf(1) + num_cells(2) + cell_area_end(2) +
    //             right_sibling(4) + left_sibling(4) + [right_child(4)]
    struct PageHdr {
        bool is_leaf;
        uint16_t num_cells;
        uint16_t cell_area_end;
        uint32_t right_sibling;
        uint32_t left_sibling;
        uint32_t right_child;  // only for internal nodes
    };

    static constexpr size_t HDR_LEAF = 13;
    static constexpr size_t HDR_INT  = 17;

    static PageHdr read_hdr(const uint8_t* page);
    static void write_hdr(uint8_t* page, const PageHdr& hdr);
    static size_t hdr_size(bool is_leaf);

    static uint16_t cell_offset(const uint8_t* page, uint16_t idx);
    static void set_cell_offset(uint8_t* page, uint16_t idx, uint16_t off);

    int find_key_in_node(const uint8_t* page, const std::vector<uint8_t>& key);
    uint32_t find_child_page(const uint8_t* page, const std::vector<uint8_t>& key);

    static size_t leaf_cell_size(const std::vector<uint8_t>& key,
                                 const std::vector<uint8_t>& value);
    static size_t int_cell_size(const std::vector<uint8_t>& key);
    static size_t free_space(const uint8_t* page);

    void read_leaf_cell(const uint8_t* page, uint16_t idx,
                        std::vector<uint8_t>& key,
                        std::vector<uint8_t>& value);
    static void read_leaf_key(const uint8_t* page, uint16_t idx,
                              std::vector<uint8_t>& key);
    static void read_int_cell(const uint8_t* page, uint16_t idx,
                              std::vector<uint8_t>& key,
                              uint32_t& child_page);
    static void read_int_key(const uint8_t* page, uint16_t idx,
                             std::vector<uint8_t>& key);
    static void set_int_cell_child(uint8_t* page, uint16_t idx, uint32_t child);

    void insert_into_leaf(uint8_t* page,
                          const std::vector<uint8_t>& key,
                          const std::vector<uint8_t>& value);
    void insert_into_internal(uint8_t* page,
                              const std::vector<uint8_t>& key,
                              uint32_t right_child_page);

    bool insert_impl(uint32_t page_num,
                     const std::vector<uint8_t>& key,
                     const std::vector<uint8_t>& value,
                     std::vector<uint8_t>& split_key,
                     uint32_t& split_right_child);

    bool remove_impl(uint32_t page_num,
                     const std::vector<uint8_t>& key,
                     uint32_t& freed_page,
                     uint32_t& survivor_page);

    static int key_cmp(const std::vector<uint8_t>& a,
                       const std::vector<uint8_t>& b);

    uint32_t write_overflow_pages(const std::vector<uint8_t>& data);
    void read_overflow_pages(uint32_t first_page, uint32_t total_size,
                             std::vector<uint8_t>& out);
    void free_overflow_pages(uint32_t first_page);

    Pager* pager_;
    uint32_t root_page_num_;
};
