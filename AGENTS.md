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

## Overflow pages

Values too large to fit inline in a 4 KB B-tree page are automatically stored
in a chain of **overflow pages**. This removes the previous ~4 KB row size limit.

- **Sentinel**: `0xFFFFFFFF` in the `value_len` field indicates overflow.
- **Overflow cell**: `[2 key_len][key][4 sentinel][4 total_size][4 first_page]`.
- **Overflow page format**: `[4 next_page][4 data_len][4088 data]`.
- **Transparent**: the B-tree `insert`, `find`, `remove`, `scan_all`, and `Cursor`
  APIs handle overflow internally; callers (Database/Executor) see no difference.
- **Binary search optimized**: `read_leaf_key()` avoids reading overflow values
  during `find_key_in_node()`.
- **No `free_page()` on Pager**: overflow pages from `remove()` or splits are
  leaked (allocated but orphaned). Acceptable for a learning project.
- **Threshold**: a value is stored inline if and only if
  `2 + key.size() + 4 + value.size() <= PAGE_SIZE - HDR_LEAF - 2` (~4075 bytes
  for an 8-byte key). Larger values use overflow automatically.

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

- **`remove()` ora gestisce i merge**, con reindirizzamento delle celle del genitore e pulizia delle celle ridondanti. Tuttavia non c'è logica di redistribuzione — una foglia vuota assorbe un intero fratello, il che può fallire se il fratello non ci sta.
- **`cell_area_end`** non viene aggiornato alla rimozione di una cella. Deve essere resettato a `PAGE_SIZE` prima di fare merge di celle in una pagina vuota (corretto nel codice di merge).
- **Nodi interni genitori** dopo un merge: le celle adiacenti con lo stesso figlio vengono collassate, e l'ultima cella viene rimossa se il suo figlio è uguale a `right_child`. Questo evita che `scan_all` visiti la stessa foglia due volte.
- **Catalog overflow**: supportato tramite la stessa catena di overflow usata dal B-tree.
- **Cursor `next()` scansiona solo una foglia** — scansione multi-pagina non implementata.
- **Il tokenizer richiede il punto e virgola** per finalizzare lo statement nel REPL.
- **I test usano `tmpnam`** — il build avvisa; accettabile per test.

---

## Procedimenti (didattica)

### Navigazione nei nodi interni: `find_child_page`

Ogni cella di un nodo interno contiene:

    cella[i] = (separatore S, puntatore_figlio)

Il separatore `S` è la **prima chiave del fratello destro**. Quindi:
- chiavi **strettamente minori** di `S` → vanno al **figlio sinistro** (cella[i])
- chiavi **maggiori o uguali** a `S` → vanno al **prossimo figlio** (cella[i+1] o `right_child`)

**Perché < e non ≤?** Se usassimo ≤, una chiave uguale al separatore finirebbe in *entrambi* i figli, creando duplicati in `scan_all` e facendo fallire le cancellazioni (una DELETE rimuoverebbe la stessa riga due volte).

La funzione `find_child_page` implementa questa logica: cerca col `find_key_in_node` (binary search), poi scorre in avanti finché non trova un separatore strettamente maggiore della chiave.

### Inserimento con split: la logica contraintuitiva

Quando un figlio si split, succede questo:

1. Il figlio originale viene DIVISO in due pagine: sinistra (originale) e destra (nuova)
2. `child_split_key` = la prima chiave della metà destra
3. `child_split_right` = il numero di pagina della metà destra

Nel genitore dobbiamo fare DUE cose:

1. **AGGIORNARE** i vecchi riferimenti: dove c'era scritto "figlio = pagina originale", ora scriviamo "figlio = child_split_right" (la metà destra)
2. **INSERIRE** una nuova cella `(child_split_key, pagina_originale)` (la metà sinistra)

È l'opposto di ciò che sembra intuitivo:
- La **nuova** cella punta alla pagina **vecchia** (sinistra)
- Il **vecchio** riferimento ora punta alla pagina **nuova** (destra)

### Merge di foglie vuote

Quando una foglia resta senza celle (`num_cells == 0`), invece di lasciarla vuota, assorbiamo le celle del fratello:

1. Resettiamo `cell_area_end = PAGE_SIZE` (fondamentale! vedi sotto)
2. Copiamo TUTTE le celle del fratello nella pagina vuota via `insert_into_leaf`
3. Aggiorniamo i puntatori ai fratelli (destra e sinistra)
4. Liberiamo la pagina del fratello
5. Restituiamo al genitore: `freed_page = fratello`, `survivor_page = noi`

**Problema critico:** la funzione che rimuove una cella (`remove_cell_from_leaf`) NON aggiorna `cell_area_end`. Dopo molte cancellazioni, `cell_area_end` rimane al punto più basso raggiunto durante gli inserimenti. Se facciamo un merge senza resettarlo, `insert_into_leaf` usa quel valore stantio per posizionare le nuove celle, scrivendole in mezzo alla pagina e corrompendo l'header. Soluzione: resettare `cell_area_end = PAGE_SIZE` prima di iniziare il merge.

### Pulizia del genitore dopo un merge

Dopo un merge, il genitore ha ricevuto `freed_page = X` e `survivor_page = Y`. Aggiorna tutti i puntatori da X a Y. Ma ora potremmo avere:

    cella[0] = (sep=58, figlio=Y)
    cella[1] = (sep=115, figlio=Y)   ← appena aggiornato

Entrambe le celle puntano allo stesso figlio Y. `scan_all` visiterebbe Y **due volte**, producendo duplicati. La soluzione: tra celle adiacenti con lo stesso figlio, teniamo solo l'**ultima** (quella col separatore più alto).

Inoltre, se dopo l'aggiornamento anche `right_child == Y` (stesso valore dell'ultima cella), anche quella cella è ridondante: tutte le chiavi vanno a Y sia via cella che via `right_child`. Anche in questo caso la rimuoviamo.

### Split di foglie

Quando una foglia non ha spazio per una nuova cella:

1. Raccogliamo tutte le celle esistenti + la nuova
2. Le dividiamo a metà: le prime metà nella pagina sinistra (originale), le seconde nella pagina destra (nuova)
3. Colleghiamo le due pagine come fratelli (left_sibling / right_sibling)
4. Restituiamo al genitore: `split_key` = prima chiave della metà destra, `split_right_child` = pagina della metà destra

Il genitore poi inserirà `split_key` come separatore (vedi "Inserimento con split" sopra).

### Riduzione dell'altezza

Se dopo una rimozione la radice è un nodo interno con 0 celle, l'albero viene appiattito: la nuova radice diventa `right_child`. Questo succede automaticamente in `BTree::remove()` e mantiene l'albero bilanciato.

---

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
