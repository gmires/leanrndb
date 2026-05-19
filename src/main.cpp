#include <iostream>
#include <string>
#include <sstream>
#include <iomanip>
#include "database.h"
#include "tokenizer.h"
#include "parser.h"
#include "executor.h"

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

/**
 * REPL interattivo: legge SQL da stdin riga per riga,
 * accumula fino al punto e virgola, poi tokenizza, analizza ed esegue.
 */
static void run_repl(Database& db) {
    std::cout << "leanrndb v0.1.0 — type SQL statements or EXIT\n";
    std::cout << "> " << std::flush;

    std::string line;
    std::string buffer;

    while (std::getline(std::cin, line)) {
        // Controlla se l'utente vuole uscire (EXIT, QUIT, .exit, .quit)
        std::string trimmed;
        for (char c : line) {
            if (c != ' ' && c != '\t' && c != ';') trimmed += (char)std::toupper(c);
        }
        if (trimmed == "EXIT" || trimmed == "QUIT" || trimmed == ".EXIT" || trimmed == ".QUIT")
            break;

        buffer += line + " ";

        // Il comando è completo solo quando troviamo un punto e virgola
        bool has_semicolon = false;
        for (char c : line) {
            if (c == ';') { has_semicolon = true; break; }
        }

        if (!has_semicolon) {
            std::cout << "> " << std::flush; // prosegue su più righe
            continue;
        }

        try {
            Tokenizer tok(buffer);
            tok.tokenize_all();
            auto toks = tok.tokens();

            if (toks.empty() || toks[0].type == TokenType::END) {
                buffer.clear();
                std::cout << "> " << std::flush;
                continue;
            }

            while (!toks.empty() &&
                   (toks.back().type == TokenType::END ||
                    toks.back().type == TokenType::ERROR))
                toks.pop_back();

            if (toks.empty()) {
                buffer.clear();
                std::cout << "> " << std::flush;
                continue;
            }

            Parser parser(toks);
            Statement stmt = parser.parse();
            Executor exec(&db);
            QueryResult result = exec.execute(stmt);
            print_result(result);
        } catch (std::exception& e) {
            std::cout << "Error: " << e.what() << "\n";
        }

        buffer.clear();
        std::cout << "> " << std::flush;
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
