---
name: add-schema-migration
description: Change the catalog.sqlite schema safely with a new numbered, checksummed forward migration (never edit a released one), plus the outbox/revision and test obligations.
---

# Add a schema migration

The catalog verifies the SHA-256 of every applied migration on open. Editing a released migration makes
every existing library refuse to open (`DB_CORRUPT`). Always add a new one.

## Steps

1. Read `contracts/schema.sql`: it is the end-state reference. Copy table definitions from it when a
   feature ships (STRICT tables, CHECK constraints, explicit indexes).
2. In `src/catalog/migrations.cpp` add `kMigrationN` (a raw SQL string) and append
   `{N, "what it adds", kMigrationN, {}}` to the list. Never reorder or change earlier entries.
3. Opening an older library now: takes a verified backup in `<library>/backups/` (pre-migration),
   applies each pending migration in its own transaction, records its checksum, runs
   `PRAGMA integrity_check`. You get this for free; just keep the migration transactional (no
   `BEGIN`/`COMMIT` inside the SQL).
4. Mutations: every write path must run inside `sqlite::WriteTransaction`, call
   `detail::advance_revision(connection, envelope)` exactly once (catalog revision + outbox), and refresh
   `detail::refresh_search_projection` for samples whose searchable text changed.
5. User-visible edits go through history: express them as patch operations in
   `src/catalog/patch_operations.cpp` (compare-and-set `before`/`after`) so undo/redo and conflict
   detection keep working.
6. Tests (`tests/catalog_tests.cpp`): create a library, exercise the new table through the public
   `catalog::Library` API, reopen it, and check `integrity_report()` stays clean. If you add a
   constraint, test that invalid data is rejected without partial writes.
7. Pin the new migration's checksum in the test "released migrations are pinned: their checksums never
   change" (`tests/catalog_tests.cpp`) in the same change, so a later edit of the shipped SQL fails the
   tests instead of every existing library. Pinned so far: migrations 1-6; migration 6 (`jobs_ready`
   and `scan_frontier_state` indexes, `scan_frontier.directory_key`) is
   `d626e8ddfd71f63dfdc0c742c4942cfdf2dd2f85fca222dc844667ec36817b5c`.
8. Update `contracts/schema.sql` (the end-state reference) with the new tables, columns and indexes.
9. Indexes, especially partial ones: a query uses a partial index only when its WHERE clause repeats
   the index's own condition (SQLite does not infer `state<>'retired'` from `state='missing'`). The
   test "every statement the catalog runs on locations and jobs is index-driven" explains every
   statement the library compiled while exercising scans, edits and the analysis queue, and fails on a
   full scan or a temporary B-tree sort of `locations` or `jobs`. When you add a statement on those
   tables, make sure the scenario in that test runs it; accept a plan line there only with a written
   reason it stays bounded.
10. Changing `fold_key` or `path_key` output: bump `kFoldVersion` (`include/mso/domain/core.hpp`).
    The next open takes a verified backup and rebuilds every stored key; a row whose new key another
    row already has keeps a placeholder key and is listed in a `normalization_conflict` review item
    (nothing is merged).

## Do not

- Do not delete rows of samples whose files disappeared: tombstone or mark locations missing.
- Do not store absolute machine paths in the portable catalog (root bindings live in the workspace).
- Do not run filesystem or decoding work inside a write transaction.
