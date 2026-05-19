#include "executor.h"
#include <sstream>
#include <stdexcept>

// Dispatcher principale: usa std::visit per chiamare il metodo giusto
// in base al tipo di statement contenuto nel variant.
QueryResult Executor::execute(const Statement& stmt) {
    try {
        return std::visit([this](const auto& s) -> QueryResult {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, CreateTableStmt>)
                return execute_create_table(s);
            else if constexpr (std::is_same_v<T, CreateIndexStmt>)
                return execute_create_index(s);
            else if constexpr (std::is_same_v<T, InsertStmt>)
                return execute_insert(s);
            else if constexpr (std::is_same_v<T, SelectStmt>)
                return execute_select(s);
            else if constexpr (std::is_same_v<T, DeleteStmt>)
                return execute_delete(s);
            else if constexpr (std::is_same_v<T, DropTableStmt>)
                return execute_drop_table(s);
            else if constexpr (std::is_same_v<T, UpdateStmt>)
                return execute_update(s);
            else
                return {false, "Unknown statement type"};
        }, stmt);
    } catch (std::exception& e) {
        return {false, e.what()};
    }
}

QueryResult Executor::execute_create_table(const CreateTableStmt& stmt) {
    std::vector<ColumnSchema> cols;
    for (auto& c : stmt.columns)
        cols.push_back({c.name, c.type});

    bool ok = db_->create_table(stmt.table_name, cols);
    if (!ok)
        return {false, "Table '" + stmt.table_name + "' already exists"};
    return {true, "CREATE TABLE"};
}

QueryResult Executor::execute_create_index(const CreateIndexStmt& stmt) {
    bool ok = db_->create_index(stmt.index_name, stmt.table_name, stmt.column);
    if (!ok)
        return {false, "Failed to create index (table or column not found)"};
    return {true, "CREATE INDEX"};
}

QueryResult Executor::execute_drop_table(const DropTableStmt& stmt) {
    bool ok = db_->drop_table(stmt.table_name);
    if (!ok)
        return {false, "Table '" + stmt.table_name + "' not found"};
    return {true, "DROP TABLE"};
}

QueryResult Executor::execute_insert(const InsertStmt& stmt) {
    int64_t rowid = db_->insert(stmt.table_name, stmt.values);
    if (rowid < 0)
        return {false, "Table '" + stmt.table_name + "' not found"};
    QueryResult r;
    r.ok = true;
    r.message = "INSERT";
    r.affected = 1;
    return r;
}

QueryResult Executor::execute_select(const SelectStmt& stmt) {
    QueryResult r;
    auto* table = db_->find_table(stmt.table_name);
    if (!table) {
        r.message = "Table '" + stmt.table_name + "' not found";
        return r;
    }

    // Costruisce i nomi delle colonne del risultato
    if (stmt.columns.empty()) {
        for (auto& col : table->columns)
            r.columns.push_back(col.name);
    } else {
        r.columns = stmt.columns;
    }

    std::vector<Row> rows;
    db_->select(stmt.table_name, rows,
                stmt.where.column, stmt.where.op, stmt.where.value);

    // Proietta solo le colonne richieste (o tutte se SELECT *)
    for (auto& row : rows) {
        std::vector<Value> out;
        if (stmt.columns.empty()) {
            out = row.values;
        } else {
            for (auto& col_name : stmt.columns) {
                int idx = -1;
                for (int i = 0; i < (int)table->columns.size(); i++) {
                    if (table->columns[i].name == col_name) { idx = i; break; }
                }
                if (idx >= 0 && idx < (int)row.values.size())
                    out.push_back(row.values[idx]);
                else
                    out.emplace_back();
            }
        }
        r.rows.push_back(std::move(out));
    }

    r.ok = true;
    r.message = "SELECT";
    r.affected = (int64_t)rows.size();
    return r;
}

QueryResult Executor::execute_delete(const DeleteStmt& stmt) {
    int64_t affected = 0;
    bool ok = db_->delete_rows(stmt.table_name,
                               stmt.where.column, stmt.where.op,
                               stmt.where.value, affected);
    if (!ok)
        return {false, "Table '" + stmt.table_name + "' not found"};
    QueryResult r;
    r.ok = true;
    r.message = "DELETE";
    r.affected = affected;
    return r;
}

QueryResult Executor::execute_update(const UpdateStmt& stmt) {
    int64_t affected = 0;
    bool ok = db_->update_rows(stmt.table_name,
                               stmt.set_column, stmt.set_value,
                               stmt.where.column, stmt.where.op,
                               stmt.where.value, affected);
    if (!ok)
        return {false, "Table '" + stmt.table_name + "' not found"};
    QueryResult r;
    r.ok = true;
    r.message = "UPDATE";
    r.affected = affected;
    return r;
}
