#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <fstream>

/**
 * Pager: gestore di pagine su file.
 *
 * Suddivide il file in blocchi da 4 KB (PAGE_SIZE) e li mantiene in memoria.
 * Tiene traccia delle pagine "sporche" (modificate) per scriverle su disco
 * quando necessario. È lo strato più basso del database: tutto ciò che sta
 * sopra di lui vede solo pagine in memoria.
 */
class Pager {
public:
    static constexpr uint32_t PAGE_SIZE = 4096;

    /** Apre (o crea) il file e carica tutte le pagine esistenti in memoria. */
    explicit Pager(const std::string& path);
    /** Distruttore: scrive su disco tutte le pagine sporche. */
    ~Pager();

    Pager(const Pager&) = delete;
    Pager& operator=(const Pager&) = delete;

    /** Restituisce un puntatore alla pagina numero `page_num` (0-based). */
    uint8_t* get_page(uint32_t page_num);
    /** Alloca una nuova pagina (vuota) e ne restituisce il numero (riusa pagine libere se disponibili). */
    uint32_t allocate_page();
    /** Libera una pagina (viene azzerata e riutilizzata da allocate_page). */
    void free_page(uint32_t page_num);
    /** Scrive su disco la pagina `page_num` se è sporca. */
    void flush_page(uint32_t page_num);
    /** Scrive su disco tutte le pagine sporche. */
    void flush_all();
    /** Numero totale di pagine caricate. */
    uint32_t num_pages() const { return (uint32_t)pages_.size(); }

private:
    /** Assicura che la pagina `page_num` esista in memoria (la crea se necessario). */
    void ensure_page(uint32_t page_num);

    std::string path_;
    std::fstream file_;
    std::vector<std::vector<uint8_t>> pages_;
    std::vector<bool> dirty_;
    std::vector<uint32_t> free_pages_;
};
