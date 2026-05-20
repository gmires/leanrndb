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

// ── dimensioni intestazione pagina ─────────────────────────────

// L'intestazione di una foglia occupa 5 byte (tipo, num_celle, cell_area_end).
// Quella di un nodo interno è 9 byte (aggiunge right_child).
static constexpr size_t HDR_LEAF = 5;
static constexpr size_t HDR_INT  = 9;

// ── overflow pages ─────────────────────────────────────────────
// Quando il valore di una cella è troppo grande per stare inline nella pagina,
// si scrive un cella speciale con value_len = OVERFLOW_SENTINEL,
// seguita da total_size (4B) e first_overflow_page (4B).
// I dati vero vengono messi in una catena di pagine overflow.
static constexpr uint32_t OVERFLOW_SENTINEL = 0xFFFFFFFF;
// Quanti byte di dati utente entrano in una pagina overflow (4 KB - 8 byte di overhead)
static constexpr size_t OVERFLOW_PAGE_DATA = Pager::PAGE_SIZE - 8;

// Una cella inline non può mai superare lo spazio disponibile in una pagina
// foglia appena inizializzata: PAGE_SIZE - HDR_LEAF - 2 (per il primo cell ptr)
static inline bool is_overflow(const std::vector<uint8_t>& key,
                               const std::vector<uint8_t>& value) {
    return (2 + key.size() + 4 + value.size()) > (Pager::PAGE_SIZE - HDR_LEAF - 2);
}

// ── PageHdr: lettura/scrittura dell'intestazione di pagina ──────

BTree::PageHdr BTree::read_hdr(const uint8_t* page) {
    // byte 0: 0 = interno, 1 = foglia
    PageHdr h{};
    h.is_leaf   = (page[0] != 0);
    h.num_cells = r16(page + 1);   // byte 1-2: numero di celle
    h.cell_area_end = r16(page + 3); // byte 3-4: inizio area celle (cresce dal fondo)
    if (!h.is_leaf)
        h.right_child = r32(page + 5); // byte 5-8: figlio destro (solo interni)
    return h;
}

void BTree::write_hdr(uint8_t* page, const PageHdr& h) {
    page[0] = h.is_leaf ? 1 : 0;
    w16(page + 1, h.num_cells);
    w16(page + 3, h.cell_area_end);
    if (!h.is_leaf)
        w32(page + 5, h.right_child);
}

// Quanto è grande l'intestazione in base al tipo di nodo
size_t BTree::hdr_size(bool is_leaf) {
    return is_leaf ? HDR_LEAF : HDR_INT;
}

// ── array degli offset alle celle ───────────────────────────────

// Ogni cella ha un offset di 2 byte, memorizzato dopo l'intestazione.
// L'offset indica la posizione nella pagina da cui partono i dati della cella.

uint16_t BTree::cell_offset(const uint8_t* page, uint16_t idx) {
    size_t hs = (page[0] != 0) ? HDR_LEAF : HDR_INT;
    return r16(page + hs + idx * 2);
}

void BTree::set_cell_offset(uint8_t* page, uint16_t idx, uint16_t off) {
    size_t hs = (page[0] != 0) ? HDR_LEAF : HDR_INT;
    w16(page + hs + idx * 2, off);
}

// ── dimensioni celle ────────────────────────────────────────────

// Cella foglia: [2 key_len][key][4 value_len][value]
// Se il valore è troppo grande, usa il formato overflow:
//   [2 key_len][key][4 sentinel][4 total_size][4 first_overflow_page]
size_t BTree::leaf_cell_size(const std::vector<uint8_t>& key,
                             const std::vector<uint8_t>& value) {
    if (is_overflow(key, value))
        return 2 + key.size() + 12; // sentinel + total_size + first_page
    return 2 + key.size() + 4 + value.size();
}

// Cella interna: [2 key_len][key][4 child_page]
size_t BTree::int_cell_size(const std::vector<uint8_t>& key) {
    return 2 + key.size() + 4;
}

// Spazio libero nella pagina:
//   cell_area_end - (intestazione + celle * 2)
// Se è <= 0 non c'è spazio.
size_t BTree::free_space(const uint8_t* page) {
    auto h = read_hdr(page);
    size_t hs = hdr_size(h.is_leaf);
    size_t used_by_ptrs = h.num_cells * 2;
    size_t top = hs + used_by_ptrs;
    if (h.cell_area_end <= top) return 0;
    return h.cell_area_end - top;
}

// ── lettura celle ───────────────────────────────────────────────

// Legge SOLO la chiave da una cella foglia (utile per binary search,
// evita di leggere il valore, specialmente se su pagine overflow).
void BTree::read_leaf_key(const uint8_t* page, uint16_t idx,
                          std::vector<uint8_t>& key) {
    uint16_t off = cell_offset(page, idx);
    uint16_t ks  = r16(page + off);
    key.assign(page + off + 2, page + off + 2 + ks);
}

// Legge una cella foglia data la posizione idx: estrae chiave e valore.
// Se la cella usa overflow, ricostruisce il valore dalle pagine overflow.
void BTree::read_leaf_cell(const uint8_t* page, uint16_t idx,
                           std::vector<uint8_t>& key,
                           std::vector<uint8_t>& value) {
    uint16_t off = cell_offset(page, idx);
    uint16_t ks  = r16(page + off);   // key size
    key.assign(page + off + 2, page + off + 2 + ks);
    uint32_t vs  = r32(page + off + 2 + ks); // value size o sentinel

    if (vs == OVERFLOW_SENTINEL) {
        uint32_t total_size = r32(page + off + 2 + ks + 4);
        uint32_t first_page = r32(page + off + 2 + ks + 8);
        read_overflow_pages(first_page, total_size, value);
    } else {
        value.assign(page + off + 2 + ks + 4,
                     page + off + 2 + ks + 4 + vs);
    }
}

// Legge una cella interna: estrae chiave e pagina del figlio.
void BTree::read_int_cell(const uint8_t* page, uint16_t idx,
                          std::vector<uint8_t>& key,
                          uint32_t& child_page) {
    uint16_t off = cell_offset(page, idx);
    uint16_t ks  = r16(page + off);
    key.assign(page + off + 2, page + off + 2 + ks);
    child_page   = r32(page + off + 2 + ks);
}

// ── confronto lessicografico tra chiavi (byte-array) ────────────

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

// ── ricerca binaria in un nodo ──────────────────────────────────

// Cerca la posizione (slot) in cui una chiave dovrebbe trovarsi nel nodo.
// Usa binary search tra le celle del nodo.
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
            uint32_t cp;
            read_int_cell(page, mid, k, cp);
        }
        if (key_cmp(key, k) <= 0)
            hi = mid;      // chiave <= chiave_cella → vai a sinistra
        else
            lo = mid + 1;  // chiave > chiave_cella → vai a destra
    }
    return lo;
}

// ── scrittura celle ─────────────────────────────────────────────

// Inserisce una cella in un nodo foglia: scrive i dati in fondo
// all'area celle e aggiorna l'array degli offset.
// Se il valore è troppo grande, usa il formato overflow.
void BTree::insert_into_leaf(uint8_t* page,
                             const std::vector<uint8_t>& key,
                             const std::vector<uint8_t>& value) {
    auto h = read_hdr(page);
    size_t csize = leaf_cell_size(key, value);
    size_t hs = hdr_size(true);

    // Sposta il bordo dell'area celle più in alto (cresce dal basso)
    uint16_t new_end = h.cell_area_end - (uint16_t)csize;

    // Scrive la cella alla nuova posizione
    size_t pos = new_end;
    w16(page + pos, (uint16_t)key.size());
    pos += 2;
    memcpy(page + pos, key.data(), key.size());
    pos += key.size();

    if (is_overflow(key, value)) {
        // Formato overflow: sentinel + total_size + first_overflow_page
        w32(page + pos, OVERFLOW_SENTINEL);
        pos += 4;
        w32(page + pos, (uint32_t)value.size());
        pos += 4;
        uint32_t first_page = write_overflow_pages(value);
        w32(page + pos, first_page);
    } else {
        // Formato inline: value_len + value_data
        w32(page + pos, (uint32_t)value.size());
        pos += 4;
        memcpy(page + pos, value.data(), value.size());
    }

    // Trova lo slot giusto e sposta gli offset esistenti per fare spazio
    int slot = find_key_in_node(page, key);
    memmove(page + hs + (slot + 1) * 2,
            page + hs + slot * 2,
            (h.num_cells - slot) * 2);
    set_cell_offset(page, slot, new_end);

    h.num_cells++;
    h.cell_area_end = new_end;
    write_hdr(page, h);
}

// Come insert_into_leaf ma per nodi interni. Invece del valore,
// memorizza il numero di pagina del figlio destro associato alla chiave.
void BTree::insert_into_internal(uint8_t* page,
                                 const std::vector<uint8_t>& key,
                                 uint32_t right_child_page) {
    auto h = read_hdr(page);
    size_t csize = int_cell_size(key);
    size_t hs = hdr_size(false);

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

// Scrive un valore su una catena di pagine overflow.
// Formato pagina: [4 next_page][4 data_len][4088 dati].
// Restituisce il numero della prima pagina overflow.
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

        w32(page, 0);        // next = 0 (temporaneo)
        w32(page + 4, (uint32_t)chunk);
        memcpy(page + 8, data.data() + offset, chunk);

        if (prev_page != 0) {
            // Aggancia la nuova pagina alla precedente
            uint8_t* prev = pager_->get_page(prev_page);
            w32(prev, page_num);
        }

        prev_page = page_num;
        offset += chunk;
        remaining -= chunk;
    }

    return first_page;
}

// Legge un valore da una catena di pagine overflow.
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

// Libera le pagine overflow associate a un valore.
// NOTA: Pager non ha free_page(), quindi le pagine rimangono allocate
// e lo spazio non viene riutilizzato. È un memory leak noto.
void BTree::free_overflow_pages(uint32_t first_page) {
    uint32_t page_num = first_page;
    while (page_num != 0) {
        uint8_t* page = pager_->get_page(page_num);
        uint32_t next = r32(page);
        // Le pagine overflow rimangono allocate (nessun free_page disponibile)
        page_num = next;
    }
}

// ── costruttore ─────────────────────────────────────────────────

BTree::BTree(Pager* pager, uint32_t root_page_num)
    : pager_(pager), root_page_num_(root_page_num) {
    // Se la pagina radice è nuova (tutti zeri), la inizializza come foglia vuota
    uint8_t* page = pager_->get_page(root_page_num_);
    auto h = read_hdr(page);
    if (h.num_cells == 0 && h.cell_area_end == 0) {
        h.is_leaf = true;
        h.num_cells = 0;
        h.cell_area_end = Pager::PAGE_SIZE;
        h.right_child = 0;
        write_hdr(page, h);
    }
}

// ── ricerca ─────────────────────────────────────────────────────

// Cerca una chiave percorrendo l'albero dalla radice fino a una foglia.
// In ogni nodo usa binary search per decidere da che figlio proseguire.
bool BTree::find(const std::vector<uint8_t>& key,
                 std::vector<uint8_t>& value_out) {
    uint32_t page_num = root_page_num_;
    for (;;) {
        uint8_t* page = pager_->get_page(page_num);
        auto h = read_hdr(page);
        int idx = find_key_in_node(page, key);
        if (h.is_leaf) {
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
        // Nodo interno: sceglie il figlio giusto
        if (idx < h.num_cells) {
            uint32_t cp;
            std::vector<uint8_t> k;
            read_int_cell(page, idx, k, cp);
            page_num = cp;
        } else {
            page_num = h.right_child;
        }
    }
}

// ── rimozione (semplificata) ────────────────────────────────────

// Rimuove una chiave solo su alberi a foglia singola (nessun ribilanciamento).
// Per alberi con più livelli restituisce false.
// Libera le eventuali pagine overflow del valore rimosso.
bool BTree::remove(const std::vector<uint8_t>& key) {
    uint8_t* page = pager_->get_page(root_page_num_);
    auto h = read_hdr(page);
    if (!h.is_leaf) return false;

    int idx = find_key_in_node(page, key);
    if (idx >= h.num_cells) return false;

    // Legge solo la chiave per verificare il match (evita overflow read)
    uint16_t off = cell_offset(page, idx);
    uint16_t ks = r16(page + off);
    std::vector<uint8_t> cell_key(page + off + 2, page + off + 2 + ks);
    if (key_cmp(cell_key, key) != 0) return false;

    // Se la cella ha overflow pages, le libera
    uint32_t vs = r32(page + off + 2 + ks);
    if (vs == OVERFLOW_SENTINEL) {
        uint32_t first_page = r32(page + off + 2 + ks + 8);
        free_overflow_pages(first_page);
    }

    // Sposta gli offset successivi verso l'inizio per chiudere il buco
    size_t hs = hdr_size(true);
    memmove(page + hs + idx * 2,
            page + hs + (idx + 1) * 2,
            (h.num_cells - idx - 1) * 2);
    h.num_cells--;
    write_hdr(page, h);
    return true;
}

// ── inserimento ─────────────────────────────────────────────────

// Inserisce (chiave, valore) nell'albero.
// Se la radice si divide (perché piena), crea una nuova radice.
bool BTree::insert(const std::vector<uint8_t>& key,
                   const std::vector<uint8_t>& value) {
    std::vector<uint8_t> split_key;
    uint32_t split_right_child = 0;
    bool split = insert_impl(root_page_num_, key, value,
                             split_key, split_right_child);
    if (split) {
        // La radice si è divisa: serve una nuova radice che diventa interna
        // e punta alle due metà (sinistra = vecchia radice, destra = nuova pagina)
        uint32_t new_root = pager_->allocate_page();
        uint8_t* new_page = pager_->get_page(new_root);

        PageHdr h{};
        h.is_leaf = false;
        h.num_cells = 1;
        h.cell_area_end = Pager::PAGE_SIZE;
        h.right_child = split_right_child;
        write_hdr(new_page, h);

        // La cella contiene la chiave separatore e punta alla vecchia radice (figlio sinistro)
        size_t hs = hdr_size(false);
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

// Inserimento ricorsivo.
// Restituisce true se il nodo si è diviso, con split_key = chiave separatore
// e split_right_child = pagina del nuovo figlio destro.
bool BTree::insert_impl(uint32_t page_num,
                        const std::vector<uint8_t>& key,
                        const std::vector<uint8_t>& value,
                        std::vector<uint8_t>& split_key,
                        uint32_t& split_right_child) {
    uint8_t* page = pager_->get_page(page_num);
    auto h = read_hdr(page);

    if (h.is_leaf) {
        // ── inserimento in una foglia ──
        size_t needed = leaf_cell_size(key, value) + 2; // cella + offset
        if (free_space(page) >= needed) {
            insert_into_leaf(page, key, value);
            return false; // nessuno split
        }

        // La foglia è piena: serve dividerla in due
        // 1) Raccogli tutte le celle esistenti + quella nuova
        std::vector<std::vector<uint8_t>> keys;
        std::vector<std::vector<uint8_t>> values;
        for (uint16_t i = 0; i < h.num_cells; i++) {
            std::vector<uint8_t> k, v;
            read_leaf_cell(page, i, k, v);
            keys.push_back(std::move(k));
            values.push_back(std::move(v));
        }
        // Inserisce la nuova chiave in ordine
        int ins = (int)keys.size();
        for (int i = 0; i < (int)keys.size(); i++) {
            std::vector<uint8_t> kk;
            if (key_cmp(key, keys[i]) < 0) { ins = i; break; }
        }
        keys.insert(keys.begin() + ins, key);
        values.insert(values.begin() + ins, value);

        size_t half = keys.size() / 2;  // punto di taglio

        // Pagina sinistra (quella esistente, sovrascritta)
        {
            memset(page, 0, Pager::PAGE_SIZE);
            PageHdr nh{};
            nh.is_leaf = true;
            nh.num_cells = 0;
            nh.cell_area_end = Pager::PAGE_SIZE;
            write_hdr(page, nh);
            for (size_t i = 0; i < half; i++)
                insert_into_leaf(page, keys[i], values[i]);
        }

        // Pagina destra (nuova)
        uint32_t right = pager_->allocate_page();
        uint8_t* rpage = pager_->get_page(right);
        {
            memset(rpage, 0, Pager::PAGE_SIZE);
            PageHdr nh{};
            nh.is_leaf = true;
            nh.num_cells = 0;
            nh.cell_area_end = Pager::PAGE_SIZE;
            write_hdr(rpage, nh);
            for (size_t i = half; i < keys.size(); i++)
                insert_into_leaf(rpage, keys[i], values[i]);
        }

        // La chiave separatore è la prima della pagina destra
        split_key = keys[half];
        split_right_child = right;
        return true; // segnala al padre che deve inserire il separatore
    } else {
        // ── inserimento in un nodo interno ──
        // Trova il figlio giusto per questa chiave
        int idx = find_key_in_node(page, key);
        uint32_t child;
        if (idx < h.num_cells) {
            uint32_t cp;
            std::vector<uint8_t> k;
            read_int_cell(page, idx, k, cp);
            child = cp;
        } else {
            child = h.right_child;
        }

        std::vector<uint8_t> child_split_key;
        uint32_t child_split_right = 0;
        bool child_split = insert_impl(child, key, value,
                                       child_split_key, child_split_right);
        if (!child_split) return false; // il figlio non si è diviso

        // Il figlio si è diviso: dobbiamo inserire il separatore in questo nodo
        size_t needed = int_cell_size(child_split_key) + 2;
        if (free_space(page) >= needed) {
            insert_into_internal(page, child_split_key, child_split_right);
            return false;
        }

        // Anche questo nodo interno è pieno: va diviso
        struct Entry { std::vector<uint8_t> key; uint32_t child; };
        std::vector<Entry> entries;
        for (uint16_t i = 0; i < h.num_cells; i++) {
            std::vector<uint8_t> k;
            uint32_t cp;
            read_int_cell(page, i, k, cp);
            entries.push_back({std::move(k), cp});
        }
        // Inserisce la nuova entry in ordine
        int ins = (int)entries.size();
        for (int i = 0; i < (int)entries.size(); i++) {
            if (key_cmp(child_split_key, entries[i].key) < 0) {
                ins = i; break;
            }
        }
        entries.insert(entries.begin() + ins,
                       {child_split_key, child_split_right});

        size_t half = entries.size() / 2;

        // Ricostruisce la pagina sinistra (quella esistente)
        {
            memset(page, 0, Pager::PAGE_SIZE);
            PageHdr nh{};
            nh.is_leaf = false;
            nh.num_cells = (uint16_t)(half - 1);
            nh.cell_area_end = Pager::PAGE_SIZE;
            // right_child della pagina sinistra = ultima entry del gruppo sinistro
            nh.right_child = entries[half - 1].child;
            write_hdr(page, nh);

            for (uint16_t i = 0; i < half - 1; i++) {
                insert_into_internal(page, entries[i].key, entries[i].child);
            }
        }

        // La chiave separatore da promuovere
        split_key = entries[half - 1].key;

        // Crea la pagina destra
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
            rh.right_child = entries.back().child;
            write_hdr(rpage, rh);

            for (uint16_t i = 0; i < right_cell_count; i++) {
                insert_into_internal(rpage,
                                     entries[half + i].key,
                                     entries[half + i].child);
            }
        }

        split_right_child = right;
        return true;
    }

    return false;
}

// ── scansione totale (DFS) ──────────────────────────────────────

// Visita tutte le foglie in ordine e chiama cb per ogni coppia (chiave, valore).
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
            // Nodo interno: visita i figli in ordine
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

// ── cursore per scansione ordinata ──────────────────────────────

// Cerca una chiave e posiziona il cursore sulla prima cella >= key.
bool BTree::Cursor::seek(const std::vector<uint8_t>& key) {
    stack_.clear();
    valid_ = false;

    uint32_t page_num = tree_->root_page_num_;
    for (;;) {
        uint8_t* page = tree_->pager_->get_page(page_num);
        auto h = tree_->read_hdr(page);
        int idx = tree_->find_key_in_node(page, key);
        if (h.is_leaf) {
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
        uint32_t child;
        if (idx < h.num_cells) {
            uint32_t cp;
            std::vector<uint8_t> k;
            tree_->read_int_cell(page, idx, k, cp);
            child = cp;
        } else {
            child = h.right_child;
        }
        stack_.push_back({page_num, idx});
        page_num = child;
    }
}

// Avanza alla cella successiva nella stessa foglia.
// Al momento non attraversa i confini tra foglie (limitato).
bool BTree::Cursor::next() {
    if (!valid_) return false;
    auto& frame = stack_.back();
    uint8_t* page = tree_->pager_->get_page(frame.page);
    auto h = tree_->read_hdr(page);
    if (!h.is_leaf) return false;

    frame.idx++;
    if (frame.idx < h.num_cells) return true;

    // Pagina esaurita
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
