# leanrndb — AGENTS.md

Minimal SQLite-style database server with tables, B-tree indexing, and a SQL parser. Written in C++17, single-file storage.

## Build & test

```sh
cmake -B build && cmake --build build   # build leanrndb + test_basic
./build/test_basic                       # 15 tests: create/insert/select/delete/index/parser/persistence
```

## Run

```sh
./build/leanrndb [dbfile]               # REPL — type SQL or EXIT/QUIT
./build/leanrndb dbfile "SELECT * FROM t WHERE id = 1;"   # one-shot query
```

## Architecture

- **`src/pager.h/cpp`** — reads/writes 4 KB pages, tracks dirty pages, flushes on destructor
- **`src/btree.h/cpp`** — page-based B-tree with leaf splits (no rebalance on delete); keys/values are byte vectors
- **`src/tokenizer.h/cpp`** — recursive-descent SQL tokenizer (keywords → uppercase lookup)
- **`src/parser.h/cpp`** — recursive-descent SQL parser → `std::variant<SelectStmt, InsertStmt, CreateTableStmt, …>`
- **`src/executor.h/cpp`** — wires parsed AST to Database storage
- **`src/database.h/cpp`** — owns Pager, table catalog, B-tree cache; serializes schema to page 0
- **`src/row.h`** — `Value` (int/string/null) + Row serialization (little-endian, self-describing)
- **`src/main.cpp`** — REPL loop, line-buffered until `;`, one-shot with third argv

## SQL supported

```
CREATE TABLE name (col1 TYPE, col2 TYPE, ...)
INSERT INTO name VALUES (val1, val2, ...)
SELECT [col1, … | *] FROM name [WHERE col op val]
DELETE FROM name [WHERE …]
UPDATE name SET col = val WHERE …
DROP TABLE name
CREATE INDEX name ON table (col)
```

Types: `INT`, `VARCHAR`, `TEXT`, `BOOL` (all stored as int or string internally).

## Key constraints

- **B-tree `remove()` does NOT rebalance** — only works on single-leaf trees. Use with care.
- **Cursor `next()` only scans within one leaf** — multi-page range scans not implemented.
- **Catalog stored on page 0** — limited to 4 KB. Schema loss if exceeded.
- **Tokenizer requires semicolon** to finalise statement in REPL.
- **Tests use `tmpnam`** — builds warn; acceptable for test-only usage.

## Roadmap: da REPL a server di rete

Questi passi trasformano il client REPL in un server TCP multi-client.

### Passo 1 — `src/server.h/cpp` listener TCP

- `Server` classe con `socket`, `bind`, `listen`, `accept` su una porta (es. 9120)
- Per ogni connessione: spawna un thread (o thread pool)
- Ogni thread ha il proprio `Database*` (condiviso, con `std::mutex`)

**Nuovo target CMake** opzionale (`leanrndb_server`) o unifica in un eseguibile
con flag `--server --port 9120` nel `main()`.

### Passo 2 — protocollo testuale semplice

Formato richiesta/risposta su TCP:

```
>>> INSERT INTO t VALUES (1, 'ciao');\n
<<< OK INSERT (1)\n
>>> SELECT * FROM t;\n
<<< id | val\n
<<< ---+-----\n
<<< 1 | ciao\n
<<< (1 rows)\n
>>> EXIT\n
<<< Bye.\n
<<< <CONNESSIONE CHIUSA>\n
```

Il server fa da REPL remoto: riceve righe, accumula fino a `;`, esegue,
restituisce il risultato formattato, aspetta la prossima query.

**Estensione futura**: formato binario (length-prefixed messages) per
prestazioni e parsing lato client.

### Passo 3 — mutex sul Database

`Database` non è thread-safe: aggiungere `std::mutex` e `lock_guard`
in ogni metodo pubblico (`insert`, `select`, `create_table`, ecc.),
oppure un unico `std::mutex db_mutex_` avvolto da un helper `lock()`.

### Passo 4 — thread pool

Invece di un thread per connessione (limite ~1024), usare un pool
fisso di worker threads (es. 4–8). Le connessioni in attesa finiscono
in una coda (`std::queue<int> client_fds`).

### Passo 5 — graceful shutdown e config

- `SIGINT` / `SIGTERM` → chiude il socket in ascolto, aspetta i thread,
  fluscia il database, esce pulito
- Opzioni da riga di comando: `--port`, `--db`, `--threads`, `--max-connections`
- File di config opzionale (es. `leanrndb.conf` chiave=valore)

### Passo 6 — logging

Sostituire `std::cout` con un logger minimo:
- `LOG_INFO("accettata connessione da {}", ip)`
- `LOG_ERROR("errore su socket: {}", strerror(errno))`
- `LOG_DEBUG("query: {}", sql)`

Header-only: `src/logger.h` con macro o funzioni template.

### Passo 7 — client CLI dedicato

`leanrndb_cli host port` → si collega via TCP, presenta il prompt
`leanrndb> `, invia SQL al server, stampa la risposta.

Separa nettamente il server (core + rete) dal client (solo I/O testuale).

### Passo 8 (avanzato) — transazioni, recovery, WAL

- File di journal (WAL) per crash recovery
- BEGIN / COMMIT / ROLLBACK via salvataggio undo log in memoria
- B-tree `remove()` con merge (ribilanciamento) per DELETE stabile
- Catalogo con pagine overflow

---

## Dipendenze

- **linenoise** (`external/linenoise/`) — readline replacement per il REPL
  (history con frecce su/giù, editing riga). Compilato come C.
- Per la parte server: **socket POSIX** + `std::thread`. Zero altre dipendenze.

```sh
cmake -B build && cmake --build build   # compila tutto
```
