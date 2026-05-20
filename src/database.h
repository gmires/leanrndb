#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include "pager.h"
#include "btree.h"
#include "row.h"

/**
 * Metadati di una tabella: nome, pagina radice del B-tree dati,
 * schema colonne, prossimo rowid, e mappa degli indici secondari.
 */
struct TableInfo {
    std::string name;
    uint32_t root_page;                       // radice del B-tree dei dati
    std::vector<ColumnSchema> columns;
    int64_t next_rowid = 1;                   // auto-increment

    // Indici: nome_indice → (colonna, pagina_radice_btree_indice)
    std::unordered_map<std::string, std::pair<std::string, uint32_t>> indexes;
};

/**
 * Database: il cuore del motore.
 *
 * Possiede il Pager (file), le TableInfo (metadati in memoria)
 * e una cache di BTree. Salva il catalogo (schema delle tabelle)
 * sulla pagina 0 del file in formato binario.
 *
 * Flusso: chiamate dai metodi di Executor → usano BTree su Pager.
 */
class Database {
public:
    explicit Database(const std::string& path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Operazioni DDL
    bool create_table(const std::string& name,
                      const std::vector<ColumnSchema>& cols);
    bool drop_table(const std::string& name);
    bool create_index(const std::string& index_name,
                      const std::string& table_name,
                      const std::string& col_name);

    // Operazioni DML
    int64_t insert(const std::string& table_name,
                   const std::vector<Value>& values);
    bool select(const std::string& table_name,
                std::vector<Row>& results,
                const std::string& where_col = "",
                const std::string& where_op = "",
                const Value& where_val = Value{});
    bool delete_rows(const std::string& table_name,
                     const std::string& where_col,
                     const std::string& where_op,
                     const Value& where_val,
                     int64_t& affected);
    bool update_rows(const std::string& table_name,
                     const std::string& set_col,
                     const Value& set_val,
                     const std::string& where_col,
                     const std::string& where_op,
                     const Value& where_val,
                     int64_t& affected);

    // Accesso ai metadati e ai B-tree
    TableInfo* find_table(const std::string& name);
    BTree* get_btree(uint32_t root_page);

    Pager* pager() { return &pager_; }

    // Introspection / utility
    std::vector<std::string> table_names() const;
    std::string describe_table(const std::string& name) const;
    std::string table_schema(const std::string& name) const;
    std::string db_summary() const;
    std::vector<std::string> all_index_names() const;

private:
    // Salva/carica il catalogo su/da pagina 0
    void save_catalog();
    void load_catalog();

    Pager pager_;
    std::unordered_map<std::string, TableInfo> tables_;   // metadati in memoria
    std::unordered_map<uint32_t, std::unique_ptr<BTree>> btree_cache_;

    static constexpr uint32_t MAGIC = 0x4C45414E;  // "LEAN" in ASCII
    static constexpr uint32_t HEADER_PAGE = 0;
};
