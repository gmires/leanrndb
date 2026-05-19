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
