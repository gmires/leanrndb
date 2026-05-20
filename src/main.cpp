#include <iostream>
#include <string>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cctype>
#include <algorithm>
#include "database.h"
#include "tokenizer.h"
#include "parser.h"
#include "executor.h"
#include "linenoise.h"

/**
 * Stampa un risultato di query in formato tabellare (per SELECT)
 * o come messaggio (per INSERT/CREATE/DELETE/...).
 */
static void print_result(const QueryResult& r) {
    if (!r.ok) {
        std::cout << "Error: " << r.message << "\n";
        return;
    }

    if (r.message == "SELECT" && !r.rows.empty()) {
        // Intestazione colonne
        for (size_t i = 0; i < r.columns.size(); i++) {
            if (i > 0) std::cout << " | ";
            std::cout << r.columns[i];
        }
        std::cout << "\n";

        // Separatore
        for (size_t i = 0; i < r.columns.size(); i++) {
            if (i > 0) std::cout << "-+-";
            for (size_t j = 0; j < r.columns[i].size(); j++)
                std::cout << "-";
        }
        std::cout << "\n";

        // Righe
        for (auto& row : r.rows) {
            for (size_t i = 0; i < row.size(); i++) {
                if (i > 0) std::cout << " | ";
                std::cout << row[i].to_string();
            }
            std::cout << "\n";
        }
        std::cout << "(" << r.affected << " rows)\n";
    } else if (r.affected > 0) {
        std::cout << r.message << " (" << r.affected << ")\n";
    } else {
        std::cout << r.message << "\n";
    }
}

// ── dot-command handler ─────────────────────────────────────────

// Gestisce i comandi che iniziano con '.' (stile SQLite).
// Restituisce false se il comando richiede di uscire.
static bool handle_dot_command(Database& db, std::string line) {
    // Rende il comando case-insensitive
    std::string lower;
    for (char c : line) lower += (char)std::tolower(c);

    std::istringstream iss(lower);
    std::string cmd;
    iss >> cmd;

    if (cmd == ".exit" || cmd == ".quit")
        return false;

    if (cmd == ".help") {
        std::cout <<
            ".tables              list all tables\n"
            ".schema [table]      show CREATE statement for table(s)\n"
            ".describe <table>    show column types and indexes\n"
            ".head <table> [N]    show first N rows (default 10)\n"
            ".count <table>       count rows\n"
            ".indices [table]     list indexes\n"
            ".dbinfo              database statistics\n"
            ".help                this help\n"
            ".exit / .quit        exit leanrndb\n";
        return true;
    }

    if (cmd == ".tables") {
        auto names = db.table_names();
        if (names.empty()) {
            std::cout << "(no tables)\n";
        } else {
            for (auto& n : names) std::cout << n << "\n";
        }
        return true;
    }

    if (cmd == ".schema") {
        std::string name;
        iss >> name;
        if (name.empty()) {
            auto names = db.table_names();
            for (auto& n : names)
                std::cout << db.table_schema(n) << "\n";
        } else {
            std::cout << db.table_schema(name) << "\n";
        }
        return true;
    }

    if (cmd == ".describe") {
        std::string name;
        iss >> name;
        if (name.empty()) {
            std::cout << "Usage: .describe <table>\n";
        } else {
            std::cout << db.describe_table(name);
        }
        return true;
    }

    if (cmd == ".count") {
        std::string name;
        iss >> name;
        if (name.empty()) {
            std::cout << "Usage: .count <table>\n";
        } else {
            std::vector<Row> rows;
            if (db.select(name, rows))
                std::cout << rows.size() << "\n";
            else
                std::cout << "Table not found: " << name << "\n";
        }
        return true;
    }

    if (cmd == ".head") {
        std::string name;
        int n = 10;
        iss >> name;
        if (!iss.eof()) iss >> n;
        if (name.empty()) {
            std::cout << "Usage: .head <table> [N]\n";
        } else {
            std::vector<Row> rows;
            if (!db.select(name, rows)) {
                std::cout << "Table not found: " << name << "\n";
                return true;
            }
            int shown = 0;
            for (auto& row : rows) {
                if (shown >= n) break;
                for (size_t i = 0; i < row.values.size(); i++) {
                    if (i > 0) std::cout << " | ";
                    std::cout << row.values[i].to_string();
                }
                std::cout << "\n";
                shown++;
            }
            if (shown < (int)rows.size())
                std::cout << "(" << rows.size() << " rows, showing " << n << ")\n";
        }
        return true;
    }

    if (cmd == ".indices") {
        std::string name;
        iss >> name;
        if (!name.empty()) {
            auto* t = db.find_table(name);
            if (!t) {
                std::cout << "Table not found: " << name << "\n";
                return true;
            }
            for (auto& [idx_name, idx_info] : t->indexes)
                std::cout << idx_name << " ON " << idx_info.first << "\n";
        } else {
            auto names = db.table_names();
            for (auto& tname : names) {
                auto* t = db.find_table(tname);
                if (!t) continue;
                for (auto& [idx_name, idx_info] : t->indexes)
                    std::cout << tname << "." << idx_name
                              << " ON " << idx_info.first << "\n";
            }
        }
        return true;
    }

    if (cmd == ".dbinfo") {
        std::cout << db.db_summary();
        return true;
    }

    std::cout << "Unknown command: " << line
              << ". Type .help for available commands.\n";
    return true;
}

/**
 * REPL interattivo con linenoise: history, frecce, editing.
 * Accumula righe fino al punto e virgola, poi esegue.
 */
static void run_repl(Database& db) {
    linenoiseHistorySetMaxLen(100);
    linenoiseHistoryLoad(".leanrndb_history");

    std::cout << "leanrndb v0.1.0 — type SQL or .help\n";

    std::string buffer;

    while (true) {
        const char* prompt = buffer.empty() ? "> " : "... ";
        char* raw = linenoise(prompt);
        if (raw == nullptr) // EOF (Ctrl+D) o errore
            break;

        std::string line(raw);
        linenoiseFree(raw);
        if (line.empty()) continue;

        // Dot-commands: handle immediatamente (non si accumulano)
        if (line[0] == '.') {
            if (!handle_dot_command(db, line))
                break;  // .exit / .quit
            continue;
        }

        // Controlla exit
        std::string trimmed;
        for (char c : line) {
            if (c != ' ' && c != '\t' && c != ';')
                trimmed += (char)std::toupper(c);
        }
        if (trimmed == "EXIT" || trimmed == "QUIT")
            break;

        buffer += line + " ";

        // Completo solo se c'è un punto e virgola nella riga corrente
        if (line.find(';') == std::string::npos)
            continue;

        // Aggiunge all'history (solo comandi completi, non vuoti)
        linenoiseHistoryAdd(buffer.c_str());
        linenoiseHistorySave(".leanrndb_history");

        try {
            Tokenizer tok(buffer);
            tok.tokenize_all();
            auto toks = tok.tokens();

            while (!toks.empty() &&
                   (toks.back().type == TokenType::END ||
                    toks.back().type == TokenType::ERROR))
                toks.pop_back();

            if (!toks.empty()) {
                Parser parser(toks);
                Statement stmt = parser.parse();
                Executor exec(&db);
                QueryResult result = exec.execute(stmt);
                print_result(result);
            }
        } catch (std::exception& e) {
            std::cout << "Error: " << e.what() << "\n";
        }

        buffer.clear();
    }
    std::cout << "Bye.\n";
}

int main(int argc, char* argv[]) {
    std::string db_path = "leanrndb.db";
    if (argc > 1)
        db_path = argv[1];

    try {
        Database db(db_path);

        if (argc > 2) {
            // Modalità one-shot: esegue il SQL passato come terzo argomento
            std::string sql = argv[2];
            Tokenizer tok(sql);
            tok.tokenize_all();
            auto toks = tok.tokens();
            while (!toks.empty() &&
                   (toks.back().type == TokenType::END ||
                    toks.back().type == TokenType::ERROR))
                toks.pop_back();

            Parser parser(toks);
            Statement stmt = parser.parse();
            Executor exec(&db);
            QueryResult result = exec.execute(stmt);
            print_result(result);
        } else {
            // Modalità interattiva
            run_repl(db);
        }
    } catch (std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
