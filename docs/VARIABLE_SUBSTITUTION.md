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

### B.0 The kind of a name is a property of its *definition*, not its use

Whether a name is compile-time or runtime **cannot be decided from the line that
uses it.** Consider:

```stata
summarize employment           // r(min) recorded against _s3
scalar min_employment = r(min) // <-- this makes min_employment RUNTIME
keep if employment > min_employment
```

Looking at `keep if employment > min_employment` in isolation, `min_employment` is
indistinguishable from a column. It is runtime only *because* of how it was
defined two lines earlier. So the compiler must carry a **compile-time symbol
table** — a namespace that records, for every macro/scalar it has seen, which
**kind** it is and what it expands to:

```
kind ∈ { LITERAL,  RUNTIME }
       (compile-time text)  (SQL fragment / subquery)
```

| | `LITERAL` | `RUNTIME` |
|---|---|---|
| stored value | raw text (`"age educ"`, `"2020"`) | an SQL fragment (`(SELECT min(employment) FROM _s3)`) |
| resolved by | textual substitution (Category A) | expression translation (this section) |
| set by | `local x text`, `scalar x = const`, foldable `= exp` | RHS that touches **any** runtime symbol |

**Taint propagation.** A definition is `RUNTIME` **iff its right-hand side, after
expansion, references at least one runtime symbol** — `r(...)`, `e(...)`, a
`levelsof` list, or a name already bound `RUNTIME`. The taint is transitive and
the fragment composes:

```stata
scalar min_employment = r(min)      // RUNTIME: (SELECT min(employment) FROM _s3)
scalar floor = min_employment - 1   // RUNTIME: ((SELECT min(employment) FROM _s3) - 1)
local big = floor * 2               // RUNTIME: (((SELECT ...) - 1) * 2)
```

Everything else (`local controls age educ`, `scalar pi = 3.14159`,
`local n2 = 4*4`) stays `LITERAL` and is handled by Category A.

**Name resolution at a use site.** When translating an expression, a bare
identifier is looked up in the scalar namespace first: if it is a known `RUNTIME`
scalar, substitute its fragment; if a known `LITERAL` scalar, substitute its
literal; otherwise treat it as a **column reference** and leave it alone. (Stata
lets a variable and a scalar share a name; dodo resolves scalar-table hits first
and the docs recommend distinct names to avoid surprise.) Delimited macro refs
`` `x' ``/`$x` are still resolved in the textual pass — but a `RUNTIME` macro
pastes its *SQL fragment text*, which then flows into expression translation as a
subquery, so the two namespaces meet cleanly.

This symbol table is the single source of truth for the A-vs-B decision; the rest
of Category B (`r()`, `levelsof`) just *populates* it.

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

`scalar hi = r(max)` or `local hi = r(max)` does not store a number — it records a
`RUNTIME` symbol (§B.0) whose value is the **SQL fragment** that `r(max)` currently
maps to, snapshotted at definition time. A later bare reference to `hi` (scalar) or
`` `hi' `` (macro) expands to that fragment. Scalars and macros thus share one
symbol-table mechanism; the only difference from Category A is the `kind` flag —
`RUNTIME` symbols carry a subquery, `LITERAL` symbols carry text — and the
resolution site (expression translation vs the textual pass).

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

## Stretch goal: named result structs via `let`

`r(min)` is legacy: it is **anonymous** (one shared bucket), **volatile** (the next
r-class command wipes it), and **opaque** (you must know Stata's per-command result
list). We keep it for compatibility with existing `.do` files, but offer a
dodo-native alternative that is named, stable, and self-documenting.

### Syntax and why `let`

```stata
let result = summarize employment
keep if employment > result.min
generate z = (employment - result.mean) / result.sd
```

`let` binds the **stored results of a command to a named struct**, and the fields
(`result.min`, `result.mean`, `result.N`, …) are referenced by dotted access.

Why a keyword: dodo's parser dispatches on the **first token of every line** —
that is the invariant that lets the extension distinguish a dodo command from raw
SQL. A bare `result = summarize employment` would begin with an identifier and
break that invariant (and collide visually with SQL). `let` joins `DODO_COMMANDS`
like any other verb, so the line routes cleanly: the handler strips `let`, reads
`<name> =`, and runs the remainder as a captured command. This mirrors the
keyword-led design of `local`, `scalar`, `egen`, etc.

### Semantics — it is just a named, non-volatile §B.1

`let NAME = <r-class command>` runs the command in **capture mode**: instead of
materializing output, it records the command's result fields as `RUNTIME` symbols
(§B.0) under the namespace `NAME`, each bound to the same frozen subquery B.1 would
produce against the step the command saw.

| `let result = summarize employment` (step `_s3`) | resolves to |
|---|---|
| `result.N` | `(SELECT count(employment) FROM _s3)` |
| `result.mean` | `(SELECT avg(employment) FROM _s3)` |
| `result.min` / `result.max` | `(SELECT min/max(employment) FROM _s3)` |
| `result.sd` | `(SELECT stddev_samp(employment) FROM _s3)` |

`result.min` is resolved exactly like a bare runtime scalar (§B.0): a lookup in the
symbol table during expression translation, substituting the fragment. Taint
propagates normally — `scalar lo = result.min - 1` is `RUNTIME`.

Two properties make it strictly better than `r()`:

1. **Not volatile.** A later `summarize`/`count` does **not** disturb `result`.
   You can hold several at once:
   ```stata
   let emp = summarize employment
   let rev = summarize revenue
   keep if employment > emp.min & revenue > rev.mean
   ```
   This is impossible with `r()`, where the second `summarize` overwrites the first.
2. **Validated fields.** `result.median` when `summarize` did not compute it is a
   **compile-time error** listing the available fields — no silent NULLs.

### Unifying `r()` with `let`

`r()` becomes the special case of an **implicit struct named `r`** that every
r-class command reassigns: `summarize x` is sugar for `let r = summarize x`, and
`r(min)` is just `r.min` with legacy `()` access. One recording mechanism backs
both surfaces; the only difference is that `r` is auto-reassigned (hence volatile)
while a user-named `let` struct is stable. This keeps the implementation single and
makes the legacy/modern relationship obvious in the docs.

### Scope, state, parity

- A struct is a `RUNTIME` namespace in the symbol table — reuse the §B.0 machinery;
  add a `std::unordered_map<std::string, std::unordered_map<std::string,std::string>> result_structs;`
  (`name -> {field -> fragment}`), or fold dotted names straight into `scalars`.
- **Scope:** session-scoped like `scalar`/`global`, cleared by `clear`. (Open
  question: do-file vs session scope — leaning session, since a named result is
  meant to outlive a block. Flag for sign-off.)
- **Pure compile-time, dodoc-safe.** `let` records fragments and `.field` access
  emits subqueries; nothing executes, so `dodoc` supports it with no database —
  same parity guarantee as the rest of Category B.
- **Sequencing after M14b**, since it is a thin, named re-skin of the `r()`
  recording it depends on.

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

Every macro/scalar lives in one symbol table whose entries carry the kind flag
from §B.0, so the A-vs-B decision is a single lookup:

```cpp
enum class BindingKind { LITERAL, RUNTIME };

struct Symbol {
    BindingKind kind;   // LITERAL = compile-time text; RUNTIME = SQL fragment
    std::string value;  // raw text  (LITERAL)  |  "(SELECT max(x) FROM _s3)"  (RUNTIME)
};

// the three Stata namespaces, each a name -> Symbol map
std::unordered_map<std::string, Symbol> locals;   // `x'    (do-file / loop scoped)
std::unordered_map<std::string, Symbol> globals;  // $x     (session)
std::unordered_map<std::string, Symbol> scalars;  // bare x (session)

// r()/e() and levelsof lists: always RUNTIME, populated by terminal commands
std::unordered_map<std::string, std::string> stored_results; // "r(max)" -> "(SELECT ...)"
std::unordered_map<std::string, std::string> runtime_lists;  // levelsof local -> "(SELECT DISTINCT ...)"

// the command that last populated r(), for good error messages and volatility
std::string last_rclass_command;
```

`Define(table, name, rhs)` classifies once: scan the expanded `rhs` for any
runtime symbol (`r()`, `e()`, a `RUNTIME` entry in any table, a `runtime_lists`
name); if found, store `{RUNTIME, fragment}` with runtime refs replaced by their
fragments; otherwise store `{LITERAL, foldedText}`. That is where taint
propagation (§B.0) lives.

Scoping: `locals` live for the duration of a `do`-file (and loop bodies); push a
frame on `do`/loop entry and pop on exit. `globals`, `scalars`, `stored_results`,
and `runtime_lists` are session-scoped. All are reset by `clear`/`ClearAll`
alongside the existing fields.

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
- **M14c — Named result structs via `let` (stretch).** `let name = <command>`
  capture, dotted `name.field` access, field validation, and unifying `r()` as the
  implicit `r` struct. Thin re-skin of M14b's recording; sequenced after it.

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
- **Indirection (§B.0):** `summarize employment; scalar min_employment = r(min);
  keep if employment > min_employment` → `min_employment` resolves to the
  `(SELECT min(employment) FROM _sN)` subquery even though the use site looks like
  a plain column comparison.
- **Transitive taint:** `scalar floor = min_employment - 1` is RUNTIME and nests
  the inner subquery; `scalar pi = 3.14; keep if x > pi` stays LITERAL.
- Name resolution: a bare name that is *not* a defined scalar is left as a column
  reference, not mistaken for a macro.
- Stale `r()`: `summarize a; count; di r(max)` → error mentioning `count`.
- `levelsof year, local(yrs); keep if inlist(year, `yrs')` →
  `year IN (SELECT DISTINCT year FROM _sK)`.
- Round-trip determinism: the same `.do` compiles to identical SQL regardless of
  data (guards against accidental round-tripping).
</content>
</invoke>
