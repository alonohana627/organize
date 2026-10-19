---
name: add-search-field
description: Add a field:value predicate or a sort column to the typed search query language (parser, SQL compiler with bound parameters, in-memory RowMatcher in exact parity, sort whitelist, tests).
---

# Add a search field or sort

User text is never concatenated into SQL or forwarded as raw FTS5 syntax. Every value is parsed into a
typed AST and bound as a parameter.

## A new `field:value`

1. `include/mso/search/query.hpp`: add the enum value to `Field` (and to `to_string`).
2. `src/search/query_parser.cpp`:
   - add the name (and aliases) to `field_from_name`;
   - parse the value in `parse_value`; numeric fields reuse `parse_numeric` with a unit table
     (BPM → milli-BPM, durations → microseconds); errors call `error_at(offset, ...)` so the UI can
     point at the exact code point.
3. `src/search/query_compiler.cpp`: compile it in `QueryCompiler::compile_predicate`, appending SQL
   with `?` placeholders and pushing the values to `out.parameters` **in the same order**. Use
   `EXISTS(...)` for one-to-many relations so rows never duplicate. LIKE patterns go through
   `escape_like` with `ESCAPE '\'`. Anything read from the catalog during compilation (roots,
   collections, tag redirects) is cached on the compiler, which serves the whole smart-collection
   expansion. A condition on the primary location (like `folder:`) goes through
   `primary_location_filter`; pass the compiler's `LibraryRevision` on, so the plan it picks is
   remembered per library, revision and condition instead of being counted again on every page.
4. `src/search/row_matcher.cpp`: evaluate it in `RowMatcher::Prepared::evaluate_predicate` with
   **exactly** the SQL semantics (the desktop app answers row-evaluable queries from its in-memory
   index, so the two must agree). Keep the per-row cost to field reads and integer lookups: anything
   that depends only on the user's value, a library, a root or a taxonomy id is computed once
   (`prepare`, or the node's per-library `LibraryMemo`, which also remembers decisions by path for
   tags and categories that have no id yet), never per row. Compare stored keys
   (`SampleRow::path_key`, taxonomy ids) instead of re-normalizing row text, and never hard-code a
   case: path keys come from `mso::path_key`, names from `mso::fold_key`.
5. Whether a predicate can be evaluated from a `SampleRow` is decided in **one** place,
   `detail::is_predicate_row_evaluable` (`predicate_support.cpp`); return false there when it
   cannot (the query then runs in SQLite through `matching_ids`).
6. If the field needs an index for speed, add it in a migration (see add-schema-migration).
7. Statements ordered by `s.id` write `+s.deleted=0`: plain `s.deleted=0` lets SQLite pick a
   `(deleted, ...)` sort index, read every live sample and sort them, even for one id (`load_rows` of
   one id cost 45 ms at 1M rows that way). "row hydration reads ... (query plans)" guards it.

## Semantics every field follows

- **Equality at typed precision:** `bpm:120` means 119.5 ≤ x < 120.5, `bpm:120.5` means
  120.45 ≤ x < 120.55; a range `a..b` widens both ends the same way. `<`, `<=`, `>`, `>=` are exact.
- **Two-valued NOT:** `X` and `NOT X` partition the library; unknown values (NULL) fall on the `NOT`
  side, never in neither (the compiler wraps predicates under NOT in `COALESCE(…,0)`).
- **Primary location:** `folder:` and the root/folder scopes use a sample's primary location
  (`detail::kPrimaryLocationOrder`); a folder scope requires its `root_id`. `primary_location_filter`
  picks an index-driven plan for selective folders and a streaming one for broad folders. A typed
  folder is addressed from a root down: leading separators and the spaces around them are dropped
  (`folder:"/ Drums"` is `folder:Drums`).
- **Taxonomy:** `tag:Kick` matches every tag named Kick at any depth with its subtree, and follows
  merged tags (`tag:<old name>` matches the canonical tag's subtree). The in-memory matcher needs the
  libraries' `TagAliases` (`load_tag_aliases`) for that. Multi-segment `tag:` paths are SQL-only.
- `analyzed:none` (alias `false`) matches samples with no analysis result at all.
- **Smart collections** are expanded inline by one compiler, capped at `kMaxCompiledNodes` (1024)
  nodes and nesting 4. A problem in a *stored* query is that library's failure (DB_CORRUPT or
  RESOURCE_LIMIT in its coverage); only problems of the typed text fail the whole request.
- Relevance sort keeps non-text OR matches; they rank after the text hits. Its cursor records the
  rows delivered per library.
- **Hydration failures end:** rows of a library that fail to hydrate (`load_rows`) are offered again
  on the next page once (`kHydrationRetries`) and then skipped; every page that lost rows marks that
  library incomplete in its coverage. Both cursors (keyset and relevance) carry the retry count, so a
  persistent failure can never make `mso search` or an export page forever.

## A new sort

Add it to `SortField`, `to_string`/`parse_sort_field`, `sort_expression` (a column or deterministic
expression, NULL when unknown) and `is_text_sort` if the key is text. Keyset paging handles nulls in a
second partition automatically; the final tie-breaker is always the sample id.

## Tests (`tests/search_tests.cpp`)

- parse success and positioned failures for malformed values;
- the predicate holds for **every** returned row ("hard filters");
- paging with a small page size visits every row exactly once for the new sort, both directions;
- an injection-looking value returns no rows and changes nothing;
- parity: add the predicate (plain, negated, combined) to "the in-memory matcher agrees with the SQL
  compiler" and, when it depends on a library, root or taxonomy id, to "one matcher agrees with SQL
  over two libraries whose ids collide".

Performance: `mso_tests --test-suite=search --no-skip --test-case=*benchmark*` runs the skipped-by-default
100K-row benchmark; run it in a release build (it checks the docs/06 budgets). Measured on the dev PC
(2026-10-19, release, median of five): first page about 3 ms, `for_each_row` about 150 ms,
`folder:Loops` in memory about 2.6 ms (53–75 ms before `SampleRow::path_key`), `tag:Kick` in memory
about 0.6 ms; in SQL, `folder:Packs/Goa` (1% of the rows) counts in about 11 ms (56 ms before the
index-driven plan) and `folder:Loops` (25%) stays at about 57 ms. `MSO_SEARCH_BENCHMARK_ROWS=1000000`
runs it at D1M (the in-memory budgets are for 100K, so those checks fail there). Choosing the folder
plan costs about 0.2 ms per compile at 100K and 2 ms at 1M; with the plan cache a second page of
`folder:Loops` skips it (1M, a loaded machine, 2026-10-19: 56.3 ms before, 51.7 ms after).
Hydration by rowid (`+s.deleted`, 2026-10-19, release): `load_rows` of one id 2.9 ms → 0.01 ms at
100K and 45 ms → 0.01 ms at 1M; 200 ids 51 → 3.6 ms and `for_each_row` 1977 → 1064 ms at 1M;
`matching_ids` for one file name 41.6 → 0.02 ms and for `kick` (16% of rows) 222 → 156 ms at 1M (at
100K that broad term goes 13.4 → 18.9 ms, the one regression).

Also document the field in the "Searching" table of `README.md` (written for users: plain words, no SQL).
