# DuckDB Stata `.do` Extension — Implementation Plan

## Context

We are building a DuckDB community extension called **`stata_do`** (working name) that lets users write a subset of Stata `.do` file commands directly inside DuckDB. The extension implements the same commands as [Kezdi.jl](https://github.com/codedthinking/Kezdi.jl), translating them to SQL under the hood.

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
class StataDoState : public ClientContextState {
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
| `use "file.dta", clear` | Resets chain. `_s0 AS (SELECT * FROM st_read('file.dta'))` |
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
- Rename `waddle` → `stata_do` everywhere (CMakeLists.txt, extension_config.cmake, Makefile, source files, headers)
- Remove OpenSSL dependency
- Set up `StataDoParserExtension` + `StataDoOperatorExtension` skeleton (following prql pattern)
- Set up `StataDoState` as `ClientContextState`
- Verify it builds and loads (empty extension, no commands yet)
- **Test:** `LOAD stata_do; SELECT 1;` works

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
   - Store in per-connection state: `StataDoStateInfo.variable_labels[varname] = label`
   - When `save` is called to a DuckLake catalog, emit `COMMENT ON COLUMN`
   - In the CTE chain: labels are metadata-only, no SQL step needed
   - Returns `SELECT 'OK'`

2. **`label define labelname value1 "text1" value2 "text2" ...`**
   - Store in state: `StataDoStateInfo.value_label_defs[labelname] = {val: text, ...}`
   - These define the mapping but don't attach it to a column yet

3. **`label values varname labelname`**
   - Store in state: `StataDoStateInfo.column_labels[varname] = labelname`
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

**State changes to `StataDoStateInfo`:**
```cpp
struct StataDoStateInfo : public ParserExtensionInfo {
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

## Key Design Decisions

1. **CTE chain, not views**: Transformation commands accumulate as CTE steps in per-connection state. Nothing touches the catalog. DuckDB optimizes the full chain when materialized.

2. **Lazy by default**: Only terminal commands (`list`, `summarize`, `count`, `tabulate`, `save`, `describe`, `head`, `tail`) trigger execution. Transformation commands are instant (just string append).

3. **`use` resets the chain**: `use "file.csv", clear` clears all accumulated CTEs and starts fresh with step 0.

4. **Expression translation, not evaluation**: We translate Stata expressions to SQL expressions at parse time. No custom executor needed.

5. **`==` stays as `==`**: DuckDB supports `==` as an alias for `=` in WHERE clauses, so minimal rewriting needed for comparisons.

6. **Error on empty chain**: If no `use` has been called and user tries `keep`/`drop`/etc., throw a clear error: "No dataset in memory. Use 'use' to load data."

7. **No catalog side effects**: The extension never creates tables, views, or other persistent objects. Everything lives in the CTE chain in memory.

---

## Files to Create/Modify

| File | Action |
|---|---|
| `CMakeLists.txt` | Rename to `stata_do`, remove OpenSSL |
| `extension_config.cmake` | Rename to `stata_do` |
| `Makefile` | Rename `EXT_NAME` |
| `src/include/stata_do_extension.hpp` | Extension class + StataDoState + parser structs |
| `src/stata_do_extension.cpp` | Extension registration (parser + operator) |
| `src/stata_do_parser.cpp` | Stata command tokenizer and SQL generator |
| `src/include/stata_do_parser.hpp` | Parser declarations |
| `test/sql/stata_do.test` | Main test file |
| `vcpkg.json` | Remove OpenSSL dependency |

---

## Verification

1. `make` — builds successfully
2. `make test` — all sqllogictest tests pass
3. Manual test sequence:
   ```sql
   LOAD stata_do;
   use "test/data/firms.csv", clear;
   describe;
   keep if year == 2018;
   generate ln_rev = log(revenue);
   summarize ln_rev;
   list;
   ```
