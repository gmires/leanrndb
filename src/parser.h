#pragma once

#include <string>
#include <vector>
#include <variant>
#include <memory>
#include "tokenizer.h"
#include "row.h"

// ── strutture dati per l'AST (Abstract Syntax Tree) ─────────────

/** Definizione di una colonna nella clausola CREATE TABLE. */
struct ColumnDef {
    std::string name;
    std::string type;
};

/** Clausola WHERE: colonna, operatore di confronto e valore. */
struct WhereClause {
    std::string column;
    std::string op;
    Value value;
    bool present = false; // true se la clausola WHERE è stata specificata
};

/** Statement SQL: SELECT */
struct SelectStmt {
    std::vector<std::string> columns; // vuoto = SELECT *
    std::string table_name;
    WhereClause where;
};

/** Statement SQL: INSERT INTO ... VALUES ... */
struct InsertStmt {
    std::string table_name;
    std::vector<Value> values;
};

/** Statement SQL: CREATE TABLE */
struct CreateTableStmt {
    std::string table_name;
    std::vector<ColumnDef> columns;
};

/** Statement SQL: CREATE INDEX */
struct CreateIndexStmt {
    std::string index_name;
    std::string table_name;
    std::string column;
};

/** Statement SQL: DROP TABLE */
struct DropTableStmt {
    std::string table_name;
};

/** Statement SQL: DELETE FROM */
struct DeleteStmt {
    std::string table_name;
    WhereClause where;
};

/** Statement SQL: UPDATE ... SET ... WHERE ... */
struct UpdateStmt {
    std::string table_name;
    std::string set_column;
    Value set_value;
    WhereClause where;
};

/** Uno statement SQL può essere uno qualsiasi dei tipi sopra. */
using Statement = std::variant<
    SelectStmt, InsertStmt, CreateTableStmt,
    CreateIndexStmt, DropTableStmt, DeleteStmt, UpdateStmt>;

/**
 * Parser: analisi sintattica ricorsiva-discendente.
 *
 * Prende un array di Token (dal Tokenizer) e costruisce l'AST
 * corrispondente. Riconosce la grammatica SQL supportata.
 */
class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    /** Analizza i token e restituisce uno Statement. */
    Statement parse();

private:
    // Utilità per navigare tra i token
    Token peek() const;     // guarda il prossimo token senza consumarlo
    Token consume();        // consuma e restituisce il prossimo token
    Token expect(TokenType type); // consuma un token del tipo atteso (errore se diverso)
    bool match(TokenType type);   // consuma solo se corrisponde, restituisce esito

    // Metodi di parsing per i sotto-grammatiche
    ColumnDef parse_column_def();
    Value parse_value();
    WhereClause parse_where();

    size_t pos_ = 0;
    std::vector<Token> tokens_;
};
