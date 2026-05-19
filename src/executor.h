#pragma once

#include <string>
#include <vector>
#include "parser.h"
#include "database.h"

/**
 * Risultato di una query: ok/flago, messaggio, colonne, righe, righe modificate.
 */
struct QueryResult {
    bool ok = false;
    std::string message;
    std::vector<std::string> columns;
    std::vector<std::vector<Value>> rows;
    int64_t affected = 0;
};

/**
 * Executor: collega il parser al database.
 *
 * Prende uno Statement (AST) e lo esegue chiamando i metodi di Database.
 * Separa la logica di parsing da quella di esecuzione.
 */
class Executor {
public:
    explicit Executor(Database* db) : db_(db) {}

    /** Esegue uno statement e restituisce il risultato. */
    QueryResult execute(const Statement& stmt);

private:
    // Metodi privati per ogni tipo di statement
    QueryResult execute_select(const SelectStmt& stmt);
    QueryResult execute_insert(const InsertStmt& stmt);
    QueryResult execute_create_table(const CreateTableStmt& stmt);
    QueryResult execute_create_index(const CreateIndexStmt& stmt);
    QueryResult execute_drop_table(const DropTableStmt& stmt);
    QueryResult execute_delete(const DeleteStmt& stmt);
    QueryResult execute_update(const UpdateStmt& stmt);

    Database* db_;
};
