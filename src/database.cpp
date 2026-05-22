#include "database.h"
#include <cstring>
#include <sstream>
#include <algorithm>
#include <stdexcept>

// ── helper di serializzazione ────────────────────────────────────

// Scrive uint32_t in little-endian in un vector di byte
static void w32_bytes(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back((uint8_t)(v & 0xFF));
    buf.push_back((uint8_t)((v >> 8) & 0xFF));
    buf.push_back((uint8_t)((v >> 16) & 0xFF));
    buf.push_back((uint8_t)((v >> 24) & 0xFF));
}

// Scrive uint32_t little-endian in un buffer raw ad un dato puntatore
static void w32_bytes_raw(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

// Legge uint32_t little-endian da un buffer a un dato offset
static uint32_t r32_at(const uint8_t* p, size_t off) {
    return (uint32_t)p[off] | ((uint32_t)p[off+1] << 8) |
           ((uint32_t)p[off+2] << 16) | ((uint32_t)p[off+3] << 24);
}

// Scrive uint16_t in little-endian in un vector
static void w16_bytes(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back((uint8_t)(v & 0xFF));
    buf.push_back((uint8_t)((v >> 8) & 0xFF));
}

// ── encoding delle chiavi per il B-tree ──────────────────────────

// Converte un rowid int64 in 8 byte big-endian per il B-tree.
// Big-endian garantisce che l'ordinamento lessicografico coincida
// con l'ordinamento numerico.
static std::vector<uint8_t> rowid_key(int64_t rowid) {
    std::vector<uint8_t> k(8);
    uint64_t v = (uint64_t)rowid;
    for (int i = 7; i >= 0; i--) {
        k[i] = (uint8_t)(v & 0xFF);
        v >>= 8;
    }
    return k;
}

// Decodifica un rowid da 8 byte big-endian
static int64_t key_rowid(const std::vector<uint8_t>& k) {
    uint64_t v = 0;
    for (size_t i = 0; i < 8 && i < k.size(); i++) {
        v = (v << 8) | k[i];
    }
    return (int64_t)v;
}

// Codifica una stringa come chiave per indice: [4 byte lunghezza big-endian][dati].
// Il prefisso di lunghezza garantisce che stringhe di diversa lunghezza
// vengano ordinate correttamente.
static std::vector<uint8_t> str_key(const std::string& s) {
    std::vector<uint8_t> k;
    uint32_t len = (uint32_t)s.size();
    for (int i = 3; i >= 0; i--)
        k.push_back((uint8_t)((len >> (i * 8)) & 0xFF));
    for (char c : s) k.push_back((uint8_t)c);
    return k;
}

// ── costruttore / distruttore ────────────────────────────────────

Database::Database(const std::string& path) : pager_(path) {
    load_catalog();  // carica lo schema dalla pagina 0 se esiste
}

Database::~Database() {
    save_catalog();  // salva lo schema su pagina 0 prima di chiudere
}

// Restituisce (o crea in cache) un BTree per una data pagina radice.
BTree* Database::get_btree(uint32_t root_page) {
    auto it = btree_cache_.find(root_page);
    if (it != btree_cache_.end())
        return it->second.get();
    auto bt = std::make_unique<BTree>(&pager_, root_page);
    auto* ptr = bt.get();
    btree_cache_[root_page] = std::move(bt);
    return ptr;
}

// ── serializzazione catalogo (pagina 0) ──────────────────────────
//
// Formato inline (backward compatible):
//   [4 byte] MAGIC ("LEAN")
//   [4 byte] num_tables
//   per ogni tabella: [...]
//
// Formato overflow (quando data.size() > PAGE_SIZE - 8):
//   [4 byte] MAGIC ("LEAN")
//   [4 byte] 0xFFFFFFFF (OVERFLOW_SENTINEL)
//   [4 byte] total_size (dimensione dei dati serializzati)
//   [4 byte] first_page (prima pagina overflow)
//
// I dati su overflow pages hanno lo stesso formato inline:
//   [4 byte] num_tables
//   per ogni tabella: [...]

static constexpr uint32_t CATALOG_OVERFLOW = 0xFFFFFFFF;
static constexpr size_t OVERFLOW_PAGE_DATA = Pager::PAGE_SIZE - 8;

// Scrive dati su una catena di pagine overflow. Restituisce la prima pagina.
static uint32_t write_catalog_overflow(Pager& pager, const std::vector<uint8_t>& data) {
    size_t offset = 0;
    size_t remaining = data.size();
    uint32_t first_page = 0;
    uint32_t prev_page = 0;

    while (remaining > 0) {
        uint32_t page_num = pager.allocate_page();
        uint8_t* page = pager.get_page(page_num);

        if (first_page == 0) first_page = page_num;

        size_t chunk = remaining > OVERFLOW_PAGE_DATA ? OVERFLOW_PAGE_DATA : remaining;
        w32_bytes_raw(page, 0);        // next = 0 (temporaneo)
        w32_bytes_raw(page + 4, (uint32_t)chunk);
        memcpy(page + 8, data.data() + offset, chunk);

        if (prev_page != 0) {
            uint8_t* prev = pager.get_page(prev_page);
            w32_bytes_raw(prev, page_num);
        }

        prev_page = page_num;
        offset += chunk;
        remaining -= chunk;
    }
    return first_page;
}

// Legge dati da una catena di pagine overflow.
static std::vector<uint8_t> read_catalog_overflow(Pager& pager, uint32_t first_page,
                                                    uint32_t total_size) {
    std::vector<uint8_t> out;
    out.reserve(total_size);

    uint32_t page_num = first_page;
    while (page_num != 0) {
        uint8_t* page = pager.get_page(page_num);
        uint32_t next = r32_at(page, 0);
        uint32_t data_len = r32_at(page, 4);
        out.insert(out.end(), page + 8, page + 8 + data_len);
        page_num = next;
    }
    return out;
}

void Database::save_catalog() {
    uint8_t* page = pager_.get_page(HEADER_PAGE);
    memset(page, 0, Pager::PAGE_SIZE);

    // Formato dati: [4 MAGIC][4 num_tables][tables...] (stesso di sempre)
    std::vector<uint8_t> data;
    w32_bytes(data, MAGIC);

    uint32_t num_tables = (uint32_t)tables_.size();
    w32_bytes(data, num_tables);

    for (auto& [name, info] : tables_) {
        w16_bytes(data, (uint16_t)name.size());
        for (char c : name) data.push_back((uint8_t)c);
        w32_bytes(data, info.root_page);
        w32_bytes(data, (uint32_t)info.next_rowid);

        w16_bytes(data, (uint16_t)info.columns.size());
        for (auto& col : info.columns) {
            w16_bytes(data, (uint16_t)col.name.size());
            for (char c : col.name) data.push_back((uint8_t)c);
            w16_bytes(data, (uint16_t)col.type.size());
            for (char c : col.type) data.push_back((uint8_t)c);
        }

        w16_bytes(data, (uint16_t)info.indexes.size());
        for (auto& [idx_name, idx_info] : info.indexes) {
            w16_bytes(data, (uint16_t)idx_name.size());
            for (char c : idx_name) data.push_back((uint8_t)c);
            w16_bytes(data, (uint16_t)idx_info.first.size());
            for (char c : idx_info.first) data.push_back((uint8_t)c);
            w32_bytes(data, idx_info.second);
        }
    }

    if (data.size() <= Pager::PAGE_SIZE) {
        // Inline: copia tutto su page 0 (stesso formato di sempre)
        memcpy(page, data.data(), data.size());
    } else {
        // Overflow: page 0 contiene solo MAGIC + indice overflow
        // page[0-3] = MAGIC
        // page[4-7] = CATALOG_OVERFLOW sentinel
        // page[8-11] = total_size (intero data escluso MAGIC)
        // page[12-15] = first_overflow_page
        // I dati (esclusi i primi 4 byte MAGIC) vanno su overflow pages
        memcpy(page, data.data(), 4);  // MAGIC
        w32_bytes_raw(page + 4, CATALOG_OVERFLOW);
        uint32_t payload_size = (uint32_t)(data.size() - 4);  // exclude MAGIC
        w32_bytes_raw(page + 8, payload_size);
        std::vector<uint8_t> payload(data.begin() + 4, data.end());
        uint32_t first_page = write_catalog_overflow(pager_, payload);
        w32_bytes_raw(page + 12, first_page);
    }
    pager_.flush_page(HEADER_PAGE);
}

void Database::load_catalog() {
    uint8_t* page = pager_.get_page(HEADER_PAGE);
    uint32_t magic = r32_at(page, 0);
    if (magic != MAGIC) return;

    uint32_t field4 = r32_at(page, 4);
    std::vector<uint8_t> buf;

    if (field4 == CATALOG_OVERFLOW) {
        uint32_t payload_size = r32_at(page, 8);
        uint32_t first_page = r32_at(page, 12);
        buf = read_catalog_overflow(pager_, first_page, payload_size);
    } else {
        // Inline: field4 è num_tables, dati seguono da offset 4
        // Ricostruisce buf = [4 num_tables][resto del catalogo]
        buf.resize(Pager::PAGE_SIZE - 4);
        memcpy(buf.data(), page + 4, Pager::PAGE_SIZE - 4);
    }

    // Parse buf: [4 num_tables][tables...]
    size_t pos = 0;
    auto read_u16 = [&]() -> uint16_t {
        uint16_t v = (uint16_t)buf[pos] | ((uint16_t)buf[pos+1] << 8);
        pos += 2;
        return v;
    };
    auto read_u32 = [&]() -> uint32_t {
        uint32_t v = (uint32_t)buf[pos] | ((uint32_t)buf[pos+1] << 8) |
                     ((uint32_t)buf[pos+2] << 16) | ((uint32_t)buf[pos+3] << 24);
        pos += 4;
        return v;
    };
    auto read_str = [&]() -> std::string {
        uint16_t len = read_u16();
        std::string s((const char*)buf.data() + pos, len);
        pos += len;
        return s;
    };

    uint32_t num_tables = read_u32();
    for (uint32_t t = 0; t < num_tables; t++) {
        if (pos >= buf.size()) break;
        TableInfo info;
        info.name = read_str();
        info.root_page = read_u32();
        info.next_rowid = (int64_t)read_u32();

        uint16_t num_cols = read_u16();
        for (uint16_t c = 0; c < num_cols; c++) {
            ColumnSchema col;
            col.name = read_str();
            col.type = read_str();
            info.columns.push_back(std::move(col));
        }

        uint16_t num_idxs = read_u16();
        for (uint16_t i = 0; i < num_idxs; i++) {
            std::string idx_name = read_str();
            std::string col_name = read_str();
            uint32_t idx_root = read_u32();
            info.indexes[idx_name] = {col_name, idx_root};
        }

        tables_[info.name] = std::move(info);
    }
}

// ── operazioni sulle tabelle ─────────────────────────────────────

// Crea una nuova tabella: alloca una pagina radice, la inizializza
// come foglia vuota, e registra i metadati.
bool Database::create_table(const std::string& name,
                            const std::vector<ColumnSchema>& cols) {
    if (tables_.count(name)) return false;

    TableInfo info;
    info.name = name;
    info.root_page = pager_.allocate_page();
    info.columns = cols;
    info.next_rowid = 1;

    uint8_t* page = pager_.get_page(info.root_page);
    memset(page, 0, Pager::PAGE_SIZE);
    page[0] = 1; // is_leaf = true
    page[1] = 0; page[2] = 0;
    page[3] = (uint8_t)(Pager::PAGE_SIZE & 0xFF);
    page[4] = (uint8_t)((Pager::PAGE_SIZE >> 8) & 0xFF);

    tables_[name] = std::move(info);
    save_catalog();
    return true;
}

// Elimina una tabella (solo metadati; le pagine restano orfane nel file).
bool Database::drop_table(const std::string& name) {
    auto it = tables_.find(name);
    if (it == tables_.end()) return false;
    tables_.erase(it);
    save_catalog();
    return true;
}

// Crea un indice su una colonna: alloca una pagina, la inizializza,
// e popola l'indice scandendo tutti i dati esistenti.
bool Database::create_index(const std::string& index_name,
                            const std::string& table_name,
                            const std::string& col_name) {
    auto* table = find_table(table_name);
    if (!table) return false;
    if (table->indexes.count(index_name)) return false;

    bool found = false;
    for (auto& col : table->columns) {
        if (col.name == col_name) { found = true; break; }
    }
    if (!found) return false;

    uint32_t idx_root = pager_.allocate_page();
    uint8_t* page = pager_.get_page(idx_root);
    memset(page, 0, Pager::PAGE_SIZE);
    page[0] = 1;
    page[1] = 0; page[2] = 0;
    page[3] = (uint8_t)(Pager::PAGE_SIZE & 0xFF);
    page[4] = (uint8_t)((Pager::PAGE_SIZE >> 8) & 0xFF);

    table->indexes[index_name] = {col_name, idx_root};

    // Popola l'indice con i dati già presenti
    BTree* idx_btree = get_btree(idx_root);
    BTree* table_btree = get_btree(table->root_page);
    std::vector<Row> rows;
    table_btree->scan_all([&](const std::vector<uint8_t>& k,
                               const std::vector<uint8_t>& v) {
        Row row = deserialize_row(v, table->columns);
        row.rowid = key_rowid(k);
        rows.push_back(std::move(row));
    });

    int col_idx = -1;
    for (int i = 0; i < (int)table->columns.size(); i++) {
        if (table->columns[i].name == col_name) { col_idx = i; break; }
    }

    for (auto& row : rows) {
        std::string val_str = row.values[col_idx].to_string();
        auto idx_key = str_key(val_str);
        idx_btree->insert(idx_key, rowid_key(row.rowid));
    }

    save_catalog();
    return true;
}

TableInfo* Database::find_table(const std::string& name) {
    auto it = tables_.find(name);
    if (it == tables_.end()) return nullptr;
    return &it->second;
}

// ── insert ──────────────────────────────────────────────────────

// Inserisce una riga: assegna un rowid auto-incrementante,
// serializza il record, lo inserisce nel B-tree della tabella,
// e aggiorna tutti gli indici secondari.
int64_t Database::insert(const std::string& table_name,
                         const std::vector<Value>& values) {
    auto* table = find_table(table_name);
    if (!table) return -1;

    int64_t rowid = table->next_rowid++;
    Row row;
    row.rowid = rowid;
    row.values = values;

    auto key = rowid_key(rowid);
    auto val = serialize_row(row);
    BTree* bt = get_btree(table->root_page);
    bt->insert(key, val);

    // Aggiorna gli indici
    for (auto& [idx_name, idx_info] : table->indexes) {
        auto& [col_name, idx_root] = idx_info;
        BTree* idx_bt = get_btree(idx_root);

        int col_idx = -1;
        for (int i = 0; i < (int)table->columns.size(); i++) {
            if (table->columns[i].name == col_name) { col_idx = i; break; }
        }
        if (col_idx >= 0 && col_idx < (int)values.size()) {
            std::string val_str = values[col_idx].to_string();
            idx_bt->insert(str_key(val_str), key);
        }
    }

    save_catalog();
    return rowid;
}

// ── select ──────────────────────────────────────────────────────

// Seleziona righe con filtro WHERE opzionale.
// Se esiste un indice sulla colonna del WHERE e l'operatore è "=",
// usa l'indice per una ricerca puntuale. Altrimenti scansione totale.
bool Database::select(const std::string& table_name,
                      std::vector<Row>& results,
                      const std::string& where_col,
                      const std::string& where_op,
                      const Value& where_val) {
    auto* table = find_table(table_name);
    if (!table) return false;

    BTree* bt = get_btree(table->root_page);

    // Cerca un indice utilizzabile
    bool use_index = false;
    uint32_t idx_root = 0;
    if (!where_col.empty() && !where_op.empty()) {
        for (auto& [idx_name, idx_info] : table->indexes) {
            if (idx_info.first == where_col && where_op == "=") {
                use_index = true;
                idx_root = idx_info.second;
                break;
            }
        }
    }

    if (use_index) {
        BTree* idx_bt = get_btree(idx_root);
        std::vector<uint8_t> lookup_key = str_key(where_val.to_string());
        std::vector<uint8_t> rowid_bytes;
        if (idx_bt->find(lookup_key, rowid_bytes)) {
            std::vector<uint8_t> k = rowid_key(key_rowid(rowid_bytes));
            std::vector<uint8_t> v;
            if (bt->find(k, v)) {
                Row row = deserialize_row(v, table->columns);
                row.rowid = key_rowid(k);
                results.push_back(std::move(row));
            }
        }
    return true;
}

    // Scansione completa (full scan) con filtro
    bt->scan_all([&](const std::vector<uint8_t>& k,
                      const std::vector<uint8_t>& v) {
        Row row = deserialize_row(v, table->columns);
        row.rowid = key_rowid(k);

        if (!where_col.empty()) {
            int col_idx = -1;
            for (int i = 0; i < (int)table->columns.size(); i++) {
                if (table->columns[i].name == where_col) {
                    col_idx = i; break;
                }
            }
            if (col_idx < 0 || col_idx >= (int)row.values.size()) return;

            bool match = false;
            int64_t rv = 0;
            if (row.values[col_idx].type == Value::INT_VAL)
                rv = row.values[col_idx].int_val;

            int64_t wv = 0;
            if (where_val.type == Value::INT_VAL)
                wv = where_val.int_val;

            if (where_op == "=")
                match = (row.values[col_idx].to_string() == where_val.to_string());
            else if (where_op == "!=")
                match = (row.values[col_idx].to_string() != where_val.to_string());
            else if (where_op == "<") match = (rv < wv);
            else if (where_op == ">") match = (rv > wv);
            else if (where_op == "<=") match = (rv <= wv);
            else if (where_op == ">=") match = (rv >= wv);

            if (!match) return;
        }

        results.push_back(std::move(row));
    });

    return true;
}

// ── delete ──────────────────────────────────────────────────────

// Cancella righe che soddisfano il WHERE: prima le seleziona,
// poi le rimuove una per una dal B-tree.
bool Database::delete_rows(const std::string& table_name,
                           const std::string& where_col,
                           const std::string& where_op,
                           const Value& where_val,
                           int64_t& affected) {
    auto* table = find_table(table_name);
    if (!table) return false;

    std::vector<Row> rows;
    select(table_name, rows, where_col, where_op, where_val);
    affected = 0;

    BTree* bt = get_btree(table->root_page);
    for (auto& row : rows) {
        auto key = rowid_key(row.rowid);
        bt->remove(key);
        affected++;
    }
    return true;
}

// ── update ──────────────────────────────────────────────────────

// Aggiorna righe che soddisfano il WHERE: cancella e reinserisce
// ogni riga con il nuovo valore.
bool Database::update_rows(const std::string& table_name,
                           const std::string& set_col,
                           const Value& set_val,
                           const std::string& where_col,
                           const std::string& where_op,
                           const Value& where_val,
                           int64_t& affected) {
    auto* table = find_table(table_name);
    if (!table) return false;

    std::vector<Row> rows;
    select(table_name, rows, where_col, where_op, where_val);
    affected = 0;

    int set_idx = -1;
    for (int i = 0; i < (int)table->columns.size(); i++) {
        if (table->columns[i].name == set_col) { set_idx = i; break; }
    }
    if (set_idx < 0) return false;

    BTree* bt = get_btree(table->root_page);
    for (auto& row : rows) {
        bt->remove(rowid_key(row.rowid));
        row.values[set_idx] = set_val;
        bt->insert(rowid_key(row.rowid), serialize_row(row));
        affected++;
    }
    return true;
}

// ── introspection ───────────────────────────────────────────────

std::vector<std::string> Database::table_names() const {
    std::vector<std::string> names;
    for (auto& [name, _] : tables_)
        names.push_back(name);
    return names;
}

std::string Database::describe_table(const std::string& name) const {
    auto it = tables_.find(name);
    if (it == tables_.end())
        return "Table not found: " + name;

    auto& t = it->second;
    std::ostringstream out;
    out << "      table: " << t.name << "\n";
    out << "  root page: " << t.root_page << "\n";
    out << "  next rowid: " << t.next_rowid << "\n";
    out << "    columns:\n";
    for (auto& col : t.columns)
        out << "    " << col.name << "  " << col.type << "\n";
    if (!t.indexes.empty()) {
        out << "    indexes:\n";
        for (auto& [idx_name, idx_info] : t.indexes)
            out << "      " << idx_name << " ON " << idx_info.first
                << " (root:" << idx_info.second << ")\n";
    }
    return out.str();
}

std::string Database::table_schema(const std::string& name) const {
    auto it = tables_.find(name);
    if (it == tables_.end())
        return "Table not found: " + name;

    auto& t = it->second;
    std::ostringstream out;
    out << "CREATE TABLE " << t.name << " (";
    for (size_t i = 0; i < t.columns.size(); i++) {
        if (i > 0) out << ", ";
        out << t.columns[i].name << " " << t.columns[i].type;
    }
    out << ");\n";
    for (auto& [idx_name, idx_info] : t.indexes)
        out << "CREATE INDEX " << idx_name << " ON " << t.name
            << " (" << idx_info.first << ");\n";
    return out.str();
}

std::string Database::db_summary() const {
    std::ostringstream out;
    out << "      tables: " << tables_.size() << "\n";
    out << "       pages: " << pager_.num_pages() << "\n";
    out << "   page size: " << Pager::PAGE_SIZE << "\n";
    return out.str();
}

std::vector<std::string> Database::all_index_names() const {
    std::vector<std::string> result;
    for (auto& [tname, table] : tables_) {
        for (auto& [idx_name, _] : table.indexes)
            result.push_back(tname + "." + idx_name);
    }
    return result;
}
