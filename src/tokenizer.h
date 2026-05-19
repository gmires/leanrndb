#pragma once

#include <string>
#include <vector>

/**
 * Tipi di token riconosciuti dal tokenizer SQL.
 * Ogni keyword SQL ha il suo tipo; gli operatori e i letterali hanno i propri.
 */
enum class TokenType {
    // Keyword SQL
    CREATE, TABLE, INSERT, INTO, VALUES, SELECT, FROM, WHERE,
    AND, OR, NOT, DELETE, DROP, INDEX, ON, ORDER, BY, ASC, DESC,
    SET, UPDATE,
    KW_INT, KW_VARCHAR, KW_TEXT, KW_BOOL, KW_TRUE, KW_FALSE,
    KW_NULL, PRIMARY, KEY,

    // Simboli
    STAR, EQ, LT, GT, LE, GE, NE, LPAREN, RPAREN, COMMA, SEMICOLON,

    // Letterali
    IDENTIFIER, STRING, NUMBER,

    END, ERROR
};

/**
 * Token singolo: tipo, testo originale, valore numerico (se NUMBER), posizione.
 */
struct Token {
    TokenType type;
    std::string text;
    int64_t int_val = 0;
    size_t pos = 0;
};

/**
 * Tokenizer: trasforma una stringa SQL in un array di Token.
 * Funzionamento: scorre la stringa carattere per carattere,
 * riconosce parole chiave (confronto case-insensitive), identificatori,
 * numeri, stringhe tra apici e simboli.
 */
class Tokenizer {
public:
    explicit Tokenizer(std::string input);

    Token next();
    const std::vector<Token>& tokens();
    void tokenize_all();

private:
    void skip_ws();
    Token next_token();

    std::string input_;
    size_t pos_ = 0;
    std::vector<Token> tokens_;
};
