---
name: add-search-field
description: Add a field:value predicate or a sort column to the typed search query language (parser, SQL compiler with bound parameters, sort whitelist, tests).
---

# Add a search field or sort

User text is never concatenated into SQL or forwarded as raw FTS5 syntax. Every value is parsed into a
typed AST and bound as a parameter.

## A new `field:value`

1. `include/mso/search/query.hpp`: add the enum value to `Field` (and to `to_string`).
2. `src/search/query_parser.cpp`:
   - add the name (and aliases) to `field_from_name`;
   - parse the value in `parse_value`; numeric fields reuse `parse_numeric` with a scale and unit table
     (e.g. BPM → milli-BPM, durations → microseconds); errors call `error_at(offset, ...)` so the UI can
     point at the exact code point.
3. `src/search/query_compiler.cpp`: compile it in `compile_predicate`, appending SQL with `?`
   placeholders and pushing the values to `out.parameters` **in the same order**. Use
   `EXISTS(...)` for one-to-many relations so rows never duplicate. LIKE patterns go through
   `escape_like` with `ESCAPE '\'`.
4. `src/search/row_matcher.cpp`: evaluate it in `matches()` with **exactly** the SQL semantics (the
   desktop app answers row-evaluable queries from its in-memory index, so the two must agree); if it
   cannot be evaluated from `SampleRow` fields, make `is_row_evaluable` return false for it.
5. If the field needs an index for speed, add it in a migration (see add-schema-migration).

## Semantics every field follows

- **Equality at typed precision:** `bpm:120` means 119.5 ≤ x < 120.5 (the precision the user typed),
  `bpm:120.5` means 120.45 ≤ x < 120.55; ranges and comparisons are exact.
- **Two-valued NOT:** `X` and `NOT X` partition the library; unknown values (NULL) fall on the `NOT`
  side, never in neither.
- **Primary location:** `folder:` and the root/folder navigation scopes use a sample's primary (first
  live) location; a folder scope requires its `root_id`.
- `analyzed:none` (alias `false`) matches samples with no analysis result at all.
- Smart collections are expanded inline, capped at `kMaxCompiledNodes` (1024) nodes.
- Relevance sort keeps non-text OR matches; they rank after the text hits.

## A new sort

Add it to `SortField`, `to_string`/`parse_sort_field`, `sort_expression` (a column or deterministic
expression, NULL when unknown) and `is_text_sort` if the key is text. Keyset paging handles nulls in a
second partition automatically; the final tie-breaker is always the sample id.

## Tests (`tests/search_tests.cpp`)

- parse success and positioned failures for malformed values;
- the predicate holds for **every** returned row ("hard filters");
- paging with a small page size visits every row exactly once for the new sort, both directions;
- an injection-looking value returns no rows and changes nothing;
- `matches()` agrees with the SQL result for the new predicate (the row-evaluator parity tests).

Performance: `mso_tests --test-suite=search --no-skip --test-case=*benchmark*` runs the skipped-by-default
100K-row benchmark (release build: `for_each_row` about 141 ms, first page about 2.9 ms on the dev PC).

Also document the field in the "Searching" table of `README.md` (written for users: plain words, no SQL).
