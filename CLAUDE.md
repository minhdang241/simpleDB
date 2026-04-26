# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## About chidb

chidb is a didactic relational database management system (RDBMS) built at the University of Chicago. It is a C project using Autotools (autoconf/automake) for the build system and the `check` framework for tests.

## Build Commands

```sh
# Initial setup (run once after clone or after modifying configure.ac/Makefile.am)
./autogen.sh
./configure

# Build everything
make

# Run all tests
make check

# Run a single test suite binary directly (after make check has built them)
./tests/check_btree
./tests/check_pager
./tests/check_dbrecord
./tests/check_dbm
./tests/check_utils

# Run the interactive shell
./chidb <database-file>
```

Dependencies required: `flex`, `bison`, `libedit`, and the `check` testing library (>= 0.9.14).

## Architecture

The system is organized as two libraries plus a shell binary:

### `libchisql` — SQL parsing layer
Located in [src/libchisql/](src/libchisql/). Flex/Bison grammar in `sql.l` / `sql.y` (auto-generates `sql-lexer.c` and `sql-parser.c` at build time). Produces a `chisql_statement_t` AST representing parsed SQL.

### `libchidb` — Database engine
Located in [src/libchidb/](src/libchidb/). Layers from bottom to top:

1. **Pager** ([pager.c](src/libchidb/pager.c) / [pager.h](src/libchidb/pager.h)) — raw page I/O; manages a file as fixed-size pages (`DEFAULT_PAGE_SIZE = 1024`).

2. **B-Tree** ([btree.c](src/libchidb/btree.c) / [btree.h](src/libchidb/btree.h)) — SQLite-compatible B-tree over pages. Four node types: `PGTYPE_TABLE_INTERNAL`, `PGTYPE_TABLE_LEAF`, `PGTYPE_INDEX_INTERNAL`, `PGTYPE_INDEX_LEAF`.

3. **Record** ([record.c](src/libchidb/record.c)) — encodes/decodes database records stored in B-tree leaf cells.

4. **DBM (Database Machine)** — a register-based virtual machine that executes compiled SQL programs:
   - Types/opcodes in [dbm-types.h](src/libchidb/dbm-types.h) — opcodes are defined via `FOREACH_OP` macro (OpenRead, OpenWrite, Rewind, Next, Seek*, Column, Insert, Eq/Ne/Lt/etc., Halt, …).
   - Execution engine in [dbm.c](src/libchidb/dbm.c) and [dbm-ops.c](src/libchidb/dbm-ops.c).
   - Cursor abstraction in [dbm-cursor.c](src/libchidb/dbm-cursor.c) / [dbm-cursor.h](src/libchidb/dbm-cursor.h).
   - DBM file format helpers in [dbm-file.c](src/libchidb/dbm-file.c).

5. **Code Generator** ([codegen.c](src/libchidb/codegen.c)) — walks the `chisql_statement_t` AST and emits a DBM program (`chidb_stmt`).

6. **Optimizer** ([optimizer.c](src/libchidb/optimizer.c)) — rewrites the relational algebra plan before code generation.

7. **API** ([api.c](src/libchidb/api.c)) — public interface: `chidb_open`, `chidb_prepare`, `chidb_step`, `chidb_finalize`, `chidb_close`, plus column accessors. The `chidb` struct holds a single `BTree*`; the `chidb_stmt` struct is the compiled DBM program.

### Shell
[src/shell/](src/shell/) — readline-based interactive shell using `libedit`.

### Internal types ([src/libchidb/chidbInt.h](src/libchidb/chidbInt.h))
- `chidb_key_t` — `uint32_t` B-tree key
- `npage_t` — `uint32_t` page number
- `ncell_t` — `uint16_t` cell index
- Private error codes (`CHIDB_ENOTFOUND`, `CHIDB_EDUPLICATE`, etc.) distinct from the public API codes.

## Test Structure

Tests live in [tests/](tests/) and use the `check` framework. The btree suite is split across numbered files (`check_btree_1a.c` through `check_btree_8.c`), each testing incremental functionality. `check_common.c` provides shared test helpers.
