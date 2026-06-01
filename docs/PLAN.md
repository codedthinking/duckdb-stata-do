# Dodo — Implementation Plan

## Context

We are building a DuckDB community extension called **`dodo`** that lets users write a subset of Stata `.do` file commands directly inside DuckDB. The extension implements the same commands as [Kezdi.jl](https://github.com/codedthinking/Kezdi.jl), translating them to SQL under the hood.

**Key design principle:** Commands do NOT mutate any table. Instead, each command appends a SQL transformation (CTE step) to a running query chain. The chain is only materialized when a "terminal" command (`list`, `summarize`, `tabulate`, `count`, `save`, `regress`) is executed. Terminal commands do NOT consume or reset the chain — you can keep transforming after a `list`. This aligns perfectly with DuckDB's lazy evaluation model.

---

## Architecture

### Core Concept: The CTE Chain

Transformation commands accumulate as a chain of CTEs stored in per-connection state. Nothing executes until a terminal command (`list`, `summarize`, `count`, etc.) triggers materialization.

```
use "firms.csv"                    →  CTE chain: [_s0 AS (SELECT * FROM read_csv('firms.csv'))]
keep id revenue year               →  CTE chain: [..., _s1 AS (SELECT id, revenue, year FROM _s0)]
keep if year == 2018               →  CTE chain: [..., _s2 AS (SELECT * FROM _s1 WHERE year = 2018)]
generate ln_rev = log(revenue)     →  CTE chain: [..., _s3 AS (SELECT *, LN(revenue) AS ln_rev FROM _s2)]
summarize ln_rev                   →  EXECUTE: WITH _s0 AS (...), _s1 AS (...), _s2 AS (...), _s3 AS (...)
                                              SELECT COUNT(ln_rev), AVG(ln_rev), ... FROM _s3
```

**Why CTEs, not views:**
- No catalog side effects — no view creation, no naming conflicts
- DuckDB optimizes the full CTE chain at once (better than nested views)
- Naturally lazy — nothing runs until a terminal command
- Clean state: `clear` just empties the chain, no `DROP VIEW` needed

### Extension Integration

Follow the **duckdb-prql pattern**:
1. **ParserExtension** — regex-detect Stata commands (lines starting with `use`, `keep`, `drop`, `generate`, etc.)
2. **Parse function** — transform Stata syntax → equivalent SQL statement(s)
3. **Plan function** — hand the SQL back to DuckDB's standard parser/binder
4. **ClientContextState** — store the CTE chain and step counter per connection

### State (per connection via `ClientContextState`)

```cpp
class DodoState : public ClientContextState {
    vector<string> cte_chain;       // each entry: "_sN AS (SELECT ... FROM _sN-1)"
    int step_counter = 0;           // incremented per transformation command
    string current_source;          // original table/file for debugging

    // Build the full WITH clause from the chain
    string BuildCTEPrefix() const;
    // Get the name of the latest step
    string LatestStep() const;
    // Add a new step
    void AddStep(const string &sql);
    // Reset everything
    void Clear();
};
```

Terminal commands call `BuildCTEPrefix()` + their own `SELECT ... FROM <LatestStep()>` to produce a complete SQL query that DuckDB executes in one shot.

---

## Command Classification & SQL Mapping

### Transformation Commands (append CTE step to chain)

Each command adds a new `_sN AS (...)` referencing the previous `_sN-1`. Shown below as the inner SELECT only.

| Stata Command | CTE Step SQL |
|---|---|
| `use "file.csv", clear` | Resets chain. `_s0 AS (SELECT * FROM read_csv('file.csv'))` |
| `use "file.dta", clear` | Resets chain. `_s0 AS (SELECT * FROM read_dta('file.dta'))` |
| `use tablename, clear` | Resets chain. `_s0 AS (SELECT * FROM tablename)` |
| `keep var1 var2` | `SELECT var1, var2 FROM _prev` |
| `keep if expr` | `SELECT * FROM _prev WHERE expr` |
| `keep var1 var2 if expr` | `SELECT var1, var2 FROM _prev WHERE expr` |
| `drop var1 var2` | `SELECT * EXCLUDE (var1, var2) FROM _prev` |
| `drop if expr` | `SELECT * FROM _prev WHERE NOT (expr)` |
| `generate y = expr` | `SELECT *, (expr) AS y FROM _prev` |
| `generate y = expr if cond` | `SELECT *, CASE WHEN cond THEN expr ELSE NULL END AS y FROM _prev` |
| `replace y = expr` | `SELECT * REPLACE ((expr) AS y) FROM _prev` |
| `replace y = expr if cond` | `SELECT * REPLACE (CASE WHEN cond THEN expr ELSE y END AS y) FROM _prev` |
| `rename old new` | `SELECT * EXCLUDE (old), old AS new FROM _prev` |
| `sort var1 var2` | `SELECT * FROM _prev ORDER BY var1, var2` |
| `sort var1, desc` | `SELECT * FROM _prev ORDER BY var1 DESC` |
| `egen y = func(x), by(g)` | `SELECT *, func(x) OVER (PARTITION BY g) AS y FROM _prev` |
| `collapse (mean) y = x, by(g)` | `SELECT g, AVG(x) AS y FROM _prev GROUP BY g` |
| `mvencode var1 var2, mv(0)` | `SELECT * REPLACE (COALESCE(var1, 0) AS var1, COALESCE(var2, 0) AS var2) FROM _prev` |
| `append using "file.csv"` | `SELECT * FROM _prev UNION ALL BY NAME SELECT * FROM read_csv('file.csv')` |
| `reshape long y, i(id) j(t)` | `UNPIVOT _prev ON ... INTO NAME t VALUE y` |
| `reshape wide y, i(id) j(t)` | `PIVOT _prev ON t USING FIRST(y) GROUP BY id` |
| `clear` | Clears the chain entirely (resets state) |
| `undo [N]` | Remove last N steps (default 1), push to redo stack |
| `redo [N]` | Re-apply last N undone steps (default 1) |

### Terminal Commands (materialize the CTE chain + final SELECT)

These prepend the full `WITH _s0 AS (...), _s1 AS (...), ...` then append their own query against the latest step.

| Stata Command | Final SELECT (after WITH prefix) |
|---|---|
| `list` | `SELECT * FROM _sN` |
| `list var1 var2` | `SELECT var1, var2 FROM _sN` |
| `list if expr` | `SELECT * FROM _sN WHERE expr` |
| `count` | `SELECT COUNT(*) AS n FROM _sN` |
| `count if expr` | `SELECT COUNT(*) AS n FROM _sN WHERE expr` |
| `summarize var` | `SELECT COUNT(var), AVG(var), STDDEV(var), MIN(var), PERCENTILE_CONT(0.25)... FROM _sN` |
| `tabulate var1 var2` | `SELECT var1, var2, COUNT(*) FROM _sN GROUP BY var1, var2 ORDER BY var1, var2` |
| `describe` | Special: `WITH ... SELECT * FROM _sN LIMIT 0` then use result metadata, or `DESCRIBE (WITH ... SELECT * FROM _sN)` |
| `head [n]` | `SELECT * FROM _sN LIMIT n` |
| `tail [n]` | `SELECT * FROM (SELECT *, ROW_NUMBER() OVER () AS _rn, COUNT(*) OVER () AS _total FROM _sN) WHERE _rn > _total - n` |
| `history` | `SELECT step_id, command, undone, ts FROM dodo._history` |
| `save "file.csv", replace` | `COPY (WITH ... SELECT * FROM _sN) TO 'file.csv'` |
| `save "file.parquet", replace` | `COPY (WITH ... SELECT * FROM _sN) TO 'file.parquet'` |
| `regress y x1 x2` | Stretch goal — needs stats extension or custom implementation |

### Special Variables

| Variable | SQL Equivalent |
|---|---|
| `_n` | `ROW_NUMBER() OVER ()` (or `OVER (PARTITION BY g)` with `by()`) |
| `_N` | `COUNT(*) OVER ()` (or `OVER (PARTITION BY g)` with `by()`) |
| `_all` | All columns (expand to `*` or column list as needed) |

### Expression Translation

| Stata | SQL |
|---|---|
| `==` | `=` |
| `&` | `AND` |
| `\|` | `OR` |
| `!` or `~` | `NOT` |
| `log(x)` | `LN(x)` |
| `abs(x)` | `ABS(x)` |
| `missing(x)` | `x IS NULL` |
| `cond(a, b, c)` | `CASE WHEN a THEN b ELSE c END` |
| `round(x, d)` | `ROUND(x, d)` |
| `substr(s, i, n)` | `SUBSTRING(s, i, n)` |
| `strlen(s)` | `LENGTH(s)` |
| `strlower(s)` | `LOWER(s)` |
| `strupper(s)` | `UPPER(s)` |
| `strtrim(s)` | `TRIM(s)` |
| `sum(x)` (in egen) | `SUM(x) OVER (...)` |
| `mean(x)` (in egen) | `AVG(x) OVER (...)` |
| `min(x)` (in egen) | `MIN(x) OVER (...)` |
| `max(x)` (in egen) | `MAX(x) OVER (...)` |
| `count(x)` (in egen) | `COUNT(x) OVER (...)` |
| `sd(x)` (in egen) | `STDDEV(x) OVER (...)` |

---

## Milestones

### M0: Scaffolding (rename template, build infrastructure)
- Rename `waddle` → `dodo` everywhere (CMakeLists.txt, extension_config.cmake, Makefile, source files, headers)
- Remove OpenSSL dependency
- Set up `DodoParserExtension` + `DodoOperatorExtension` skeleton (following prql pattern)
- Set up `DodoState` as `ClientContextState`
- Verify it builds and loads (empty extension, no commands yet)
- **Test:** `LOAD dodo; SELECT 1;` works

### M1: `use` and `list` (the minimal loop)
- Implement Stata command tokenizer: split input into `(command, arguments, condition, options)`
- Implement `use "file.csv", clear` → reset chain, `_s0 AS (SELECT * FROM read_csv('file.csv'))`
- Implement `list` → materialize chain with `SELECT * FROM _sN`
- Implement `clear` → empty the CTE chain
- **Test:** `use "test.csv", clear` then `list` returns the data

### M2: `keep`, `drop`, `rename` (row/column filtering)
- `keep var1 var2` — column selection
- `keep if expr` — row filtering (basic expressions: `==`, `!=`, `<`, `>`, `&`, `|`)
- `keep var1 var2 if expr` — both
- `drop var1 var2` — column removal (DuckDB `EXCLUDE`)
- `drop if expr` — row removal
- `rename old new` — column rename
- **Test:** Chain of `use` → `keep` → `drop` → `list` produces correct output

### M3: `generate`, `replace` (column creation/mutation)
- `generate y = expr` — add column
- `generate y = expr if cond` — conditional column (NULL where false)
- `replace y = expr` — overwrite column (DuckDB `REPLACE`)
- `replace y = expr if cond` — conditional overwrite
- Expression translator: arithmetic, comparison, basic functions (`log`, `abs`, `round`, `missing`)
- `cond(test, a, b)` → `CASE WHEN ... THEN ... ELSE ... END`
- **Test:** `generate ln_wage = log(wage)` then `list` shows new column

### M4: `sort`, `order`, `count`, `describe`, `head`, `tail`
- `sort var1 var2` / `sort var1, desc`
- `order var1 var2` — column reordering
- `count` / `count if expr` — terminal
- `describe` / `describe var1` — terminal
- `head [n]` / `tail [n]` — terminal
- **Test:** Sorting and counting work correctly

### M5: `egen`, `collapse` with `by()` (grouping)
- Parse `by(var1, var2)` in options
- `egen y = mean(x), by(g)` → window function `AVG(x) OVER (PARTITION BY g)`
- `collapse (mean) y = x, by(g)` → `SELECT g, AVG(x) AS y FROM _prev GROUP BY g`
- Support aggregate functions: `sum`, `mean`, `min`, `max`, `count`, `sd`
- Special variables `_n`, `_N` (as window functions)
- **Test:** `egen` with `by()` produces per-group aggregates; `collapse` reduces rows

### M5.5: `do "script.do"` — run .do files without semicolons
DuckDB requires `;` to terminate statements, but Stata uses newlines. Register a `do "script.do"` command that reads a .do file and processes it line by line.

**Design:**
- `do` is a Stata command handled by the parser extension like any other
- `ProcessCommand` reads the file, strips Stata comments (`//`, `*`, `/* */`), handles line continuations (`///`)
- Calls `ProcessCommand` recursively for each non-empty line
- Transformation commands update the CTE chain state silently
- Terminal commands in the script are skipped (user inspects results interactively after `do` finishes, matching Stata behavior)
- Returns `SELECT 'OK' AS status`
- **Test:** Write a .do file with transformations, run `do "test.do";`, then `list;` to verify

### M6: `summarize`, `tabulate` (statistical output)
- `summarize var` — compute N, mean, sd, min, p25, p50, p75, max
- `summarize var if cond` — filtered summary
- `tabulate var1` — frequency table
- `tabulate var1 var2` — cross-tabulation
- Format output nicely (Stata-style table display)
- **Test:** `summarize` output matches expected statistics

### M7: `save`, `append`, `mvencode` (I/O and cleanup)
- `save "file.csv", replace` → `COPY (WITH ... SELECT * FROM _sN) TO 'file.csv'`
- `save "file.parquet", replace` → `COPY (WITH ... SELECT * FROM _sN) TO 'file.parquet'`
- `append using "file.csv"` → `UNION ALL BY NAME`
- `mvencode var1 var2, mv(0)` → `COALESCE`
- **Test:** Round-trip `use` → transform → `save` → `use` produces same data

### M8: `reshape` (pivot/unpivot)
- `reshape long y, i(id) j(t)` → DuckDB `UNPIVOT`
- `reshape wide y, i(id) j(t)` → DuckDB `PIVOT`
- **Test:** Wide-to-long and back produces expected shape

### M9: `regress` (stretch goal)
- OLS regression: either implement directly or depend on a stats extension
- `regress y x1 x2, robust` — basic OLS with robust SE
- `regress y x1 x2, cluster(id)` — clustered SE
- This may require a separate C++ stats library or duckdb-stats extension
- **Decision point:** may defer or implement basic OLS only

### M9.5: Labels and `describe` (DuckLake-compatible metadata)

Stata has rich column metadata: variable labels (describing what a column is) and value labels (mapping integers to human-readable strings). The [stata2ducklake](https://github.com/codedthinking/stata2ducklake) project established conventions for storing this metadata in DuckLake:

- **Variable labels** → `COMMENT ON COLUMN table.col IS 'label'`
- **Value labels** → `value_label_<name>` lookup tables with `(value INTEGER, label VARCHAR)`
- **Column↔label mapping** → `_column_value_labels(table_name, column_name, label_name)`
- **Macros** → `labels(tbl)` shows all metadata, `decode(lbl, val)` resolves a value label

**Commands to implement:**

1. **`label variable varname "label text"`**
   - Store in per-connection state: `DodoStateInfo.variable_labels[varname] = label`
   - When `save` is called to a DuckLake catalog, emit `COMMENT ON COLUMN`
   - In the CTE chain: labels are metadata-only, no SQL step needed
   - Returns `SELECT 'OK'`

2. **`label define labelname value1 "text1" value2 "text2" ...`**
   - Store in state: `DodoStateInfo.value_label_defs[labelname] = {val: text, ...}`
   - These define the mapping but don't attach it to a column yet

3. **`label values varname labelname`**
   - Store in state: `DodoStateInfo.column_labels[varname] = labelname`
   - Attaches a value label definition to a specific column

4. **`describe` (enhanced)**
   - Current: shows `column_name, column_type`
   - Enhanced: also shows variable label and value label name from state
   - SQL: join column metadata with in-memory label state
   - Since labels live in state (not in the CTE chain), `describe` builds the output by:
     a. Getting column names/types from the CTE chain via `DESCRIBE`
     b. Left-joining with the in-memory variable_labels and column_labels maps
     c. Returning `column_name, column_type, variable_label, value_label`

5. **`decode varname`** (bonus, Stata-like)
   - If varname has an attached value label, replace integer values with label text
   - SQL: join with the value label definition
   - `generate decoded_var = decode(labelname, varname)` or automatic via the lookup

**State changes to `DodoStateInfo`:**
```cpp
struct DodoStateInfo : public ParserExtensionInfo {
    // ... existing CTE chain fields ...

    // Variable labels: column_name -> label text
    unordered_map<string, string> variable_labels;
    // Value label definitions: label_name -> {value -> text}
    unordered_map<string, unordered_map<int, string>> value_label_defs;
    // Column-to-value-label mapping: column_name -> label_name
    unordered_map<string, string> column_labels;
};
```

**DuckLake integration on `save`:**
When saving to a DuckLake-attached catalog (not a plain file), emit:
- `COMMENT ON COLUMN` for each variable label
- `CREATE TABLE value_label_<name>` for each value label definition
- `INSERT INTO _column_value_labels` for each column↔label mapping

**Test plan:**
- `label variable revenue "Annual revenue in USD"`
- `label define yesno 0 "No" 1 "Yes"`
- `label values employed yesno`
- `describe` shows labels
- Round-trip: save to DuckLake, reload, labels preserved

### M10: Polish & Community Extension Submission
- Write `description.yml` for community extensions repo
- README with examples
- Comprehensive test suite
- CI/CD via `.github/workflows`
- Handle edge cases: missing `_current`, invalid syntax errors, type mismatches
- Performance testing with larger datasets

---

## Phase 2: Real-World Compatibility

Based on commands used in [korenmiklos/ceo-value](https://github.com/korenmiklos/ceo-value/tree/main/lib/create). See `docs/MISSING_COMMANDS.md` for full inventory.

### M11: Expression functions (`cond`, `inrange`, `inlist`, `substr`, `real`)
- `cond(test, a, b)` → `CASE WHEN test THEN a ELSE b END`
- `inrange(x, lo, hi)` → `x BETWEEN lo AND hi`
- `inlist(x, a, b, c)` → `x IN (a, b, c)`
- `substr(s, start, len)` → `SUBSTRING(s, start, len)` (Stata is 1-indexed, same as SQL)
- `real(s)` → `CAST(s AS DOUBLE)`
- `int(x)` → `CAST(x AS INTEGER)`
- Add `firstnm` to collapse aggregate functions → `FIRST(x)`
- **Test:** expressions from balance.do: `real(substr(frame_id, 3, .))`, `cond(missing(L.assets), ...)`

### M12: `merge` (joins)
- `merge 1:1 varlist using "file"` → inner/left join on varlist
- `merge m:1 varlist using "file"` → left join
- `merge 1:m varlist using "file"` → left join (other direction)
- Options: `keep(match)` → INNER JOIN, `keep(master match)` → LEFT JOIN, `nogen` → exclude `_merge` column
- The `using` file is loaded via `read_csv`/`read_parquet`/etc. (same as `use`)
- SQL: `SELECT * FROM _prev JOIN read_csv('file') USING (varlist)`
- `joinby varlist using "file"` → unrestricted cross join on matching keys
- **Test:** merge m:1 from ceo-panel.do, merge 1:1 from unfiltered.do

### M13: `duplicates drop`, `expand`, `export delimited`
- `duplicates drop` → `SELECT DISTINCT * FROM _prev`
- `duplicates drop varlist` → deduplicate on specific columns (keep first)
- `expand N` → replicate each row N times (N can be a variable)
  - SQL: `SELECT * FROM _prev, GENERATE_SERIES(1, N) AS t(idx)` then drop idx
  - `expand N, generate(newvar)` → keep the index as a new 0/1 variable
- `export delimited using "file", replace` → `COPY ... TO 'file' (FORMAT CSV, HEADER)`
- `import delimited "file", clear` → same as `use "file.csv", clear`
- **Test:** duplicates drop from edgelist.do, expand from ceo-panel.do

### M14: Local/global macros, scalars, and loops

**Macros:**
- `local name value` / `local name = expr` / `local name "string"` → store in `DodoState.local_macros`
- `global name value` / `global name = expr` → store in `DodoState.global_macros`
- `` `name' `` expansion (locals), `$name` / `${name}` expansion (globals) — via `ExpandMacros()` pre-processing pass before command recognition
- `local ++name` / `local --name` — increment/decrement numeric macros
- `macro drop name [name ...] | _all`
- Nested backtick-quote expansion with depth tracking (e.g., `` `:word count `sentence'' ``)

**Scalars:**
- `scalar [define] name = expr` → stores in both `state.scalars` and `state.local_macros`
- Bare scalar names expanded in expressions via `ExpandMacros()` (e.g., `keep if year > threshold`)
- `scalar list` / `scalar drop names | _all`
- Simple expression evaluator (`EvaluateSimpleExpr`) for `+`, `-`, `*`, `/`, parens, `int()`, `round()`, `sqrt()`, `ln()`, `exp()`, `abs()`, `ceil()`, `floor()`

**Loops:**
- `foreach lname in list { body }` — arbitrary word list
- `foreach lname of local macname { body }` — expand local macro as list
- `foreach lname of global macname { body }` — expand global macro as list
- `foreach lname of numlist spec { body }` — numeric list via `ParseNumlist()` (`1/5`, `1(2)10`, etc.)
- `forvalues lname = range { body }` — `#(#)#` and `#/#` range syntax
- Multi-line brace blocks accumulated via `AccumulateBraceBlock()`
- Single-line loop syntax: `foreach x in a b c { generate y = x }`
- Nested loops supported

**Other:**
- `tempvar lclname [...]` — generate unique column names (`__dodo_tmp_N`)
- `tempname lclname [...]` — generate unique scalar/macro names
- `display "text"` / `display expr` — informational output
- Macro functions: `:variable label varname`, `:value label varname`, `:label labelname #`, `:word count string`, `:word # of string`
- `` `=expr' `` inline expression evaluation

**Architecture:**
- `ExpandMacros()` runs before `IsDodoCommand()` in all processing paths (file, CLI, REPL)
- `ProcessDoFile()` and `process_stream()` refactored into shared `ProcessLines()` with `LineReader` abstraction
- Macros and scalars persist across `clear` (Stata behavior: `clear` only clears data)
- REPL integration: macro commands always trigger parser override; multi-line loop support via newline splitting

**Not implemented:** `_N` at macro expansion time (requires DuckDB query), stored results (`r()`/`e()`), `macval()`, `macro shift`, extended macro functions (`:subinstr`, `:strlen`, `:permname`, etc.), `foreach of varlist`/`foreach of newlist`

**Test:** 250 new assertions covering all features (1233 total)

### M15: `save` to tables, `tempfile`, `preserve`/`restore`

**Enhanced `save` — explicit `table` option for in-memory tables:**

`save` defaults to writing to disk (Stata behavior). The new `table` (or `memory`) option creates an in-memory DuckDB table instead:

| Syntax | Behavior | SQL |
|---|---|---|
| `save "data.csv", replace` | Write CSV to disk | `COPY (...) TO 'data.csv' (FORMAT CSV, HEADER)` |
| `save "data.parquet", replace` | Write Parquet to disk | `COPY (...) TO 'data.parquet' (FORMAT PARQUET)` |
| `save "data"` | Write to disk (no extension = no format hint) | `COPY (...) TO 'data'` |
| `save mytable, replace table` | Create in-memory table | `CREATE OR REPLACE TABLE mytable AS (...)` |
| `save mytable, replace memory` | Same (`memory` is alias for `table`) | `CREATE OR REPLACE TABLE mytable AS (...)` |

`save` remains a side-effect: the CTE chain continues unchanged after saving. You can keep transforming after a save.

Correspondingly, `use mytable, clear` loads from an in-memory table when no file extension is present (already works — `FileReadFunction` passes through bare names).

**`tempfile`:**
- `tempfile name` → registers `name` as a tempfile identifier in state
- On save/use, tempfile names resolve to `_tempfiles.name` (a private schema)
- `save "\`name'", replace` → `CREATE OR REPLACE TABLE _tempfiles.name AS (...)`
- `use "\`name'", clear` → resets chain with `SELECT * FROM _tempfiles.name`
- Schema `_tempfiles` created on first use via `CREATE SCHEMA IF NOT EXISTS _tempfiles`
- State: `set<string> tempfile_names` to track registered tempfile names

**`preserve`/`restore`:**
- `preserve` → save current `cte_steps.size()` and `step_counter` as a checkpoint
- `restore` → truncate `cte_steps` back to checkpoint, reset `step_counter`
- Pure state manipulation — no SQL, no tables, no disk
- State: `int preserve_checkpoint = -1` (index into cte_steps)
- Nested preserve not supported (same as Stata)

**Commands classification:**
- `save "file"` → side-effect (writes file/table, returns result of COPY/CREATE)
- `save tablename` → side-effect (creates table, chain continues)
- `tempfile` → transformation (registers name)
- `preserve` → transformation (saves checkpoint)
- `restore` → transformation (rewinds chain)

**Garbage collection:**
- State tracks all created tables: `vector<string> created_tables` (fully qualified names)
- `clear` drops all tracked tables, the `_tempfiles` schema, and resets the CTE chain
- `do "script.do"` completion drops tables created within that do-file scope (tempfiles die, explicit tables die unless saved to disk)
- Disk files are never touched by garbage collection

**Test:**
- `save mytable, replace table; use mytable, clear` — roundtrip via table
- `tempfile firms; save firms, replace; use firms, clear` — tempfile workflow
- `preserve; drop name; list; restore; list` — preserve/restore checkpoint
- `clear` after `save mytable, table` — table is dropped

### M16: `xtset`/`tsset` + lag/lead operators (`L.`, `F.`, `D.`)

Based on research of real-world usage in [korenmiklos/ceo-value](https://github.com/korenmiklos/ceo-value/tree/main/lib/create) and Stata documentation.

**Commands:**
- `xtset panelvar timevar` → store panel structure in state
- `tsset timevar` → store time-series structure (no panel var)
- `tsset panelvar timevar` → same as `xtset` (Stata treats them identically)
- State: `string panel_var`, `string time_var` in `DodoStateInfo`

**Time-series operators in expressions:**

| Operator | Meaning | SQL (gap-aware) |
|---|---|---|
| `L.x` | Lag (x at t-1) | See below |
| `L2.x` | 2nd lag (x at t-2) | See below |
| `F.x` | Lead (x at t+1) | See below |
| `F2.x` | 2nd lead (x at t+2) | See below |
| `D.x` | First difference (x - L.x) | Expand to `(x - L.x)` then translate |

**Gap-aware lag/lead (critical):**

Stata respects gaps in the time variable. If `year` goes 2015, 2017 (no 2016), then `L.x` at year 2017 is missing — NOT the 2015 value. Naive `LAG() OVER()` ignores gaps.

Gap-aware SQL translation for `L.x`:
```sql
CASE WHEN (year - LAG(year, 1) OVER (PARTITION BY id ORDER BY year)) = 1
     THEN LAG(x, 1) OVER (PARTITION BY id ORDER BY year)
     ELSE NULL END
```

For `L2.x` (2nd lag):
```sql
CASE WHEN (year - LAG(year, 2) OVER (PARTITION BY id ORDER BY year)) = 2
     THEN LAG(x, 2) OVER (PARTITION BY id ORDER BY year)
     ELSE NULL END
```

For `F.x` (lead):
```sql
CASE WHEN (LEAD(year, 1) OVER (PARTITION BY id ORDER BY year) - year) = 1
     THEN LEAD(x, 1) OVER (PARTITION BY id ORDER BY year)
     ELSE NULL END
```

**Implementation:**

The expression translator (`TranslateExpression`) detects `L.`, `L2.`, `F.`, `F2.`, `D.` prefixes via regex and expands them to gap-aware window function expressions. Requires `panel_var` and `time_var` to be set (throws error if not).

Regex pattern: `\bL(\d*)\.(\w+)` captures the lag count and variable name. Similarly for `F` and `D`.

`D.x` is syntactic sugar: expanded to `(x - L.x)` before translation.

**Real-world usage patterns (from ceo-value):**
- `generate capital = cond(missing(L.assets), assets - EBITDA, L.assets)` — fallback when lag missing
- `generate someone_exited = L.someone_exits == 1` — lagged indicator
- `drop if missing(L.ceo_spell) & missing(F.ceo_spell)` — filter on lag+lead
- `bysort fake_id (year): replace TFP = rho * TFP[_n-1] + dTFP` — explicit subscript lag (M17)

**`var[_n-1]` subscript syntax:**
- `x[_n-1]` is equivalent to `L.x` but does NOT respect gaps (raw positional lag)
- SQL: `LAG(x, 1) OVER (PARTITION BY panel_var ORDER BY time_var)` (no gap check)
- Used in `bysort` context where the sort order guarantees no gaps

**Test:**
- `xtset id year; generate lag_rev = L.revenue` — basic lag
- `generate lead_rev = F.revenue` — basic lead
- `generate diff_rev = D.revenue` — first difference
- `generate lag2_rev = L2.revenue` — 2nd lag
- Gap test: data with year gaps, verify L. produces NULL at gaps
- `drop if missing(L.x) & missing(F.x)` — filter on lag/lead

### M17: `bysort` prefix and `var[_n-1]` subscript

**`bysort` prefix:**

`bysort` is not a standalone command — it's a prefix that modifies the next command. The syntax `bysort group_vars (sort_vars): command` sets a partition-by and order-by context.

| Stata | SQL window |
|---|---|
| `bysort id (year): gen y = _n` | `ROW_NUMBER() OVER (PARTITION BY id ORDER BY year)` |
| `bysort id (year): gen y = _N` | `COUNT(*) OVER (PARTITION BY id)` |
| `bysort id (year): gen y = sum(x)` | `SUM(x) OVER (PARTITION BY id ORDER BY year ROWS UNBOUNDED PRECEDING)` |
| `bysort id (year): gen y = x[_n-1]` | `LAG(x, 1) OVER (PARTITION BY id ORDER BY year)` |

Implementation:
- Detect `bysort` at start of command line
- Parse `group_vars (sort_vars):` — group_vars are PARTITION BY, sort_vars (in parens) are ORDER BY
- Strip the prefix, pass partition/order context to the command handler
- `_n`, `_N`, `sum()`, and `[_n-1]` all use this context for their window functions

**`var[_n-1]` subscript syntax:**
- `x[_n-1]` → `LAG(x, 1) OVER (...)` — positional lag, does NOT respect time gaps
- `x[_n+1]` → `LEAD(x, 1) OVER (...)` — positional lead
- `x[1]` → first value of x (within group if bysort)
- Used in `bysort` context where sort order is explicit

Regex: `(\w+)\[_n\s*([+-]\s*\d+)\]` captures variable and offset.

**Real-world usage (from ceo-value):**
- `bysort frame_id_numeric (year): generate byte ceo_spell = sum(someone_enters | someone_exited)` — running sum
- `bysort fake_id (year): replace TFP = rho * TFP[_n-1] + dTFP if _n > 1` — AR(1) process
- `bysort frame_id_numeric person_id spell: generate year = start_year + _n - 1` — sequential year within spell
- `bysort frame_id_numeric (year): generate byte spell_id = sum(new_spell)` — cumulative indicator

**Test:**
- `bysort year: generate row = _n` — row number within year
- `bysort year: generate n = _N` — count within year
- `bysort id (year): generate cum_rev = sum(revenue)` — running sum
- `bysort id (year): generate prev_rev = revenue[_n-1]` — positional lag

### M18: `undo` / `redo` and structured command history

**Problem:** Interactive data exploration involves trial and error. A wrong `drop` or `keep` currently requires reloading the data and replaying all commands. `preserve`/`restore` only provides a single checkpoint.

**Commands:**

| Command | Behavior |
|---|---|
| `undo` | Remove the last transformation step, push it to redo stack |
| `undo N` | Remove the last N steps |
| `redo` | Re-apply the last undone step |
| `redo N` | Re-apply the last N undone steps |
| `history` | Terminal command: show the command history table |

**Implementation — structured history table:**

Replace the in-memory `vector<string> cte_steps` with a persistent table in the `dodo` schema:

```sql
CREATE TABLE IF NOT EXISTS dodo._history (
    step_id   INTEGER PRIMARY KEY,
    command   VARCHAR,     -- original dodo command text (e.g., 'keep if year >= 2020')
    cte_sql   VARCHAR,     -- generated CTE inner SQL (e.g., 'SELECT * FROM _s0 WHERE year >= 2020')
    undone    BOOLEAN DEFAULT false,  -- true if this step has been undone
    ts        TIMESTAMP DEFAULT current_timestamp
)
```

The `dodo._history` table lives in the same schema as `dodo._current`. It is created on the first `use` command (alongside the schema).

**State changes to `DodoStateInfo`:**

```cpp
struct DodoStateInfo : public ParserExtensionInfo {
    // ... existing fields ...

    // Replace cte_steps vector with history-backed chain
    // The active chain is: all rows in dodo._history WHERE NOT undone, ordered by step_id
    // For BuildCTEPrefix(), read from the table (or keep an in-memory cache synced)

    // In-memory cache (mirrors dodo._history WHERE NOT undone)
    vector<string> cte_steps;      // CTE SQL strings (active only)
    vector<string> cte_commands;   // original command text (for display)
    int step_counter = 0;          // next step_id to assign

    // Redo stack: steps that were undone (popped from cte_steps)
    vector<pair<string, string>> redo_stack;  // (command, cte_sql) pairs
};
```

**How `undo` works:**

1. Pop the last entry from `cte_steps` and `cte_commands`
2. Push it onto `redo_stack`
3. Decrement active step count
4. Update `dodo._history`: `UPDATE dodo._history SET undone = true WHERE step_id = <last>`
5. Refresh `_dodo_data` view if live_view enabled
6. Return `SELECT 'OK' AS status`

**How `redo` works:**

1. Pop from `redo_stack`
2. Push back onto `cte_steps` and `cte_commands`
3. Update `dodo._history`: `UPDATE dodo._history SET undone = false WHERE step_id = <step>`
4. Refresh live view
5. Return `SELECT 'OK' AS status`

**Redo invalidation:** Any new transformation command clears the redo stack (standard undo/redo semantics). Delete undone rows: `DELETE FROM dodo._history WHERE undone = true`.

**`history` terminal command:**

```sql
SELECT step_id, command, undone, ts FROM dodo._history ORDER BY step_id
```

Shows the full history including undone steps, so users can see what happened.

**Why a table instead of just in-memory vectors:**

1. **Visible in the UI** — the DuckDB UI data panel shows `dodo._history` alongside `dodo._current`, making the command history browsable
2. **Queryable** — `SELECT * FROM dodo._history WHERE command LIKE '%revenue%'` to find when a column was transformed
3. **Survives within session** — even if the extension state is somehow reset, the history persists
4. **Foundation for future features** — branching, named checkpoints, session replay

**Interaction with existing features:**

- `clear` → drops `dodo._history` along with `dodo._current`
- `preserve`/`restore` → works as before (index into active steps), orthogonal to undo
- `use "file", clear` → drops and recreates `dodo._history`
- `.do` files → no undo support (forced lazy, no history table)
- `BuildCTEPrefix()` → reads from in-memory `cte_steps` cache (unchanged performance)
- Live view → refreshed after undo/redo same as any transformation

**Edge cases:**

- `undo` with no steps → error: "Nothing to undo"
- `redo` with empty redo stack → error: "Nothing to redo"
- `undo` after `use` (only _s0 remains) → error: "Cannot undo past data load"
- `undo N` where N > available steps → undo all available, warn

**Test plan:**

- `use; keep if x > 0; undo; list` — data shows all rows again
- `use; keep; generate; undo; undo; redo; list` — back to keep-only state
- `use; keep; undo; drop if y < 0; redo` → error (redo stack cleared)
- `history` — shows step_id, command, undone status
- `clear` — history table dropped
- `SELECT * FROM dodo._history` — queryable from SQL

### M19: Polish phase 2
- `compress` → no-op (DuckDB doesn't need storage type optimization)
- `display` → `SELECT 'message'` (informational output)
- `assert` → `SELECT CASE WHEN NOT (expr) THEN error('Assertion failed') END`
- `set seed N` → `SELECT setseed(N)`
- Error messages: improve error reporting for unsupported commands
- **Test:** comprehensive test with real .do file fragments from ceo-value

---

## Key Design Decisions

1. **CTE chain, not views**: Transformation commands accumulate as CTE steps in per-connection state. Nothing touches the catalog. DuckDB optimizes the full chain when materialized.

2. **Lazy by default**: Only terminal commands (`list`, `summarize`, `count`, `tabulate`, `save`, `describe`, `head`, `tail`) trigger execution. Transformation commands are instant (just string append).

3. **`use` resets the chain**: `use "file.csv", clear` clears all accumulated CTEs and starts fresh with step 0.

4. **Expression translation, not evaluation**: We translate Stata expressions to SQL expressions at parse time. No custom executor needed.

5. **`==` stays as `==`**: DuckDB supports `==` as an alias for `=` in WHERE clauses, so minimal rewriting needed for comparisons.

6. **Error on empty chain**: If no `use` has been called and user tries `keep`/`drop`/etc., throw a clear error: "No dataset in memory. Use 'use' to load data."

7. **Structured state in `dodo` schema**: The extension stores its state in `dodo._current` (materialized data) and `dodo._history` (command history). This makes state visible in the DuckDB UI and queryable from SQL. `clear` drops everything.

---

## Current Source Structure (v0.2.0)

The codebase is split into three modules:

| Directory | Purpose | DuckDB dependency? |
|---|---|---|
| `src/core/` | Compiler: tokenizer, expression translator, command processor, SQL formatter, state | No |
| `src/extension/` | DuckDB glue: parser hooks, planner redirect, option registration | Yes |
| `src/dta/` | Native .dta reader/writer: `DtaReader` (pure C++), `DtaWriter`, DuckDB table function + CopyFunction | Reader: No, DuckDB wrappers: Yes |
| `src/cli/` | `dodoc` standalone compiler (reads stdin/.do files, writes SQL to stdout) | No |

Key files:
- `src/core/dodo_core.hpp` — `DodoState`, `DodoCommand`, free function declarations
- `src/core/dodo_core.cpp` — all command processing, expression translation, SQL formatting
- `src/core/string_utils.hpp` — standalone string utilities (no DuckDB dependency)
- `src/extension/dodo_extension.hpp` — `DodoStateInfo` (wraps `DodoState` with DuckDB interfaces)
- `src/extension/dodo_extension.cpp` — parser/planner/operator registration, live view, history table
- `src/dta/dta_reader.hpp/cpp` — pure C++ .dta file reader (formats 117-121)
- `src/dta/dta_writer.hpp/cpp` — pure C++ .dta file writer (format 118)
- `src/dta/read_dta_function.hpp/cpp` — DuckDB `read_dta()` table function
- `src/dta/write_dta_function.hpp/cpp` — DuckDB `COPY TO (FORMAT dta)` CopyFunction
- `src/cli/dodoc.cpp` — CLI compiler entry point
- `test/sql/dodo.test` — sqllogictest suite (1233 assertions)
- `test/sql/read_dta.test` — read_dta tests
- `test/sql/write_dta.test` — write_dta + round-trip tests

## Milestone Status

| Milestone | Description | Status |
|---|---|---|
| M0 | Scaffolding | Done |
| M1 | `use` and `list` | Done |
| M2 | `keep`, `drop`, `rename` | Done |
| M3 | `generate`, `replace`, expressions | Done |
| M4 | `sort`, `order`, `count`, `describe`, `head`, `tail` | Done |
| M5 | `egen`, `collapse` with `by()` | Done |
| M5.5 | `do "script.do"` | Done |
| M6 | `summarize`, `tabulate` | Done |
| M7 | `save`, `append`, `mvencode` | Done |
| M8 | `reshape` (long + wide) | Done |
| M9 | `regress` | Not started (stretch goal) |
| M9.5 | Labels and `describe` | Done |
| M10 | Polish, community extension, CI | Done |
| M11 | Expression functions (`cond`, `inrange`, `inlist`, etc.) | Done |
| M12 | `merge` | Done |
| M13 | `duplicates drop`, `expand`, `export`/`import delimited` | Done |
| M14 | Local macros and loops | Done |
| M15 | `save` to tables, `tempfile`, `preserve`/`restore` | Done |
| M16 | `xtset`/`tsset` + `L.`/`F.`/`D.` | Done |
| M17 | `bysort` prefix, `var[_n-1]` | Done |
| M18 | `undo`/`redo`, `history` | Done |
| M19 | Polish phase 2 | Partial (`display` done, `compress` pending) |

### Added in v0.2.0 (not in original plan)

- **`dodoc` standalone compiler** (`src/cli/`) — translates .do to SQL without DuckDB
- **SQL formatting** — formatted output with indentation and `-- [source]` comments
- **`show sql` command** — displays the generated SQL for the current CTE chain
- **Settings**: `dodo_format_sql`, `dodo_sql_comments`, `dodo_live_view`
- **Core/extension/cli module split** — compiler is DuckDB-independent

## Next: Pipeline Execution Model

See `docs/DBT_RESEARCH.md` for research on bipartite DAG execution, build system taxonomy, and materialization strategies. This is the foundation for dodo Studio's pipeline features (Benefit 4 in `docs/BENEFITS.md`).

---

## Verification

1. `make` — builds successfully
2. `./build/release/test/unittest --test-dir . "test/sql/dodo.test"` — 1233 assertions pass
3. `echo 'use "data.csv", clear' | ./build/release/extension/dodo/dodoc` — compiler works
4. Manual test:
   ```sql
   LOAD dodo;
   use "test/data/firms.csv", clear;
   keep if year >= 2018;
   generate ln_revenue = log(revenue);
   show sql;
   list;
   ```
