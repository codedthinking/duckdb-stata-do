# Missing Commands in dodo

Inventory based on [korenmiklos/ceo-value](https://github.com/korenmiklos/ceo-value/tree/main/lib/create). Updated May 2026 for v0.2.0.

## Implemented (v0.2.0)

These were previously listed as missing but are now complete:

- `merge` (1:1, m:1, 1:m, m:m) with `keep()`, `keepusing()`, `nogenerate`
- `bysort`/`by` prefix with partition and sort vars
- `xtset`/`tsset` + `L.`/`F.`/`D.` gap-aware lag/lead operators
- `tempfile`, `preserve`/`restore`
- `duplicates drop` (all columns or by varlist)
- `expand` with optional `generate()`
- `import delimited`, `export delimited`
- `inrange()`, `inlist()`, `cond()`, `substr()`, `real()`, `int()`
- `strlower()`, `strupper()`, `strtrim()`, `strlen()`
- Running `sum()` in `bysort:` context
- `label variable`, `label define`, `label values`, `label list`
- `reshape long` and `reshape wide`
- `mvencode`
- `undo`/`redo`, `history`
- `show sql`
- `var[_n-1]` subscript syntax (positional lag/lead)

## Still missing

### Critical

| Gap | Usage | SQL mapping |
|---|---|---|
| Compile-time macros (`local`/`global`/literal `scalar`, `` `x' ``/`$x`) | Variable substitution | Textual substitution in a pre-tokenization pass |
| `foreach` / `forvalues` loops | Repeated operations | Unroll the body at compile time |
| Runtime results (`r(max)`, `r(N)`, `levelsof … local()`) | Reuse of computed values | Compiled to **SQL subqueries**, not substituted as literals |

These are programming constructs, not data commands. Design lives in
**`docs/VARIABLE_SUBSTITUTION.md`**: the core split is *compile-time-known* values
(handled by textual substitution + loop unrolling, works in `dodoc` with no
database) versus *runtime-dependent* values (`r()`, `levelsof`), which are
compiled into the generated SQL as subqueries rather than fetched and pasted
back — preserving lazy evaluation and `dodoc`/extension parity.

### Nice to have

| Gap | Usage | Notes |
|---|---|---|
| `joinby` | Many-to-many merge | Could use `CROSS JOIN` or unrestricted `JOIN` |
| `reghdfe` / `regress` | Regression | Needs stats extension or custom implementation |
| `recode` | Value recoding | `CASE WHEN` chains |
| `scalar`, `display`, `assert` | Scripting/debugging | Low priority |
| `set seed` | Simulation setup | `SELECT setseed(N)` |
| `compress` | No-op | DuckDB doesn't need this |
| `mvencode _all` | Replace all missing | Needs column introspection at runtime |
| `reshape wide` with multiple value vars | Multi-var pivot | Currently supports one value variable |
