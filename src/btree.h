#pragma once

#include <cstdint>
#include <vector>
#include <functional>
#include "pager.h"

/**
 * BTree: albero B su pagine.
 *
 * Ogni nodo occupa una pagina (4 KB). Le foglie contengono coppie
 * (chiave → valore), mentre i nodi interni contengono chiavi separatori
 * e puntatori ai figli. Le chiavi e i valori sono byte-array generici.
 *
 * Supporta insert (con split automatico), find, remove (solo su alberi
 * a foglia singola, senza ribilanciamento), scan_all e cursori.
 */
class BTree {
public:
    BTree(Pager* pager, uint32_t root_page_num);

    uint32_t root_page_num() const { return root_page_num_; }
    void set_root_page_num(uint32_t n) { root_page_num_ = n; }

    /** Inserisce (chiave, valore). Se il nodo è pieno, lo divide. */
    bool insert(const std::vector<uint8_t>& key, const std::vector<uint8_t>& value);
    /** Cerca una chiave; se trovata, scrive il valore in value_out e restituisce true. */
    bool find(const std::vector<uint8_t>& key, std::vector<uint8_t>& value_out);
    /** Rimuove una chiave (solo su alberi a foglia singola, senza ribilanciamento). */
    bool remove(const std::vector<uint8_t>& key);
    /** Scansiona tutte le coppie (chiave, valore) con una DFS nelle foglie. */
    void scan_all(std::function<void(const std::vector<uint8_t>&,
                                     const std::vector<uint8_t>&)> cb);

    /**
     * Cursore per scorrere le chiavi in ordine.
     * seek() parte dalla chiave data; next() avanza all'interno della stessa foglia.
     * Al momento supporta solo scansione entro una singola foglia.
     */
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
    // Intestazione di ogni pagina B-tree
    struct PageHdr {
        bool is_leaf;          // true = foglia, false = nodo interno
        uint16_t num_cells;    // quante celle contiene
        uint16_t cell_area_end; // offset da cui partono i dati delle celle (cresce dal fondo pagina)
        uint32_t right_child;  // per nodi interni: figlio destro (chiavi > ultima chiave)
    };

    static PageHdr read_hdr(const uint8_t* page);
    static void write_hdr(uint8_t* page, const PageHdr& hdr);
    static size_t hdr_size(bool is_leaf);

    // Array di offset alle celle (2 byte per cella)
    static uint16_t cell_offset(const uint8_t* page, uint16_t idx);
    static void set_cell_offset(uint8_t* page, uint16_t idx, uint16_t off);

    // Cerca la posizione di una chiave in un nodo (binary search)
    int find_key_in_node(const uint8_t* page, const std::vector<uint8_t>& key);

    static size_t leaf_cell_size(const std::vector<uint8_t>& key,
                                 const std::vector<uint8_t>& value);
    static size_t int_cell_size(const std::vector<uint8_t>& key);
    static size_t free_space(const uint8_t* page);

    // Legge una cella da un nodo foglia o interno
    void read_leaf_cell(const uint8_t* page, uint16_t idx,
                        std::vector<uint8_t>& key,
                        std::vector<uint8_t>& value);
    static void read_leaf_key(const uint8_t* page, uint16_t idx,
                              std::vector<uint8_t>& key);
    static void read_int_cell(const uint8_t* page, uint16_t idx,
                              std::vector<uint8_t>& key,
                              uint32_t& child_page);

    // Scrive una cella in un nodo (foglia o interno)
    void insert_into_leaf(uint8_t* page,
                          const std::vector<uint8_t>& key,
                          const std::vector<uint8_t>& value);
    void insert_into_internal(uint8_t* page,
                              const std::vector<uint8_t>& key,
                              uint32_t right_child_page);

    // Inserimento ricorsivo: restituisce true se il nodo si è diviso
    bool insert_impl(uint32_t page_num,
                     const std::vector<uint8_t>& key,
                     const std::vector<uint8_t>& value,
                     std::vector<uint8_t>& split_key,
                     uint32_t& split_right_child);

    static int key_cmp(const std::vector<uint8_t>& a,
                       const std::vector<uint8_t>& b);

    // Overflow page helpers
    uint32_t write_overflow_pages(const std::vector<uint8_t>& data);
    void read_overflow_pages(uint32_t first_page, uint32_t total_size,
                             std::vector<uint8_t>& out);
    void free_overflow_pages(uint32_t first_page);

    Pager* pager_;
    uint32_t root_page_num_;
};
