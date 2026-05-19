#include "parser.h"
#include <stdexcept>

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

// Restituisce il token corrente senza avanzare
Token Parser::peek() const {
    if (pos_ < tokens_.size()) return tokens_[pos_];
    return {TokenType::END, "", 0, 0};
}

// Consuma e restituisce il token corrente
Token Parser::consume() {
    if (pos_ < tokens_.size()) return tokens_[pos_++];
    return {TokenType::END, "", 0, 0};
}

// Consuma un token e verifica che sia del tipo atteso
Token Parser::expect(TokenType type) {
    Token t = consume();
    if (t.type != type) {
        std::string msg = "Expected token type " + std::to_string((int)type) +
                          " but got '" + t.text + "'";
        throw std::runtime_error(msg);
    }
    return t;
}

// Consuma il token solo se è del tipo specificato
bool Parser::match(TokenType type) {
    if (peek().type == type) {
        consume();
        return true;
    }
    return false;
}

// Analizza una definizione di colonna: NOME TIPO [PRIMARY KEY]
ColumnDef Parser::parse_column_def() {
    ColumnDef col;
    Token name = expect(TokenType::IDENTIFIER);
    col.name = name.text;

    Token t = consume();
    std::string upper;
    for (char c : t.text) upper += (char)std::toupper(c);
    if (upper == "INT" || upper == "INTEGER") col.type = "INT";
    else if (upper == "VARCHAR") col.type = "VARCHAR";
    else if (upper == "TEXT") col.type = "TEXT";
    else if (upper == "BOOL" || upper == "BOOLEAN") col.type = "BOOL";
    else col.type = "INT";

    if (peek().type == TokenType::PRIMARY) {
        consume();
        expect(TokenType::KEY);
    }

    return col;
}

// Analizza un valore: NUMBER, STRING, NULL, TRUE, FALSE o IDENTIFIER
Value Parser::parse_value() {
    Token t = consume();
    if (t.type == TokenType::NUMBER)
        return Value(t.int_val);
    if (t.type == TokenType::STRING)
        return Value(t.text);
    if (t.type == TokenType::KW_NULL)
        return Value();
    if (t.type == TokenType::KW_TRUE)
        return Value((int64_t)1);
    if (t.type == TokenType::KW_FALSE)
        return Value((int64_t)0);
    if (t.type == TokenType::IDENTIFIER)
        return Value(t.text);
    throw std::runtime_error("Expected value, got " + t.text);
}

// Analizza una clausola WHERE: WHERE colonna OP valore
WhereClause Parser::parse_where() {
    WhereClause w;
    if (!match(TokenType::WHERE)) return w;

    w.present = true;
    Token col = expect(TokenType::IDENTIFIER);
    w.column = col.text;

    Token op = consume();
    if (op.type == TokenType::EQ) w.op = "=";
    else if (op.type == TokenType::LT) w.op = "<";
    else if (op.type == TokenType::GT) w.op = ">";
    else if (op.type == TokenType::LE) w.op = "<=";
    else if (op.type == TokenType::GE) w.op = ">=";
    else if (op.type == TokenType::NE) w.op = "!=";
    else throw std::runtime_error("Expected comparison op, got " + op.text);

    w.value = parse_value();
    return w;
}

// Analizza uno statement SQL completo: decide il tipo in base al primo token.
Statement Parser::parse() {
    Token t = consume();
    std::string upper;
    for (char c : t.text) upper += (char)std::toupper(c);

    if (upper == "CREATE") {
        // CREATE TABLE ... oppure CREATE INDEX ...
        Token what = consume();
        std::string upper2;
        for (char c : what.text) upper2 += (char)std::toupper(c);

        if (upper2 == "TABLE") {
            CreateTableStmt stmt;
            stmt.table_name = expect(TokenType::IDENTIFIER).text;
            expect(TokenType::LPAREN);
            stmt.columns.push_back(parse_column_def());
            while (peek().type == TokenType::COMMA) {
                consume();
                stmt.columns.push_back(parse_column_def());
            }
            expect(TokenType::RPAREN);
            return stmt;
        } else if (upper2 == "INDEX") {
            CreateIndexStmt stmt;
            stmt.index_name = expect(TokenType::IDENTIFIER).text;
            expect(TokenType::ON);
            stmt.table_name = expect(TokenType::IDENTIFIER).text;
            expect(TokenType::LPAREN);
            stmt.column = expect(TokenType::IDENTIFIER).text;
            expect(TokenType::RPAREN);
            return stmt;
        }
        throw std::runtime_error("Expected TABLE or INDEX after CREATE");
    } else if (upper == "INSERT") {
        // INSERT INTO nome VALUES (val1, val2, ...)
        expect(TokenType::INTO);
        InsertStmt stmt;
        stmt.table_name = expect(TokenType::IDENTIFIER).text;
        expect(TokenType::VALUES);
        expect(TokenType::LPAREN);
        stmt.values.push_back(parse_value());
        while (peek().type == TokenType::COMMA) {
            consume();
            stmt.values.push_back(parse_value());
        }
        expect(TokenType::RPAREN);
        return stmt;
    } else if (upper == "SELECT") {
        // SELECT [col1, ... | *] FROM nome [WHERE ...]
        SelectStmt stmt;
        if (peek().type == TokenType::STAR) {
            consume();
        } else {
            stmt.columns.push_back(expect(TokenType::IDENTIFIER).text);
            while (peek().type == TokenType::COMMA) {
                consume();
                stmt.columns.push_back(expect(TokenType::IDENTIFIER).text);
            }
        }
        expect(TokenType::FROM);
        stmt.table_name = expect(TokenType::IDENTIFIER).text;
        stmt.where = parse_where();
        return stmt;
    } else if (upper == "DELETE") {
        // DELETE FROM nome [WHERE ...]
        expect(TokenType::FROM);
        DeleteStmt stmt;
        stmt.table_name = expect(TokenType::IDENTIFIER).text;
        stmt.where = parse_where();
        return stmt;
    } else if (upper == "DROP") {
        // DROP TABLE nome
        expect(TokenType::TABLE);
        DropTableStmt stmt;
        stmt.table_name = expect(TokenType::IDENTIFIER).text;
        return stmt;
    } else if (upper == "UPDATE") {
        // UPDATE nome SET colonna = valore WHERE ...
        UpdateStmt stmt;
        stmt.table_name = expect(TokenType::IDENTIFIER).text;
        expect(TokenType::SET);
        stmt.set_column = expect(TokenType::IDENTIFIER).text;
        expect(TokenType::EQ);
        stmt.set_value = parse_value();
        stmt.where = parse_where();
        return stmt;
    }

    throw std::runtime_error("Unknown statement: " + t.text);
}
