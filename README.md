# leanrndb — un database minimo in C++

> **Scopo didattico**: capire come funziona un database SQL-like
> leggendo e modificando il codice. ~500 righe di C++17, zero dipendenze.

## Architettura a strati

```
┌─────────────────────────────────────────────────────────────┐
│  main.cpp          REPL interattivo + one-shot CLI          │
├─────────────────────────────────────────────────────────────┤
│  parser.h/cpp      Trasforma token → AST (albero sintattico)│
│  tokenizer.h/cpp   Trasforma SQL → token                    │
├─────────────────────────────────────────────────────────────┤
│  executor.h/cpp    Collega parser al database               │
│  database.h/cpp    Operazioni su tabelle, righe, indici     │
├─────────────────────────────────────────────────────────────┤
│  btree.h/cpp       B-tree: indici ordinati su pagine        │
│  pager.h/cpp       Gestione pagine 4KB su file              │
├─────────────────────────────────────────────────────────────┤
│  row.h             Value, Row, serializzazione binaria       │
└─────────────────────────────────────────────────────────────┘
```

Il flusso di una query SQL è:

```
SQL "SELECT * FROM t WHERE id = 1"
  → Tokenizer: lo divide in token (SELECT, *, FROM, t, WHERE, id, =, 1)
  → Parser: costruisce un SelectStmt
  → Executor: chiama Database::select()
  → Database cerca l'indice su "id" o fa full scan
  → BTree::find() o BTree::scan_all() naviga le pagine
  → Pager::get_page() legge i blocchi 4KB dal file
```

---

## 1. Pager — lo strato più basso

**File**: `src/pager.h`, `src/pager.cpp`

Il Pager gestisce il file del database dividendolo in **pagine da 4 KB**.
Quando chiedi `get_page(3)`, lui carica dal file i byte 12288–16383
e te li restituisce come puntatore a un buffer in memoria.

**Concetti chiave:**

- **Pagine sporche** (`dirty_`): quando modifichi una pagina in memoria,
  il Pager segna il flag. Al `flush()`, riscrive solo le pagine modificate.
  Senza questo trucco, dovresti riscrivere tutto il file ogni volta.
- **Allocazione**: `allocate_page()` aggiunge una pagina in coda al file.
  Il numero di pagina è la posizione: pagina 0 = byte 0–4095,
  pagina 1 = byte 4096–8191, ecc.

> **Perché 4 KB?** È la dimensione standard delle pagine dei filesystem
> (page size del disco). SQLite usa la stessa dimensione di default.

Il costruttore apre il file in lettura+scrittura (`std::fstream`).
Se il file non esiste, lo crea. Poi carica in memoria **tutte** le pagine
esistenti — per un database piccolo va bene, per uno grande no.

---

## 2. B-tree — l'indice universale

**File**: `src/btree.h`, `src/btree.cpp`

Tutto in leanrndb è un B-tree:
- Le **tabelle** sono B-tree dove la chiave è il rowid (8 byte big-endian)
  e il valore è la riga serializzata.
- Gli **indici** sono B-tree dove la chiave è il valore della colonna
  indicizzata e il valore è il rowid.

### Formato di una pagina B-tree

Ogni pagina (4 KB) è un nodo dell'albero. L'intestazione dice:

- `is_leaf`: è una foglia o un nodo interno?
- `num_cells`: quante celle contiene
- `cell_area_end`: da dove partono i dati delle celle (crescono dal fondo pagina)
- `right_child` (solo interni): figlio destro per chiavi > ultimo separatore

```
┌──────────────────────────────────────────────────┐
│  Header (5 o 9 byte)                              │
│  [is_leaf][num_cells][cell_area_end][right_child] │
├──────────────────────────────────────────────────┤
│  Offset array (2 byte × num_cells)  ← cresce giù │
├──────────────────────────────────────────────────┤
│  Spazio libero                                    │
├──────────────────────────────────────────────────┤
│  Dati celle (crescono dal fondo)  ← cresce su     │
└──────────────────────────────────────────────────┘
```

### Celle

- **Cella foglia**: `[2 byte key_len][key...][4 byte value_len][value...]`
- **Cella interna**: `[2 byte key_len][key...][4 byte child_page_num]`

### Operazioni

**`find(chiave)`**: parte dalla radice. In ogni nodo, fa una **binary search**
nell'array delle chiavi per decidere da che figlio proseguire. Arrivato a una
foglia, cerca la chiave esatta. O( log N ).

**`insert(chiave, valore)`**: come find, ma quando arriva alla foglia scrive
la cella. Se la foglia è **piena** (spazio insufficiente), la **split** in due
metà e promuove la chiave separatore al padre. Lo split può propagarsi fino
alla radice. O( log N ).

Lo split della radice crea un **nuovo livello**: la vecchia radice diventa
figlia sinistra, una nuova pagina diventa figlia destra, e una nuova radice
interna le collega.

**`remove(chiave)`**: come find, ma quando arriva alla foglia rimuove la cella.
Se la foglia rimane **vuota**, fonde le celle del fratello nella pagina vuota
(**merge**) e libera la pagina del fratello. Il genitore viene aggiornato
per riflettere la nuova struttura. Se durante l'aggiornamento nascono celle
ridondanti (due celle adiacenti che puntano allo stesso figlio), vengono
compattate. Se la radice rimane senza celle, viene rimossa e il suo unico
figlio diventa la nuova radice (**riduzione dell'altezza**). O( log N ).

**`scan_all(callback)`**: visita ricorsivamente tutte le foglie in ordine
(DFS: prima tutti i figli del primo separatore, poi del secondo, ecc.).

### Perché il B-tree?

Un B-tree è l'ideale per database perché:
- È **sempre bilanciato**: tutte le foglie sono alla stessa profondità
- Ogni nodo corrisponde a una pagina del disco: poche letture per query
- Supporta **scansione in ordine** (utile per `ORDER BY` e range query)
- Insert e find sono O( log N ) con fattore di branching ~100+ per pagina 4KB

> SQLite, PostgreSQL, MySQL (InnoDB) usano tutti B-tree (o B+tree)
> come struttura di storage principale.

### Overflow pages

Quando una riga è troppo grande per stare in una pagina 4 KB, il suo valore
viene memorizzato in una **catena di pagine overflow**.

#### Soglia

Un valore è salvato inline se e solo se:

    2 + key.size() + 4 + value.size() ≤ 4081

(~4075 byte per una chiave di 8 byte). Oltre questa soglia, scatta l'overflow.

#### Formato cella con overflow

Invece del solito `[value_len][value]`, la cella foglia contiene:

    [2 key_len][key][4 0xFFFFFFFF][4 total_size][4 first_page]

| Campo | Significato |
|---|---|
| `0xFFFFFFFF` | **Sentinella** — segnala che è overflow |
| `total_size` | Dimensione totale del valore originale |
| `first_page` | Numero della prima pagina della catena overflow |

#### Formato pagina overflow

Ogni pagina overflow è un blocco 4 KB con questa struttura:

    [4 next_page][4 data_len][4088 data]

| Campo | Significato |
|---|---|
| `next_page` | Pagina successiva nella catena (0 = fine) |
| `data_len` | Quanti byte di dati utili in questa pagina |
| `data` | I dati veri e propri (fino a 4088 byte) |

#### Catena di pagine

Un valore grande viene suddiviso in blocchi da 4088 byte. Ogni blocco
occupa una pagina overflow. Le pagine sono collegate in una lista
concatenata unidirezionale tramite il campo `next_page`:

```
┌──────────┐    next_page ┌──────────┐    next_page ┌──────────┐
│ Pagina 7 ├─────────────►│ Pagina 12├─────────────►│ Pagina 3 │
│ data_len │              │ data_len │              │ data_len │
│ = 3000   │              │ = 2000   │              │ = 500    │
└──────────┘              └──────────┘              └──────────┘
                                                       next_page = 0
```

#### Ottimizzazione binary search

La funzione `read_leaf_key()` legge solo la chiave, NON il valore.
Durante `find_key_in_node()`, anche per celle overflow, viene letta
solo la chiave — la catena overflow viene seguita solo quando serve
il valore (in `read_leaf_cell()`).

#### Trasparenza

Le funzioni `insert`, `find`, `remove` e `scan_all` gestiscono
l'overflow internamente. Il chiamante (Database/Executor) non vede
differenza tra una cella inline e una overflow.

---

## 3. Serializzazione delle righe

**File**: `src/row.h`

Una riga (`Row`) contiene un rowid e un array di `Value`. Ogni `Value`
può essere NULL, intero (int64) o stringa.

La serializzazione in binario è **auto-descrittiva** (ogni valore dice
il suo tipo), così possiamo deserializzare anche senza conoscere
lo schema al momento della lettura.

Formato binario (little-endian):
```
[num_col:2] [tipo:1] [dati...] [tipo:1] [dati...] ...
```

- INT: `[tipo=1] [valore:8 byte]`
- STRING: `[tipo=2] [lunghezza:4 byte] [dati:N byte]`
- NULL: `[tipo=0]`

### Codifica delle chiavi B-tree

Per garantire l'ordinamento lessicografico nel B-tree:
- **Rowid**: 8 byte **big-endian** (il byte più significativo prima).
  Così `1 → 00...01` viene prima di `2 → 00...02`.
- **Stringhe per indici**: 4 byte di lunghezza big-endian + i byte della
  stringa. La lunghezza davanti garantisce che "abc" non venga confuso
  con "abcdef" e che stringhe più corte vengano prima.

---

## 4. Tokenizer — da SQL a token

**File**: `src/tokenizer.h`, `src/tokenizer.cpp`

Il tokenizer scorre la stringa SQL carattere per carattere e produce
una lista di `Token`. Ogni token ha un tipo, il testo originale,
e un valore numerico (per i numeri).

Riconosce:
- **Keyword** (`SELECT`, `FROM`, `WHERE`, `CREATE`, ecc.) — lookup
  in una mappa di stringhe maiuscole
- **Identificatori** (nomi di tabelle, colonne)
- **Numeri** interi
- **Stringhe** tra apici semplici: `'testo'`
- **Simboli**: `*`, `=`, `<`, `>`, `(`, `)`, `,`, `;`

Le keyword sono **case-insensitive**: `select`, `SELECT`, `Select`
producono lo stesso token.

> Nota: i commenti SQL (`--` e `/* */`) non sono gestiti.

---

## 5. Parser — da token ad AST

**File**: `src/parser.h`, `src/parser.cpp`

Il parser è **ricorsivo-discendente**: per ogni regola grammaticale
c'è una funzione che consuma i token corrispondenti.

Grammatica supportata:

```
statement    → CREATE TABLE name ( col_def {, col_def } )
             | CREATE INDEX name ON name ( name )
             | INSERT INTO name VALUES ( value {, value } )
             | SELECT [ * | name {, name } ] FROM name [ WHERE cond ]
             | DELETE FROM name [ WHERE cond ]
             | UPDATE name SET name = value [ WHERE cond ]
             | DROP TABLE name

col_def      → name TYPE [PRIMARY KEY]
cond         → name OP value
value        → NUMBER | STRING | NULL | TRUE | FALSE
OP           → = | < | > | <= | >= | != | <>
```

L'output è un `std::variant` (C++17) che contiene lo statement
specifico: `SelectStmt`, `InsertStmt`, `CreateTableStmt`, ecc.
Questo evita gerarchie di classi e puntatori.

---

## 6. Executor — esecuzione delle query

**File**: `src/executor.h`, `src/executor.cpp`

L'Executor prende lo `Statement` (l'AST del parser) e chiama i metodi
del `Database`. Usa `std::visit` per smistare il variant al gestore giusto:

```cpp
return std::visit([this](const auto& s) -> QueryResult {
    using T = std::decay_t<decltype(s)>;
    if constexpr (std::is_same_v<T, SelectStmt>)
        return execute_select(s);
    if constexpr (std::is_same_v<T, InsertStmt>)
        return execute_insert(s);
    // ...
}, stmt);
```

`QueryResult` contiene: `ok` (successo/fallimento), `message`,
`columns` (nomi colonne per SELECT), `rows` (dati), `affected` (righe modificate).

---

## 7. Database — il motore

**File**: `src/database.h`, `src/database.cpp`

Il Database coordina tutto. Mantiene in memoria la `TableInfo` di ogni
tabella (nome, colonne, pagina radice del B-tree, indici, prossimo rowid).

### Catalogo (pagina 0)

Lo schema del database è salvato sulla **pagina 0** del file.
Quando apri un database esistente, `load_catalog()` legge la pagina 0
e ricostruisce i metadati. Quando chiudi, `save_catalog()` riscrive tutto.

Il formato inizia con il magic number `0x4C45414E` ("LEAN" in ASCII).
Se la pagina 0 non ha questo magic, il database è vuoto.

#### Overflow del catalogo

Se lo schema diventa troppo grande per la pagina 0 (4 KB), scatta
l'overflow. La pagina 0 cambia formato:

```
Formato inline (schema ≤ 4 KB):
  [4 MAGIC][4 num_tables][tables...]

Formato overflow (schema > 4 KB):
  [4 MAGIC][4 0xFFFFFFFF][4 total_size][4 first_page]
  → i dati (tutto tranne MAGIC) sono su pagine overflow
```

I campi:

| Offset | Campo | Significato |
|---|---|---|
| 0–3 | `MAGIC` | "LEAN" |
| 4–7 | `0xFFFFFFFF` | **Sentinella** — segnala overflow |
| 8–11 | `total_size` | Dimensione del payload (tutto tranne MAGIC) |
| 12–15 | `first_page` | Prima pagina della catena overflow |

Le pagine overflow del catalogo usano lo **stesso formato** delle
overflow pages del B-tree: `[4 next_page][4 data_len][4088 data]`.

La `load_catalog()` è **backward compatible**: se il campo 4–7 non è
`0xFFFFFFFF`, assume il formato inline classico senza overflow.

### Indici

`create_index()`:
1. Alloca una pagina radice per l'indice
2. Scansiona tutte le righe esistenti con `scan_all()`
3. Per ogni riga, inserisce `(valore_colonna → rowid)` nell'indice

`select()` con WHERE:
- Se esiste un indice sulla colonna e l'operatore è `=`, usa l'indice
  per trovare il rowid → poi cerca la riga nel B-tree della tabella
- Altrimenti: **full scan** (scorre tutte le righe e applica il filtro)

### Transazioni

Non ci sono transazioni. Ogni operazione è immediatamente visibile
e persistente alla chiusura. Un database reale avrebbe WAL (Write-Ahead Log)
e isolamento tra transazioni.

---

## 8. REPL — interfaccia interattiva

**File**: `src/main.cpp`

Il REPL accumula righe finché non trova un `;`, poi esegue lo statement.

```
$ ./build/leanrndb
leanrndb v0.1.0 — type SQL statements or EXIT
> CREATE TABLE persone (id INT, nome VARCHAR);
CREATE TABLE
> INSERT INTO persone VALUES (1, 'Alice');
INSERT (1)
> INSERT INTO persone VALUES (2, 'Bob');
INSERT (1)
> SELECT * FROM persone;
id | nome
---+-----
1 | Alice
2 | Bob
(2 rows)
> SELECT * FROM persone WHERE id = 1;
id | nome
---+-----
1 | Alice
(1 rows)
> EXIT
Bye.
```

Si può anche usare in modalità **one-shot**:

```sh
$ ./build/leanrndb mio.db "SELECT * FROM persone;"
```

### Dot-command utility

Oltre al SQL, il REPL accetta comandi che iniziano con `.` (stile SQLite):

| Comando | Effetto |
|---|---|
| `.tables` | Elenca tutte le tabelle |
| `.schema [table]` | Mostra il `CREATE TABLE`/`CREATE INDEX` |
| `.describe <table>` | Colonne, tipi, rowid, indici nel dettaglio |
| `.head <table> [N]` | Prime N righe (default 10) |
| `.count <table>` | Conteggio righe |
| `.indices [table]` | Elenca gli indici |
| `.dbinfo` | Statistiche database (# tabelle, # pagine) |
| `.help` | Lista completa dei comandi |
| `.exit` / `.quit` | Esce dal REPL |

```sh
> CREATE TABLE t (id INT, val VARCHAR);
CREATE TABLE
> .tables
t
> .describe t
      table: t
  root page: 1
  next rowid: 1
    columns:
    id  INT
    val  VARCHAR
> .exit
```

---

## Limiti e semplificazioni

Questo database è **didattico**, non production-grade. Ecco cosa manca:

| Funzionalità | Come sarebbe in un DB vero |
|---|---|---|
| Merge senza redistribuzione | Una foglia assorbe l'intero fratello; può fallire se non ci sta |
| Cursor.scan_one_leaf | Non attraversa i bordi tra foglie |
| Catalogo su pagina 0 (max 4 KB) | Overflow pages per cataloghi grandi (supportato ma non attivo) |
| Nessun WAL / transazioni | Ogni operazione è atomica solo a livello di pagina |
| Nessun recovery | Se il programma crasha, il file può corrompersi |
| Nessun controllo vincoli | Primary key, NOT NULL, UNIQUE non forzati |
| Nessun JOIN | Solo query su una tabella |
| Stringhe tra apici | Escape limitato, no double quotes per identificatori |
| Overflow page non liberate | Le pagine overflow orphanate non vengono riciclate |

---

## Compilazione

```sh
cmake -B build
cmake --build build
./build/test_basic    # 15 test
./build/leanrndb      # REPL
```

---
