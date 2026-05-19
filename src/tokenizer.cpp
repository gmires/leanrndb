#include "tokenizer.h"
#include <cctype>
#include <unordered_map>

// Mappa delle parole chiave SQL (in MAIUSCOLO) al rispettivo TokenType.
// Il tokenizer converte ogni parola in maiuscolo per il lookup,
// quindi il SQL è case-insensitive per i keyword.
static const std::unordered_map<std::string, TokenType> keywords = {
    {"CREATE",  TokenType::CREATE},
    {"TABLE",   TokenType::TABLE},
    {"INSERT",  TokenType::INSERT},
    {"INTO",    TokenType::INTO},
    {"VALUES",  TokenType::VALUES},
    {"SELECT",  TokenType::SELECT},
    {"FROM",    TokenType::FROM},
    {"WHERE",   TokenType::WHERE},
    {"AND",     TokenType::AND},
    {"OR",      TokenType::OR},
    {"NOT",     TokenType::NOT},
    {"DELETE",  TokenType::DELETE},
    {"DROP",    TokenType::DROP},
    {"INDEX",   TokenType::INDEX},
    {"ON",      TokenType::ON},
    {"ORDER",   TokenType::ORDER},
    {"BY",      TokenType::BY},
    {"ASC",     TokenType::ASC},
    {"DESC",    TokenType::DESC},
    {"SET",     TokenType::SET},
    {"UPDATE",  TokenType::UPDATE},
    {"INT",     TokenType::KW_INT},
    {"INTEGER", TokenType::KW_INT},
    {"VARCHAR", TokenType::KW_VARCHAR},
    {"TEXT",    TokenType::KW_TEXT},
    {"BOOL",    TokenType::KW_BOOL},
    {"BOOLEAN", TokenType::KW_BOOL},
    {"TRUE",    TokenType::KW_TRUE},
    {"FALSE",   TokenType::KW_FALSE},
    {"NULL",    TokenType::KW_NULL},
    {"PRIMARY", TokenType::PRIMARY},
    {"KEY",     TokenType::KEY},
};

Tokenizer::Tokenizer(std::string input) : input_(std::move(input)) {}

// Salta spazi, tab, newline
void Tokenizer::skip_ws() {
    while (pos_ < input_.size() &&
           (input_[pos_] == ' ' || input_[pos_] == '\t' ||
            input_[pos_] == '\n' || input_[pos_] == '\r'))
        pos_++;
}

// Prossimo token: usa uno switch sul carattere corrente per decidere
// se è un simbolo, un numero, una stringa o un identificatore/keyword.
Token Tokenizer::next_token() {
    skip_ws();
    if (pos_ >= input_.size())
        return {TokenType::END, "", 0, pos_};

    char c = input_[pos_];
    size_t start = pos_;

    // Helper per simboli a 1 carattere
    auto sym = [&](TokenType t) {
        return Token{t, std::string(1, c), 0, start};
    };

    switch (c) {
        case '*': pos_++; return sym(TokenType::STAR);
        case '(': pos_++; return sym(TokenType::LPAREN);
        case ')': pos_++; return sym(TokenType::RPAREN);
        case ',': pos_++; return sym(TokenType::COMMA);
        case ';': pos_++; return sym(TokenType::SEMICOLON);
        case '=': pos_++; return sym(TokenType::EQ);
        case '<':
            if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '=')
                { pos_ += 2; return {TokenType::LE, "<=", 0, start}; }
            if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '>')
                { pos_ += 2; return {TokenType::NE, "<>", 0, start}; }
            pos_++; return sym(TokenType::LT);
        case '>':
            if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '=')
                { pos_ += 2; return {TokenType::GE, ">=", 0, start}; }
            pos_++; return sym(TokenType::GT);
        case '!':
            if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '=')
                { pos_ += 2; return {TokenType::NE, "!=", 0, start}; }
            break;
    }

    // Stringa letterale tra apici semplici: 'testo'
    if (c == '\'') {
        pos_++;
        std::string val;
        while (pos_ < input_.size() && input_[pos_] != '\'') {
            if (input_[pos_] == '\\') pos_++;
            val += input_[pos_++];
        }
        if (pos_ < input_.size()) pos_++;
        return {TokenType::STRING, val, 0, start};
    }

    // Numero intero (inclusi negativi con -)
    if (std::isdigit(c) || (c == '-' && pos_ + 1 < input_.size() &&
                            std::isdigit(input_[pos_ + 1]))) {
        size_t end = pos_;
        if (input_[end] == '-') end++;
        while (end < input_.size() && std::isdigit(input_[end])) end++;
        std::string num_str = input_.substr(pos_, end - pos_);
        pos_ = end;
        int64_t val = std::stoll(num_str);
        return {TokenType::NUMBER, num_str, val, start};
    }

    // Identificatore o keyword: parole alfanumeriche + underscore
    if (std::isalpha(c) || c == '_') {
        while (pos_ < input_.size() &&
               (std::isalnum(input_[pos_]) || input_[pos_] == '_'))
            pos_++;
        std::string word = input_.substr(start, pos_ - start);
        // Converte in maiuscolo per confronto con la mappa keyword
        std::string upper;
        for (char ch : word) upper += (char)std::toupper(ch);

        auto it = keywords.find(upper);
        if (it != keywords.end())
            return {it->second, word, 0, start};
        return {TokenType::IDENTIFIER, word, 0, start};
    }

    // Carattere sconosciuto → errore
    pos_++;
    return {TokenType::ERROR, std::string(1, c), 0, start};
}

Token Tokenizer::next() {
    return next_token();
}

void Tokenizer::tokenize_all() {
    while (true) {
        Token t = next_token();
        tokens_.push_back(t);
        if (t.type == TokenType::END || t.type == TokenType::ERROR)
            break;
    }
}

const std::vector<Token>& Tokenizer::tokens() {
    if (tokens_.empty()) tokenize_all();
    return tokens_;
}
