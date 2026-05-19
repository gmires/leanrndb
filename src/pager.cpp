#include "pager.h"

// Apre il file in lettura+scrittura. Se non esiste, lo crea.
// Poi carica in memoria tutte le pagine esistenti (ognuna 4 KB).
Pager::Pager(const std::string& path) : path_(path) {
    file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_.is_open()) {
        // File inesistente → lo creiamo
        file_.clear();
        file_.open(path_, std::ios::out | std::ios::binary);
        file_.close();
        file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
        if (!file_.is_open())
            return;
    }

    // Legge il file esistente: quante pagine ci sono?
    file_.seekg(0, std::ios::end);
    size_t file_size = (size_t)file_.tellg();
    size_t num_pages = file_size / PAGE_SIZE;

    for (size_t i = 0; i < num_pages; i++) {
        std::vector<uint8_t> page(PAGE_SIZE, 0);
        file_.seekg((std::streamoff)(i * PAGE_SIZE));
        file_.read(reinterpret_cast<char*>(page.data()), PAGE_SIZE);
        pages_.push_back(std::move(page));
        dirty_.push_back(false);  // appena caricata = non sporca
    }
}

Pager::~Pager() { flush_all(); }

// Restituisce un puntatore al buffer della pagina.
// Se la pagina non era ancora stata caricata, la crea vuota.
uint8_t* Pager::get_page(uint32_t page_num) {
    ensure_page(page_num);
    return pages_[page_num].data();
}

// Alloca una nuova pagina (tutti zeri) e restituisce il suo numero.
// Viene marcata come sporca perché la scriveremo al flush.
uint32_t Pager::allocate_page() {
    uint32_t num = (uint32_t)pages_.size();
    pages_.emplace_back(PAGE_SIZE, 0);
    dirty_.push_back(true);
    return num;
}

// Se la pagina è sporca, la riscrive su disco nel punto giusto del file.
void Pager::flush_page(uint32_t page_num) {
    if (page_num >= pages_.size() || !dirty_[page_num])
        return;
    file_.seekp((std::streamoff)(page_num * PAGE_SIZE));
    file_.write(reinterpret_cast<const char*>(pages_[page_num].data()), PAGE_SIZE);
    file_.flush();
    dirty_[page_num] = false;
}

// Sfiora tutte le pagine sporche su disco.
void Pager::flush_all() {
    for (uint32_t i = 0; i < (uint32_t)pages_.size(); i++)
        flush_page(i);
}

// Se la pagina richiesta non è in memoria, la crea vuota finché non esiste.
void Pager::ensure_page(uint32_t page_num) {
    while (page_num >= pages_.size()) {
        pages_.emplace_back(PAGE_SIZE, 0);
        dirty_.push_back(true);
    }
}
