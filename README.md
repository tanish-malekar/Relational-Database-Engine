# Transactional SQL Database Engine with MVCC and ARIES Recovery

A disk-based **C++17** SQL engine with persistent **B+ tree** primary indexes,
slotted heap pages, an LRU page cache, **MVCC snapshot isolation**, and **ARIES
write-ahead-log recovery**. CMake builds a command-line database, library,
GoogleTest specifications, and a process-crash workload tool.

## Storage and indexing

Logical pages contain 4 KiB of data. A physical envelope adds a page LSN and
checksum. A database format marker and exclusive process lock protect the data
directory. The catalog lives on page zero; table heaps use linked slotted pages
with stable row IDs. B+ tree nodes split recursively and preserve linked leaves.
Primary keys are unique signed 64-bit integers. Deletion removes leaf entries
without merging underfull index nodes. Heap tombstones and unused page IDs are
retained; online vacuum and page reuse are outside this version.

The page cache uses an LRU replacement list. Transactions hold private changed
pages, read their own writes, and retain older committed page versions for active
snapshots. Committing validates each changed page against the transaction's start
snapshot. A conflicting transaction fails with `WriteConflict` and must restart.
Read-only snapshots can continue after another transaction updates or deletes a
row. The state latch covers individual page access and the physical commit, not
the lifetime of an open transaction.

Versioning and conflict checks operate at page granularity. Two writers changing
different rows on the same page can conflict. This is conservative snapshot
isolation, not serializability: write skew on different pages remains possible.
Old versions are pruned after the oldest active snapshot advances. Long-lived
snapshots can retain substantial memory. Transaction objects are thread-confined,
cannot nest on one thread, and touch at most 4,096 changed pages.

## ARIES recovery

The append-only checksummed WAL contains transaction IDs, LSNs, previous-LSN
chains, full before/after page images, durable commit records, transaction-end
records, and redo-only compensation log records with `undoNextLSN`.

1. **Analysis** reconstructs the transaction table and conservative dirty-page
   recLSNs from the log.
2. **Redo** repeats history, including loser updates and compensation records.
   Page LSNs skip already applied records; full images repair torn page envelopes.
3. **Undo** walks loser chains in descending LSN order. A CLR is forced before its
   undo image reaches disk. A crash during recovery resumes through `undoNextLSN`
   rather than undoing a completed compensation again.

The commit path deliberately allows WAL-protected **steal** writes before the
commit record. The commit decision is durable before success is returned.
This implementation forces pages during commit and uses a quiescent checkpoint;
it does not implement fuzzy checkpoints, logical index undo, nested top actions,
or log truncation. Recovery scans the retained log. On an uncertain I/O failure,
the engine is fenced until reopened. Durability assumes a local POSIX filesystem
that honors fsync and atomic file/directory persistence.

## SQL interface

```sql
CREATE TABLE items (id INT PRIMARY KEY, value TEXT);
INSERT INTO items VALUES (1, 'hello');
BEGIN;
UPDATE items SET value = 'updated' WHERE id = 1;
COMMIT;
SELECT * FROM items WHERE id = 1;
DELETE FROM items WHERE id = 1;
```

The parser supports INT and TEXT, quoted strings with doubled apostrophes,
CREATE TABLE, INSERT VALUES, SELECT *, primary-key UPDATE/DELETE, one equality
filter, and one equijoin. Primary-key equality selects the B+ tree; a join on the
right primary key uses an index-nested-loop plan, otherwise a hash join. Scan and
join results materialize in memory. Primary keys are immutable. UPDATE requires
the replacement tuple to fit its current page after compaction. Tables have at
most 32 columns, and at most 64 table definitions must fit the catalog page.
There are no NULL values, aggregates, secondary indexes, aliases, or general SQL
expressions. A failed statement makes its transaction rollback-only.

## Build and use

Requires a C++17 compiler, CMake 3.22+, POSIX file APIs, and GoogleTest for the
specification target. The database library itself uses the C++ standard library
and POSIX APIs only.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build -j
./build/sql_engine data < examples/demo.sql
./build/sql_engine data
```

The CLI accepts one statement per line plus BEGIN, COMMIT, ROLLBACK, and QUIT.
Statements outside an explicit transaction autocommit. This C++ format uses a
fresh data directory; it does not reinterpret data files from other engines.

```cpp
#include "sqldb/engine.hpp"
sqldb::Engine db("data", 256);
auto tx = db.begin();
tx.execute("INSERT INTO items VALUES (2, 'example')");
tx.commit();
```

## Crash campaign and specifications

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
python3 tools/crash_campaign.py --worker build/sql_crash_worker --crashes 500 --output crash-results.jsonl
```

The campaign counts **500 actual injected child-process exits** by default,
including selected crashes during recovery. Each case seeds a database, durably
records an acknowledged transaction in an independent acknowledgement file,
and interrupts a multi-row update/delete/insert transaction at a WAL or page-write
boundary. Recovery must retain acknowledged rows and produce the correct atomic
before/after state. The ledger records injection points, observed state, failures,
and lost acknowledged transactions; summary counts come from those observations.
An injection point that is not reached aborts the campaign instead of counting
as a crash. These are process crashes, not a hardware power-loss simulation.

GoogleTest specifications cover old snapshots across deletion, concurrent write
conflicts, rollback-only behavior, B+ tree splits/reopen, and SQL string/type rules.
`SQL_CRASH_POINT` and `SQL_CRASH_OCCURRENCE` select deterministic hooks for separate
workloads. Runtime databases and campaign ledgers are excluded from Git.

References: [ARIES, Mohan et al.](https://research.ibm.com/publications/aries-a-transaction-recovery-method-supporting-fine-granularity-locking-and-partial-rollbacks-using-write-ahead-logging).

MIT licensed.
