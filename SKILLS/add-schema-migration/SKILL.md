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

## Do not

- Do not delete rows of samples whose files disappeared: tombstone or mark locations missing.
- Do not store absolute machine paths in the portable catalog (root bindings live in the workspace).
- Do not run filesystem or decoding work inside a write transaction.
