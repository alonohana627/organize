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
4. If the field needs an index for speed, add it in a migration (see add-schema-migration).

## A new sort

Add it to `SortField`, `to_string`/`parse_sort_field`, `sort_expression` (a column or deterministic
expression, NULL when unknown) and `is_text_sort` if the key is text. Keyset paging handles nulls in a
second partition automatically; the final tie-breaker is always the sample id.

## Tests (`tests/search_tests.cpp`)

- parse success and positioned failures for malformed values;
- the predicate holds for **every** returned row ("hard filters");
- paging with a small page size visits every row exactly once for the new sort, both directions;
- an injection-looking value returns no rows and changes nothing.

Also document the field in the query language section of `README.md`.
