# Variable Substitution — Macros, Scalars, and Stored Results

> Design for `local`, `global`, `scalar`, loops (`foreach`/`forvalues`), and
> stored results (`r()`, `e()`) in dodo. This is the plan for milestone **M14**.

## TL;DR — the one distinction that drives everything

dodo is a **compiler/transpiler**, not an interpreter like Stata. A `.do` file
becomes SQL once, and the SQL runs later (lazily, as a CTE chain). The standalone
`dodoc` front-end never touches DuckDB at all — it is pure text → SQL.

Because of that, every Stata "variable" splits into one of two categories, and the
two are handled by completely different machinery:

| | **Compile-time-known** | **Runtime-dependent** |
|---|---|---|
| Value is knowable | while compiling, from the source text alone | only after data is read and a query runs |
| Examples | `local years 2018 2019 2020`, `forvalues i = 1/10`, `foreach v of varlist x y z`, `global base "s3://bucket"`, `scalar pi = 3.14159` | `r(max)` after `summarize`, `r(N)` after `count`, `e(b)` after `regress`, `levelsof year, local(years)`, `keep if revenue == r(max)` |
| Mechanism | **textual substitution + loop unrolling** in a preprocessing pass | **compiled into the SQL** as a scalar subquery / set membership — never substituted as a literal value |
| Works in `dodoc` (no DuckDB)? | Yes, fully | Yes — the subquery is *emitted*; DuckDB evaluates it at run time |
| Requires executing a query mid-compile? | No | **No** (this is the whole point — see "Rejected: the round-trip" below) |

**The rule:** a runtime value such as `r(max)` is never resolved to a number by
dodo. It is rewritten into the generated SQL as a subquery, e.g.

```stata
summarize revenue
keep if revenue == r(max)
```

compiles to (schematically)

```sql
-- summarize records, but adds no CTE step; the chain's latest step is _s3
... WHERE revenue = (SELECT max(revenue) FROM _s3)
```

The lazy CTE chain stays intact, `dodoc` keeps working without a database, and the
value is computed exactly once, by DuckDB, at the moment the chain materializes.

---

## Background: how dodo compiles today

(See `docs/PLAN.md` for the full architecture.)

- Each transformation command appends one CTE step `_sN AS (SELECT … FROM _sN-1)`
  to a per-connection chain held in `DodoState`.
- Terminal commands (`list`, `summarize`, `count`, `tabulate`, …) prepend the
  full `WITH …` chain and add their own final `SELECT`. They **materialize but do
  not extend** the chain. In Stata these are exactly the commands that populate
  `r()`.
- `ProcessCommand(const DodoCommand&, DodoState&) -> std::string` is pure
  translation. The DuckDB extension hands the returned SQL to DuckDB; `dodoc`
  prints it. Neither path lets the compiler observe query *results*.

This last point is the hard constraint. Anything that needs a result value must be
expressed *as SQL*, not fetched and pasted back.

---

## Category A — Compile-time macros (textual substitution)

These behave like Stata's macro processor: pure text expansion that happens
**before** a command is tokenized. They never reach the data.

### A.1 Definitions

| Command | State effect | Notes |
|---|---|---|
| `local name text` | `locals[name] = "text"` | value is the literal remainder of the line |
| `local name = exp` | `locals[name] = fold(exp)` | `=` means "evaluate"; only **constant-foldable** expressions (numeric/string literals, arithmetic, `_N` is *not* constant — see A.4) |
| `global name text` | `globals[name] = "text"` | session-wide |
| `global name = exp` | `globals[name] = fold(exp)` | |
| `scalar name = exp` | constant → `scalars[name]` as literal | runtime case handled in Category B |

### A.2 References and the expansion pass

A preprocessing pass runs on each *logical* command line (after `///`
continuation joining, before `TokenizeCommand`) in this order:

1. **Globals** — `$name` and `${name}` → `globals[name]`.
2. **Locals** — `` `name' `` → `locals[name]`, expanded **innermost-first** so
   nested refs like `` `x`i'' `` work.
3. **Inline expression macros** — `` `=exp' `` → `fold(exp)` when constant-foldable.

Expansion is recursive (an expanded value may itself contain macro refs) with a
depth guard. Unknown locals expand to empty string (Stata semantics); unknown
globals likewise. Expansion happens inside double-quoted strings too, matching
Stata.

Runtime tokens (`r(...)`, `e(...)`, runtime scalars) are **deliberately left
untouched** by this pass — they are sentinels resolved later in Category B.

### A.3 Loops — unrolled at compile time

Loops are a programming construct, not a data command. The preprocessor reads the
loop header and `{ … }` body (possibly spanning lines) and emits the body once per
iteration, binding the loop local before each emission. The bound iterations feed
straight back through the macro-expansion pass.

| Header | Iterates over |
|---|---|
| `forvalues i = 1/10` | `1,2,…,10` |
| `forvalues i = 0(2)10` | `0,2,4,…,10` (start, step, stop) |
| `foreach v of varlist x y z` | literal varlist tokens |
| `foreach v in a b c` | literal list tokens |
| `foreach v of local mylist` | tokens of an **already-known** local |
| `foreach v of numlist 1 3 5` | literal numlist |
| `foreach v of global g` | tokens of a known global |

All of these are knowable from the source, so the loop is fully unrolled into
concrete commands. This is the only place loops are supported by the pure
compiler. **A loop over a runtime-populated list (e.g. the local that
`levelsof … local()` filled) cannot be unrolled** — see B.3.

### A.4 What is *not* compile-time

`local n = _N`, `local m = r(mean)`, `local lvls : levelsof …` and similar are
**not** constant-foldable, because the right-hand side depends on data. These are
Category B: the macro becomes an alias for a SQL fragment, not a literal. If a
non-foldable value is used in a position that requires a literal at compile time
(e.g. a loop bound, `forvalues i = 1/`n'` where `` `n' `` came from `_N`), the
compiler raises a clear error: *"loop bound depends on a runtime value; unroll is
impossible — rewrite set-based (see VARIABLE_SUBSTITUTION.md §B.3)."*

---

## Category B — Runtime-dependent values (compiled to SQL)

These cannot be known while compiling. Instead of fetching them, dodo **rewrites
each one into the SQL** so DuckDB computes it. There is no execution round-trip.

### B.1 Stored results `r()` become SQL subqueries

When the compiler processes an r-class terminal command, it records, in state, a
map from each `r(...)` symbol to a **SQL subquery string** evaluated against the
**latest CTE step at that point** (the snapshot the command saw — matching Stata,
where `r()` holds the value as of the command).

`summarize revenue` (latest step `_s3`) records, schematically:

| Symbol | Recorded SQL fragment |
|---|---|
| `r(N)` | `(SELECT count(revenue) FROM _s3)` |
| `r(mean)` | `(SELECT avg(revenue) FROM _s3)` |
| `r(sum)` | `(SELECT sum(revenue) FROM _s3)` |
| `r(min)` | `(SELECT min(revenue) FROM _s3)` |
| `r(max)` | `(SELECT max(revenue) FROM _s3)` |
| `r(sd)` | `(SELECT stddev_samp(revenue) FROM _s3)` |
| `r(Var)` | `(SELECT var_samp(revenue) FROM _s3)` |

Likewise `count` records `r(N) ↦ (SELECT count(*) FROM _sK)`.

A later reference to `r(max)` (recognized during expression translation, not the
textual pass) is replaced by its recorded fragment:

```stata
summarize revenue          // records r(...) against _s3
generate hi = revenue == r(max)
keep if revenue >= r(mean)
```

→ each `r(...)` becomes its frozen subquery. Because `_s3` is an earlier link in
the same chain, any later step's `WITH` prefix still contains it, so the subquery
resolves correctly — and it references the data *as summarize saw it*, even if
later commands drop columns or rows.

**`r()` volatility.** Every new r-class command clears the recorded map before
recording its own (Stata's `r()` is overwritten by the next r-class command). A
reference to a stale `r(...)` is an error: *"r(max) is not set; the most recent
r-class command was `count`."*

### B.2 Scalars and macros bound to runtime values

`scalar hi = r(max)` or `local hi = r(max)` does not store a number — it stores the
**SQL fragment** that `r(max)` currently maps to (snapshotted at definition time).
A later bare reference to `hi` (scalar) or `` `hi' `` (macro) expands to that
fragment. This unifies scalars and macros under the same "name → SQL fragment"
mechanism; the only difference from Category A is that the bound text is a subquery
rather than a literal.

### B.3 `levelsof` and set-based rewrites

`levelsof x, local(L)` produces a list whose **length and contents are unknown at
compile time**. It therefore cannot fill a compile-time local, and a
`foreach … of local L` over it cannot be unrolled. dodo handles the common idioms
by mapping them to **set-based SQL** instead of loops:

| Stata idiom | SQL rewrite |
|---|---|
| `levelsof x, local(L)` then `keep if inlist(y, `L')` | `… WHERE y IN (SELECT DISTINCT x FROM _sK)` |
| `levelsof x` used only for membership | subquery `IN (SELECT DISTINCT x FROM _sK)` |
| `foreach v of local L { gen d_`v' = x==`v' }` (one column per value) | `PIVOT`-style generation — **only if** the value set can be reified; otherwise rejected |
| `foreach … { append/stack }` | `GROUP BY` / `UNPIVOT` where expressible |

`levelsof x, local(L)` records `L` as a runtime-list symbol bound to
`(SELECT DISTINCT x FROM _sK ORDER BY x)`. References that resolve to set
membership compile cleanly. References that genuinely require iterating an unknown
number of times (e.g. generating one new column per distinct value, where the
distinct values can't be reified at compile time) are **out of scope for the pure
compiler** and produce an explicit, actionable error rather than silently wrong
SQL.

### B.4 `e()` (estimation results)

`e(...)` follows the same "compile to SQL fragment" rule as `r()`, but depends on
`regress`/`reghdfe`, which are themselves stretch goals (M9). Deferred; the
mechanism is identical to B.1 once estimation exists.

---

## Why not just run the query and paste the value back? (Rejected: the round-trip)

A tempting alternative: when the extension hits `summarize revenue`, actually
execute it, read `max`, and substitute the literal `42` into later commands.

We reject this:

1. **It breaks `dodoc`.** `dodoc` has no database and never executes anything. A
   round-trip would make the standalone compiler strictly less capable than the
   extension, splitting the language in two. Keeping runtime values as SQL means
   *one* compiler with *one* semantics.
2. **It breaks laziness.** Materializing mid-script to read a scalar forces
   execution of the whole chain early, defeating the lazy-CTE design and changing
   performance characteristics unpredictably.
3. **It changes semantics under edits.** A pasted literal is frozen to the data at
   compile time; a subquery re-evaluates against the actual chain. The subquery is
   the more faithful and the more composable choice.
4. **It is impure.** Compilation would depend on live data, so the same `.do` file
   could compile to different SQL on different days. The subquery approach makes
   compilation deterministic.

The only thing dodo *cannot* do without a round-trip is unroll a loop over a
runtime list (B.3, the rejected column-per-value case). That is a deliberate,
documented boundary, not a bug.

---

## State changes (`DodoState`)

```cpp
// --- Category A: compile-time textual macros ---
std::unordered_map<std::string, std::string> locals;   // name -> literal text
std::unordered_map<std::string, std::string> globals;  // name -> literal text

// --- Category B: name -> SQL fragment (literal OR subquery) ---
std::unordered_map<std::string, std::string> scalars;        // scalar name -> SQL expr
std::unordered_map<std::string, std::string> stored_results; // "r(max)" -> "(SELECT ...)"
std::unordered_map<std::string, std::string> runtime_lists;  // levelsof local -> "(SELECT DISTINCT ...)"

// the command that last populated r(), for good error messages and volatility
std::string last_rclass_command;
```

Scoping: `locals` live for the duration of a `do`-file (and loop bodies); push a
frame on `do`/loop entry and pop on exit. `globals`, `scalars`, and
`stored_results` are session-scoped (cleared by `clear`). All maps are reset by
`clear`/`ClearAll` alongside the existing fields.

---

## Pipeline placement

```
raw line(s)
  └─ join /// continuations, strip comments        (ProcessDoFile, exists)
       └─ LOOP UNROLLING (forvalues/foreach)        ── NEW, Category A.3
            └─ MACRO EXPANSION ($g, `l', `=exp')     ── NEW, Category A.2
                 └─ TokenizeCommand                  (exists)
                      └─ ProcessCommand              (exists)
                           └─ TranslateExpression    (exists)
                                └─ resolve r()/e()/runtime scalars ── NEW, Category B
```

Category A is a string-rewriting front pass shared by both `dodoc` and the
extension. Category B lives inside expression translation and the terminal-command
handlers (which already exist for `summarize`/`count`), so it too is shared.

---

## Milestone split

- **M14a — Compile-time macros & loops (Category A).** `local`, `global`,
  literal `scalar`, `` `x' ``/`$x`/`` `=…' `` expansion, `forvalues`, `foreach`
  over literal/varlist/known-local lists. No data dependency; fully testable in
  `dodoc`.
- **M14b — Runtime stored results (Category B).** `r()` → frozen subqueries from
  `summarize`/`count`, runtime-bound scalars/locals, `levelsof` → set-membership
  rewrites, error paths for impossible unrolls. `e()` deferred with M9.

---

## Test plan

Compile-time (M14a), checkable purely with `dodoc` (text → SQL):

- `local controls age educ` → `regress`/`keep `controls'` expands to the varlist.
- `forvalues i = 1/3 { gen x`i' = `i' }` → three `generate` steps.
- `foreach v of varlist a b { gen ln_`v' = log(`v') }` → two steps.
- `global base "data"; use "`$base'/firms.csv"` → path expansion.
- Nested `` `x`i'' `` resolves innermost-first.
- `forvalues i = 1/`n'` where `` `n' `` is runtime → **error** (A.4).

Runtime (M14b):

- `summarize revenue; keep if revenue == r(max)` → subquery against the summarize
  step; verify the kept rows.
- `count if year==2020; gen share = _N / r(N)` — note `count`'s `r(N)`.
- `scalar hi = r(max)` then `keep if x == hi` → same subquery via the scalar.
- Stale `r()`: `summarize a; count; di r(max)` → error mentioning `count`.
- `levelsof year, local(yrs); keep if inlist(year, `yrs')` →
  `year IN (SELECT DISTINCT year FROM _sK)`.
- Round-trip determinism: the same `.do` compiles to identical SQL regardless of
  data (guards against accidental round-tripping).
</content>
</invoke>
