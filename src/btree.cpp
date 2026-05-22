#include "btree.h"
#include <cstring>
#include <algorithm>
#include <cassert>

// ── lettura/scrittura little-endian ─────────────────────────────

static uint16_t r16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static void w16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}
static uint32_t r32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void w32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

// ── overflow pages ─────────────────────────────────────────────
static constexpr uint32_t OVERFLOW_SENTINEL = 0xFFFFFFFF;
static constexpr size_t OVERFLOW_PAGE_DATA = Pager::PAGE_SIZE - 8;

static inline bool is_overflow(const std::vector<uint8_t>& key,
                               const std::vector<uint8_t>& value) {
    return (2 + key.size() + 4 + value.size()) > 4081;
}

// ── PageHdr ────────────────────────────────────────────────────

BTree::PageHdr BTree::read_hdr(const uint8_t* page) {
    PageHdr h{};
    h.is_leaf        = (page[0] != 0);
    h.num_cells      = r16(page + 1);
    h.cell_area_end  = r16(page + 3);
    h.right_sibling  = r32(page + 5);
    h.left_sibling   = r32(page + 9);
    if (!h.is_leaf)
        h.right_child = r32(page + 13);
    else
        h.right_child = 0;
    return h;
}

void BTree::write_hdr(uint8_t* page, const PageHdr& h) {
    page[0] = h.is_leaf ? 1 : 0;
    w16(page + 1, h.num_cells);
    w16(page + 3, h.cell_area_end);
    w32(page + 5, h.right_sibling);
    w32(page + 9, h.left_sibling);
    if (!h.is_leaf)
        w32(page + 13, h.right_child);
}

size_t BTree::hdr_size(bool is_leaf) {
    return is_leaf ? HDR_LEAF : HDR_INT;
}

// ── cell offset array ──────────────────────────────────────────

uint16_t BTree::cell_offset(const uint8_t* page, uint16_t idx) {
    size_t hs = hdr_size(page[0] != 0);
    return r16(page + hs + idx * 2);
}

void BTree::set_cell_offset(uint8_t* page, uint16_t idx, uint16_t off) {
    size_t hs = hdr_size(page[0] != 0);
    w16(page + hs + idx * 2, off);
}

// ── cell sizes ─────────────────────────────────────────────────

size_t BTree::leaf_cell_size(const std::vector<uint8_t>& key,
                             const std::vector<uint8_t>& value) {
    if (is_overflow(key, value))
        return 2 + key.size() + 12;
    return 2 + key.size() + 4 + value.size();
}

size_t BTree::int_cell_size(const std::vector<uint8_t>& key) {
    return 2 + key.size() + 4;
}

size_t BTree::free_space(const uint8_t* page) {
    auto h = read_hdr(page);
    size_t hs = hdr_size(h.is_leaf);
    size_t used_by_ptrs = h.num_cells * 2;
    size_t top = hs + used_by_ptrs;
    if (h.cell_area_end <= top) return 0;
    return h.cell_area_end - top;
}

// ── read cells ─────────────────────────────────────────────────

void BTree::read_leaf_key(const uint8_t* page, uint16_t idx,
                          std::vector<uint8_t>& key) {
    uint16_t off = cell_offset(page, idx);
    uint16_t ks  = r16(page + off);
    key.assign(page + off + 2, page + off + 2 + ks);
}

void BTree::read_leaf_cell(const uint8_t* page, uint16_t idx,
                           std::vector<uint8_t>& key,
                           std::vector<uint8_t>& value) {
    uint16_t off = cell_offset(page, idx);
    uint16_t ks  = r16(page + off);
    key.assign(page + off + 2, page + off + 2 + ks);
    uint32_t vs  = r32(page + off + 2 + ks);

    if (vs == OVERFLOW_SENTINEL) {
        uint32_t total_size = r32(page + off + 2 + ks + 4);
        uint32_t first_page = r32(page + off + 2 + ks + 8);
        read_overflow_pages(first_page, total_size, value);
    } else {
        value.assign(page + off + 2 + ks + 4,
                     page + off + 2 + ks + 4 + vs);
    }
}

void BTree::read_int_cell(const uint8_t* page, uint16_t idx,
                          std::vector<uint8_t>& key,
                          uint32_t& child_page) {
    uint16_t off = cell_offset(page, idx);
    uint16_t ks  = r16(page + off);
    key.assign(page + off + 2, page + off + 2 + ks);
    child_page   = r32(page + off + 2 + ks);
}

void BTree::read_int_key(const uint8_t* page, uint16_t idx,
                         std::vector<uint8_t>& key) {
    uint16_t off = cell_offset(page, idx);
    uint16_t ks  = r16(page + off);
    key.assign(page + off + 2, page + off + 2 + ks);
}

void BTree::set_int_cell_child(uint8_t* page, uint16_t idx, uint32_t child) {
    uint16_t off = cell_offset(page, idx);
    uint16_t ks  = r16(page + off);
    w32(page + off + 2 + ks, child);
}

// ── key compare ────────────────────────────────────────────────

int BTree::key_cmp(const std::vector<uint8_t>& a,
                   const std::vector<uint8_t>& b) {
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    if (a.size() < b.size()) return -1;
    if (a.size() > b.size()) return 1;
    return 0;
}

// ── binary search in a node ────────────────────────────────────

int BTree::find_key_in_node(const uint8_t* page,
                            const std::vector<uint8_t>& key) {
    auto h = read_hdr(page);
    int lo = 0, hi = h.num_cells;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        std::vector<uint8_t> k;
        if (h.is_leaf) {
            read_leaf_key(page, mid, k);
        } else {
            read_int_key(page, mid, k);
        }
        if (key_cmp(key, k) <= 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

// ── internal node: find child for a key ────────────────────────
//
// L'idea fondamentale dei B-tree: in un nodo interno, la cella i-esima
// contiene un separatore S e un puntatore a un figlio.
//
//   cella[i] = (separatore S, puntatore al figlio sinistro)
//
// Il separatore S è la PRIMA chiave del fratello destro.
// Quindi: tutte le chiavi < S stanno NEL figlio sinistro (cella[i]).
//         tutte le chiavi >= S stanno nel prossimo figlio (cella[i+1] o right_child).
//
// Attenzione: la condizione è STRETTAMENTE minore, non ≤.
// Se usassimo ≤, una chiave uguale al separatore finirebbe in AMBI i figli
// (sia sinistro che destro), creando duplicati nella scansione.
//
// find_key_in_node restituisce l'indice della prima cella col separatore ≥ key.
// Poi scorriamo in avanti fino a trovare un separatore STRETTAMENTE > key.

uint32_t BTree::find_child_page(const uint8_t* page,
                                const std::vector<uint8_t>& key) {
    auto h = read_hdr(page);
    int idx = find_key_in_node(page, key);

    // Scorri le celle: se key < separatore, quel figlio è quello giusto.
    // Se key >= separatore, passa alla cella successiva.
    while (idx < h.num_cells) {
        uint32_t cp;
        std::vector<uint8_t> sep;
        read_int_cell(page, idx, sep, cp);
        if (key_cmp(key, sep) < 0)
            return cp;
        idx++;
    }
    // Nessuna cella ha separatore > key → vai al right_child
    return h.right_child;
}

// ── write cells ────────────────────────────────────────────────
//
// Formato della pagina (slotted-page):
//
//   [header] [offset array → ... ] [← ... celle ← cell_area_end]
//
// - header: 13 byte per foglia, 17 per interno
// - offset array: in fondo all'array dei puntatori alle celle,
//   parte da PAGE_SIZE-2 e cresce verso il basso
// - celle: scritte da cell_area_end verso l'alto (verso PAGE_SIZE)
// - cell_area_end: indirizzo del byte più basso occupato da una cella
//   (inizialmente PAGE_SIZE, diminuisce a ogni inserimento)
//
// Formato cella foglia:
//   [2 key_len][key][4 value_len][value]
//   Se overflow: value_len = 0xFFFFFFFF, segue [4 total_size][4 first_page]
//
// Formato cella interna:
//   [2 key_len][key][4 child_page]

void BTree::insert_into_leaf(uint8_t* page,
                             const std::vector<uint8_t>& key,
                             const std::vector<uint8_t>& value) {
    auto h = read_hdr(page);
    size_t csize = leaf_cell_size(key, value);
    size_t hs = HDR_LEAF;

    // Scrivi la cella in fondo all'area libera
    uint16_t new_end = h.cell_area_end - (uint16_t)csize;
    size_t pos = new_end;
    w16(page + pos, (uint16_t)key.size());
    pos += 2;
    memcpy(page + pos, key.data(), key.size());
    pos += key.size();

    if (is_overflow(key, value)) {
        w32(page + pos, OVERFLOW_SENTINEL);
        pos += 4;
        w32(page + pos, (uint32_t)value.size());
        pos += 4;
        uint32_t first_page = write_overflow_pages(value);
        w32(page + pos, first_page);
    } else {
        w32(page + pos, (uint32_t)value.size());
        pos += 4;
        memcpy(page + pos, value.data(), value.size());
    }

    // Inserisci l'offset nell'array ordinato (per chiave)
    int slot = find_key_in_node(page, key);
    memmove(page + hs + (slot + 1) * 2,
            page + hs + slot * 2,
            (h.num_cells - slot) * 2);
    set_cell_offset(page, slot, new_end);

    h.num_cells++;
    h.cell_area_end = new_end;
    write_hdr(page, h);
}

void BTree::insert_into_internal(uint8_t* page,
                                 const std::vector<uint8_t>& key,
                                 uint32_t right_child_page) {
    auto h = read_hdr(page);
    size_t csize = int_cell_size(key);
    size_t hs = HDR_INT;

    uint16_t new_end = h.cell_area_end - (uint16_t)csize;
    size_t pos = new_end;
    w16(page + pos, (uint16_t)key.size());
    pos += 2;
    memcpy(page + pos, key.data(), key.size());
    pos += key.size();
    w32(page + pos, right_child_page);

    int slot = find_key_in_node(page, key);
    memmove(page + hs + (slot + 1) * 2,
            page + hs + slot * 2,
            (h.num_cells - slot) * 2);
    set_cell_offset(page, slot, new_end);

    h.num_cells++;
    h.cell_area_end = new_end;
    write_hdr(page, h);
}

// ── overflow pages ─────────────────────────────────────────────

uint32_t BTree::write_overflow_pages(const std::vector<uint8_t>& data) {
    size_t offset = 0;
    size_t remaining = data.size();
    uint32_t first_page = 0;
    uint32_t prev_page = 0;

    while (remaining > 0) {
        uint32_t page_num = pager_->allocate_page();
        uint8_t* page = pager_->get_page(page_num);

        if (first_page == 0)
            first_page = page_num;

        size_t chunk = remaining > OVERFLOW_PAGE_DATA ? OVERFLOW_PAGE_DATA : remaining;

        w32(page, 0);
        w32(page + 4, (uint32_t)chunk);
        memcpy(page + 8, data.data() + offset, chunk);

        if (prev_page != 0) {
            uint8_t* prev = pager_->get_page(prev_page);
            w32(prev, page_num);
        }

        prev_page = page_num;
        offset += chunk;
        remaining -= chunk;
    }
    return first_page;
}

void BTree::read_overflow_pages(uint32_t first_page, uint32_t total_size,
                                std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(total_size);

    uint32_t page_num = first_page;
    while (page_num != 0) {
        uint8_t* page = pager_->get_page(page_num);
        uint32_t next = r32(page);
        uint32_t data_len = r32(page + 4);
        out.insert(out.end(), page + 8, page + 8 + data_len);
        page_num = next;
    }
}

void BTree::free_overflow_pages(uint32_t first_page) {
    uint32_t page_num = first_page;
    while (page_num != 0) {
        uint8_t* page = pager_->get_page(page_num);
        uint32_t next = r32(page);
        pager_->free_page(page_num);
        page_num = next;
    }
}

// ── constructor ────────────────────────────────────────────────

BTree::BTree(Pager* pager, uint32_t root_page_num)
    : pager_(pager), root_page_num_(root_page_num) {
    uint8_t* page = pager_->get_page(root_page_num_);
    auto h = read_hdr(page);
    if (h.num_cells == 0 && h.cell_area_end == 0) {
        h.is_leaf = true;
        h.num_cells = 0;
        h.cell_area_end = Pager::PAGE_SIZE;
        h.right_sibling = 0;
        h.left_sibling = 0;
        h.right_child = 0;
        write_hdr(page, h);
    }
}

// ── find ───────────────────────────────────────────────────────

bool BTree::find(const std::vector<uint8_t>& key,
                 std::vector<uint8_t>& value_out) {
    uint32_t page_num = root_page_num_;
    for (;;) {
        uint8_t* page = pager_->get_page(page_num);
        auto h = read_hdr(page);
        if (h.is_leaf) {
            int idx = find_key_in_node(page, key);
            if (idx < h.num_cells) {
                std::vector<uint8_t> k, v;
                read_leaf_cell(page, idx, k, v);
                if (key_cmp(k, key) == 0) {
                    value_out = std::move(v);
                    return true;
                }
            }
            return false;
        }
        page_num = find_child_page(page, key);
    }
}

// ── remove (public) ────────────────────────────────────────────

bool BTree::remove(const std::vector<uint8_t>& key) {
    uint32_t freed_page = 0;
    uint32_t survivor_page = 0;
    bool found = remove_impl(root_page_num_, key, freed_page, survivor_page);

    // If the root is internal and now empty, reduce tree height
    uint8_t* root_page = pager_->get_page(root_page_num_);
    auto h = read_hdr(root_page);
    if (!h.is_leaf && h.num_cells == 0) {
        uint32_t old_root = root_page_num_;
        root_page_num_ = h.right_child;
        pager_->free_page(old_root);
    }

    return found;
}

// ── remove_impl (recursive) ────────────────────────────────────
//
// Rimuove key dalla sotto-albero con radice page_num.
// Restituisce:
//   - found = true se la chiave è stata trovata e rimossa
//   - freed_page = pagina liberata da un merge (0 se nessun merge)
//   - survivor_page = pagina che ha assorbito i dati (0 se nessun merge)
//
// Il genitore (chiamante) deve aggiornare il suo puntatore:
//   DOVE c'era freed_page, ORA c'è survivor_page
//
// Merge (foglia vuota → assorbe il fratello):
//   Quando una foglia resta senza celle (num_cells == 0), invece di
//   lasciarla vuota e sprecarla, cerchiamo di assorbire le celle del
//   fratello destro (o sinistro). Questo mantiene l'albero denso.
//
//   Problema critico: remove_cell_from_leaf NON aggiorna cell_area_end.
//   Dopo molte cancellazioni, cell_area_end punta molto in basso.
//   Quando poi inseriamo nuove celle (nel merge), insert_into_leaf usa
//   cell_area_end come punto di partenza, scrivendo i dati in mezzo
//   alla pagina e corrompendo l'header. La soluzione è resettare
//   cell_area_end = PAGE_SIZE prima del merge.
//
// Genitore (aggiornamento celle interne):
//   Dopo un merge, il genitore deve aggiornare i puntatori che
//   indicavano la pagina liberata. Ma attenzione: dopo l'aggiornamento
//   potrebbero esserci DUE celle adiacenti con lo stesso figlio, oppure
//   l'ultima cella potrebbe puntare allo stesso page di right_child.
//   In entrambi i casi scan_all visiterebbe la stessa foglia due volte,
//   producendo duplicati. Dobbiamo rimuovere le celle ridondanti.

bool BTree::remove_impl(uint32_t page_num,
                        const std::vector<uint8_t>& key,
                        uint32_t& freed_page,
                        uint32_t& survivor_page) {
    uint8_t* page = pager_->get_page(page_num);
    auto h = read_hdr(page);
    freed_page = 0;
    survivor_page = 0;

    if (h.is_leaf) {
        // ── FOGLIA: cerca ed elimina la cella ──
        int idx = find_key_in_node(page, key);
        if (idx >= h.num_cells) return false;

        std::vector<uint8_t> cell_key;
        read_leaf_key(page, idx, cell_key);
        if (key_cmp(cell_key, key) != 0) return false;

        // Libera pagine overflow se presenti
        uint16_t off = cell_offset(page, idx);
        uint16_t ks = r16(page + off);
        uint32_t vs = r32(page + off + 2 + ks);
        if (vs == OVERFLOW_SENTINEL) {
            uint32_t first_page = r32(page + off + 2 + ks + 8);
            free_overflow_pages(first_page);
        }

        // Rimuovi la cella: sposta gli offset successivi verso sinistra
        // NOTA: N.B. cell_area_end NON viene aggiornato.
        // Questo è ok finché non si reinseriscono celle (vedi merge sotto).
        size_t hs = HDR_LEAF;
        memmove(page + hs + idx * 2,
                page + hs + (idx + 1) * 2,
                (h.num_cells - idx - 1) * 2);
        h.num_cells--;
        write_hdr(page, h);

        // ── MERGE: se la foglia è vuota, assorbi il fratello ──
        if (h.num_cells == 0 && page_num != root_page_num_) {
            // Prova prima il fratello DESTRO
            if (h.right_sibling != 0) {
                uint8_t* right = pager_->get_page(h.right_sibling);
                auto rh = read_hdr(right);
                size_t right_data = Pager::PAGE_SIZE - rh.cell_area_end;
                size_t needed = HDR_LEAF + (rh.num_cells * 2) + right_data;
                if (needed <= Pager::PAGE_SIZE) {
                    // RESET cell_area_end!
                    // remove_cell_from_leaf non lo aggiorna, quindi dopo tante
                    // cancellazioni punta molto in basso. insert_into_leaf lo usa
                    // per posizionare le nuove celle — se non lo resettiamo,
                    // scrive in mezzo alla pagina e corrompe l'header.
                    h.cell_area_end = Pager::PAGE_SIZE;
                    write_hdr(page, h);

                    // Copia TUTTE le celle del fratello nella pagina vuota
                    for (uint16_t i = 0; i < rh.num_cells; i++) {
                        std::vector<uint8_t> k, v;
                        read_leaf_cell(right, i, k, v);
                        insert_into_leaf(page, k, v);
                    }
                    // Aggiorna i puntatori ai fratelli
                    // (il fratello del fratello diventa il nostro nuovo fratello)
                    if (rh.right_sibling != 0) {
                        uint8_t* rrs = pager_->get_page(rh.right_sibling);
                        auto rrs_h = read_hdr(rrs);
                        rrs_h.left_sibling = page_num;
                        write_hdr(rrs, rrs_h);
                    }
                    auto nh = read_hdr(page);
                    nh.right_sibling = rh.right_sibling;
                    write_hdr(page, nh);

                    pager_->free_page(h.right_sibling);
                    freed_page = h.right_sibling;
                    survivor_page = page_num;
                    return true;
                }
            }
            // Se il destro non ci sta, prova il fratello SINISTRO
            if (h.left_sibling != 0) {
                uint8_t* left = pager_->get_page(h.left_sibling);
                auto lh = read_hdr(left);
                size_t left_data = Pager::PAGE_SIZE - lh.cell_area_end;
                size_t needed = HDR_LEAF + (lh.num_cells * 2) + left_data;
                if (needed <= Pager::PAGE_SIZE) {
                    // In questo caso NON copiamo celle: la pagina sinistra
                    // assorbe semplicemente l'intervallo di chiavi saltandoci.
                    auto nlh = read_hdr(left);
                    nlh.right_sibling = h.right_sibling;
                    write_hdr(left, nlh);
                    if (h.right_sibling != 0) {
                        uint8_t* rrs = pager_->get_page(h.right_sibling);
                        auto rrs_h = read_hdr(rrs);
                        rrs_h.left_sibling = h.left_sibling;
                        write_hdr(rrs, rrs_h);
                    }
                    pager_->free_page(page_num);
                    freed_page = page_num;
                    survivor_page = h.left_sibling;
                    return true;
                }
            }
        }
        return true;
    }

    // ── NODO INTERNO: scendi ricorsivamente ──
    uint32_t child_page_num = find_child_page(page, key);
    uint32_t child_freed = 0;
    uint32_t child_survivor = 0;
    bool found = remove_impl(child_page_num, key, child_freed, child_survivor);

    if (!found) return false;

    if (child_freed != 0) {
        // ── Il figlio ha fatto un merge: aggiorna il genitore ──
        //
        // Dopo un merge, una pagina figlia è stata liberata (child_freed)
        // e i suoi dati sono ora in child_survivor.
        // Dobbiamo aggiornare TUTTI i puntatori che indicavano child_freed.
        //
        // 1. Aggiorna tutte le celle il cui figlio == child_freed
        // 2. Aggiorna right_child se == child_freed
        // 3. Rimuovi celle adiacenti ridondanti (stesso figlio)
        // 4. Rimuovi l'ultima cella se il suo figlio == right_child

        h = read_hdr(page);
        for (uint16_t i = 0; i < h.num_cells; i++) {
            uint32_t cp;
            std::vector<uint8_t> s;
            read_int_cell(page, i, s, cp);
            if (cp == child_freed)
                set_int_cell_child(page, i, child_survivor);
        }
        if (h.right_child == child_freed) {
            h.right_child = child_survivor;
            write_hdr(page, h);
        }

        // ── Rimuovi celle adiacenti con lo stesso figlio ──
        // Dopo l'aggiornamento, potremmo avere:
        //   cella[0] = (sep=58, figlio=1)
        //   cella[1] = (sep=115, figlio=1)  ← aggiornata da 2→1
        // Qui cella[0] è ridondante: TUTTE le chiavi che andrebbero a
        // cella[0] vanno ANCHE a cella[1] (stesso figlio).
        // Tra celle adiacenti con lo stesso figlio, TENIAMO SOLO L'ULTIMA
        // (quella col separatore più alto).
        h = read_hdr(page);
        uint16_t new_nc = 0;
        for (uint16_t i = 0; i < h.num_cells; i++) {
            uint32_t cp;
            std::vector<uint8_t> sep;
            read_int_cell(page, i, sep, cp);
            bool skip = false;
            if (i + 1 < h.num_cells) {
                uint32_t next_cp;
                std::vector<uint8_t> next_sep;
                read_int_cell(page, i + 1, next_sep, next_cp);
                if (cp == next_cp)
                    skip = true;
            }
            if (!skip) {
                if (i != new_nc)
                    set_cell_offset(page, new_nc, cell_offset(page, i));
                new_nc++;
            }
        }
        if (new_nc < h.num_cells) {
            h.num_cells = new_nc;
            write_hdr(page, h);
        }

        // ── Rimuovi l'ultima cella se punta allo stesso page di right_child ──
        // Dopo il merge, right_child potrebbe essere stato aggiornato a
        // child_survivor. Se l'ultima cella punta ANCHE a child_survivor,
        // allora è ridondante: TUTTE le chiavi che passano da qui vanno
        // allo stesso figlio (sia via cella che via right_child).
        // scan_all visiterebbe la stessa foglia due volte.
        h = read_hdr(page);
        if (h.num_cells > 0) {
            uint32_t last_cp;
            std::vector<uint8_t> last_sep;
            read_int_cell(page, h.num_cells - 1, last_sep, last_cp);
            if (last_cp == h.right_child) {
                h.num_cells--;
                write_hdr(page, h);
            }
        }
    }

    return true;
}

// ── insert (public) ────────────────────────────────────────────

bool BTree::insert(const std::vector<uint8_t>& key,
                   const std::vector<uint8_t>& value) {
    std::vector<uint8_t> split_key;
    uint32_t split_right_child = 0;
    bool split = insert_impl(root_page_num_, key, value,
                             split_key, split_right_child);
    if (split) {
        uint32_t new_root = pager_->allocate_page();
        uint8_t* new_page = pager_->get_page(new_root);

        PageHdr h{};
        h.is_leaf = false;
        h.num_cells = 1;
        h.cell_area_end = Pager::PAGE_SIZE;
        h.right_sibling = 0;
        h.left_sibling = 0;
        h.right_child = split_right_child;
        write_hdr(new_page, h);

        size_t hs = HDR_INT;
        size_t csize = int_cell_size(split_key);
        h.cell_area_end = Pager::PAGE_SIZE - (uint16_t)csize;
        size_t pos = h.cell_area_end;
        w16(new_page + pos, (uint16_t)split_key.size());
        pos += 2;
        memcpy(new_page + pos, split_key.data(), split_key.size());
        pos += split_key.size();
        w32(new_page + pos, root_page_num_);

        set_cell_offset(new_page, 0, h.cell_area_end);
        write_hdr(new_page, h);

        root_page_num_ = new_root;
    }
    return true;
}

// ── insert_impl (recursive) ────────────────────────────────────

bool BTree::insert_impl(uint32_t page_num,
                        const std::vector<uint8_t>& key,
                        const std::vector<uint8_t>& value,
                        std::vector<uint8_t>& split_key,
                        uint32_t& split_right_child) {
    uint8_t* page = pager_->get_page(page_num);
    auto h = read_hdr(page);

    if (h.is_leaf) {
        size_t needed = leaf_cell_size(key, value) + 2;
        if (free_space(page) >= needed) {
            insert_into_leaf(page, key, value);
            return false;
        }

        // Leaf split
        std::vector<std::vector<uint8_t>> keys;
        std::vector<std::vector<uint8_t>> values;
        for (uint16_t i = 0; i < h.num_cells; i++) {
            std::vector<uint8_t> k, v;
            read_leaf_cell(page, i, k, v);
            keys.push_back(std::move(k));
            values.push_back(std::move(v));
        }
        int ins = (int)keys.size();
        for (int i = 0; i < (int)keys.size(); i++) {
            if (key_cmp(key, keys[i]) < 0) { ins = i; break; }
        }
        keys.insert(keys.begin() + ins, key);
        values.insert(values.begin() + ins, value);

        size_t half = keys.size() / 2;

        // Left page (existing, overwrite)
        {
            memset(page, 0, Pager::PAGE_SIZE);
            PageHdr nh{};
            nh.is_leaf = true;
            nh.num_cells = 0;
            nh.cell_area_end = Pager::PAGE_SIZE;
            nh.right_sibling = 0;
            nh.left_sibling = h.left_sibling;
            write_hdr(page, nh);
            for (size_t i = 0; i < half; i++)
                insert_into_leaf(page, keys[i], values[i]);
        }

        // Right page (new)
        uint32_t right = pager_->allocate_page();
        uint8_t* rpage = pager_->get_page(right);
        {
            memset(rpage, 0, Pager::PAGE_SIZE);
            PageHdr nh{};
            nh.is_leaf = true;
            nh.num_cells = 0;
            nh.cell_area_end = Pager::PAGE_SIZE;
            nh.right_sibling = h.right_sibling;
            nh.left_sibling = page_num;
            write_hdr(rpage, nh);
            for (size_t i = half; i < keys.size(); i++)
                insert_into_leaf(rpage, keys[i], values[i]);
        }

        // Link siblings: left page → right page
        {
            auto lh = read_hdr(page);
            lh.right_sibling = right;
            write_hdr(page, lh);
        }
        // If right page has a right sibling, update its left_sibling
        {
            auto rh = read_hdr(rpage);
            if (rh.right_sibling != 0) {
                uint8_t* rrs = pager_->get_page(rh.right_sibling);
                auto rrs_h = read_hdr(rrs);
                rrs_h.left_sibling = right;
                write_hdr(rrs, rrs_h);
            }
        }

        split_key = keys[half];
        split_right_child = right;
        return true;
    } else {
        // ── NODO INTERNO: scendi al figlio giusto ──
        uint32_t child = find_child_page(page, key);

        std::vector<uint8_t> child_split_key;
        uint32_t child_split_right = 0;
        bool child_split = insert_impl(child, key, value,
                                       child_split_key, child_split_right);
        if (!child_split) return false;

        // ── Il figlio si è SPLIT: dobbiamo inserire un separatore ──
        //
        // Quando un figlio si split, abbiamo:
        //   child_split_key = prima chiave del nuovo fratello DESTRO
        //   child_split_right = page_num del nuovo fratello DESTRO
        //
        // Dobbiamo aggiungere una cella (child_split_key, child) in QUESTO nodo,
        // dove child è la pagina ORIGINALE (la metà SINISTRA dello split).
        //
        // ATTENZIONE: prima dello split, il genitore aveva una cella che
        // puntava a 'child' (la pagina originale). Dopo lo split, quella
        // pagina contiene SOLO la metà sinistra. La metà destra è NUOVA.
        //
        // Quindi dobbiamo:
        //   1. AGGIORNARE i vecchi riferimenti a 'child' → child_split_right
        //      (così le chiavi >= child_split_key vanno al nuovo fratello)
        //   2. INSERIRE una NUOVA cella (child_split_key, child)
        //      (così le chiavi < child_split_key vanno ancora alla pagina originale)
        //
        // Questo è l'OPPOSTO di ciò che potrebbe sembrare intuitivo:
        // la NUOVA cella punta alla pagina VECCHIA (sinistra),
        // il VECCHIO riferimento punta alla pagina NUOVA (destra).

        size_t needed = int_cell_size(child_split_key) + 2;
        if (free_space(page) >= needed) {
            // Caso facile: c'è spazio in questo nodo
            h = read_hdr(page);
            // Step 1: aggiorna i vecchi riferimenti
            for (uint16_t i = 0; i < h.num_cells; i++) {
                uint32_t cp;
                std::vector<uint8_t> k;
                read_int_cell(page, i, k, cp);
                if (cp == child)
                    set_int_cell_child(page, i, child_split_right);
            }
            if (h.right_child == child) {
                h.right_child = child_split_right;
                write_hdr(page, h);
            }
            // Step 2: inserisci nuova cella che punta alla pagina ORIGINALE
            insert_into_internal(page, child_split_key, child);
            return false;
        }

        // ── Anche QUESTO nodo deve split ──
        // Raccogli tutte le entry (chiave, figlio) esistenti
        struct Entry { std::vector<uint8_t> key; uint32_t child; };
        std::vector<Entry> entries;
        for (uint16_t i = 0; i < h.num_cells; i++) {
            std::vector<uint8_t> k;
            uint32_t cp;
            read_int_cell(page, i, k, cp);
            entries.push_back({std::move(k), cp});
        }
        // Stessa logica: vecchi riferimenti → nuova pagina destra
        for (auto& e : entries) {
            if (e.child == child)
                e.child = child_split_right;
        }
        if (h.right_child == child)
            h.right_child = child_split_right;
        // Inserisci la nuova entry (puntatore alla pagina ORIGINALE, metà sinistra)
        int ins = (int)entries.size();
        for (int i = 0; i < (int)entries.size(); i++) {
            if (key_cmp(child_split_key, entries[i].key) < 0) {
                ins = i; break;
            }
        }
        entries.insert(entries.begin() + ins,
                       {child_split_key, child});

        size_t half = entries.size() / 2;

        // Left page
        {
            memset(page, 0, Pager::PAGE_SIZE);
            PageHdr nh{};
            nh.is_leaf = false;
            nh.num_cells = (uint16_t)(half - 1);
            nh.cell_area_end = Pager::PAGE_SIZE;
            nh.right_sibling = 0;
            nh.left_sibling = h.left_sibling;
            nh.right_child = entries[half - 1].child;
            write_hdr(page, nh);
            for (uint16_t i = 0; i < half - 1; i++) {
                insert_into_internal(page, entries[i].key, entries[i].child);
            }
        }

        split_key = entries[half - 1].key;

        // Right page (new)
        uint32_t right = pager_->allocate_page();
        uint8_t* rpage = pager_->get_page(right);
        size_t right_entries = entries.size() - half;
        if (right_entries > 0) {
            memset(rpage, 0, Pager::PAGE_SIZE);
            uint16_t right_cell_count = (uint16_t)(right_entries - 1);
            PageHdr rh{};
            rh.is_leaf = false;
            rh.num_cells = right_cell_count;
            rh.cell_area_end = Pager::PAGE_SIZE;
            rh.right_sibling = h.right_sibling;
            rh.left_sibling = page_num;
            rh.right_child = entries.back().child;
            write_hdr(rpage, rh);
            for (uint16_t i = 0; i < right_cell_count; i++) {
                insert_into_internal(rpage,
                                     entries[half + i].key,
                                     entries[half + i].child);
            }
        }

        // Link siblings
        {
            auto lh = read_hdr(page);
            lh.right_sibling = right;
            write_hdr(page, lh);
        }
        {
            auto rh = read_hdr(rpage);
            if (rh.right_sibling != 0) {
                uint8_t* rrs = pager_->get_page(rh.right_sibling);
                auto rrs_h = read_hdr(rrs);
                rrs_h.left_sibling = right;
                write_hdr(rrs, rrs_h);
            }
        }

        split_right_child = right;
        return true;
    }
}

// ── scan_all (DFS) ─────────────────────────────────────────────

void BTree::scan_all(std::function<void(const std::vector<uint8_t>&,
                                         const std::vector<uint8_t>&)> cb) {
    std::function<void(uint32_t)> visit = [&](uint32_t pn) {
        uint8_t* page = pager_->get_page(pn);
        auto h = read_hdr(page);
        if (h.is_leaf) {
            for (uint16_t i = 0; i < h.num_cells; i++) {
                std::vector<uint8_t> k, v;
                read_leaf_cell(page, i, k, v);
                cb(k, v);
            }
        } else {
            for (uint16_t i = 0; i < h.num_cells; i++) {
                uint32_t cp;
                std::vector<uint8_t> k;
                read_int_cell(page, i, k, cp);
                visit(cp);
            }
            visit(h.right_child);
        }
    };
    visit(root_page_num_);
}

// ── cursor ─────────────────────────────────────────────────────

bool BTree::Cursor::seek(const std::vector<uint8_t>& key) {
    stack_.clear();
    valid_ = false;

    uint32_t page_num = tree_->root_page_num_;
    for (;;) {
        uint8_t* page = tree_->pager_->get_page(page_num);
        auto h = tree_->read_hdr(page);
        if (h.is_leaf) {
            int idx = tree_->find_key_in_node(page, key);
            if (idx < h.num_cells) {
                std::vector<uint8_t> k, v;
                tree_->read_leaf_cell(page, idx, k, v);
                if (key_cmp(k, key) >= 0) {
                    stack_.push_back({page_num, idx});
                    valid_ = true;
                }
            }
            return valid_;
        }
        uint32_t child = tree_->find_child_page(page, key);
        stack_.push_back({page_num, 0});  // push placeholder
        page_num = child;
    }
}

bool BTree::Cursor::next() {
    if (!valid_) return false;
    auto& frame = stack_.back();
    uint8_t* page = tree_->pager_->get_page(frame.page);
    auto h = tree_->read_hdr(page);
    if (!h.is_leaf) return false;

    frame.idx++;
    if (frame.idx < h.num_cells) return true;

    // Move to right sibling if available
    if (h.right_sibling != 0) {
        frame.page = h.right_sibling;
        frame.idx = 0;
        uint8_t* rpage = tree_->pager_->get_page(frame.page);
        auto rh = tree_->read_hdr(rpage);
        if (rh.num_cells > 0) return true;
    }

    valid_ = false;
    return false;
}

std::vector<uint8_t> BTree::Cursor::key() {
    if (!valid_) return {};
    auto& frame = stack_.back();
    uint8_t* page = tree_->pager_->get_page(frame.page);
    std::vector<uint8_t> k, v;
    tree_->read_leaf_cell(page, frame.idx, k, v);
    return k;
}

std::vector<uint8_t> BTree::Cursor::value() {
    if (!valid_) return {};
    auto& frame = stack_.back();
    uint8_t* page = tree_->pager_->get_page(frame.page);
    std::vector<uint8_t> k, v;
    tree_->read_leaf_cell(page, frame.idx, k, v);
    return v;
}
