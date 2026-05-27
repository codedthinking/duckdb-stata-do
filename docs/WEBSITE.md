# dodo Documentation Website — Design Guide

## What this document is for

This guide gives a design agent everything it needs to design a documentation website for **dodo**, a DuckDB extension that runs `.do` files. It covers the site structure, all commands with code examples, and the tone/brand constraints.

Read `BRANDING.md` for visual identity, color palette, typography, and logo direction. Brand assets (SVGs) are in the repo's `/assets` directory — hero banners, CTE chain diagram, commands cheatsheet, terminal demo, social card, logo variants, pipeline diagram, and favicon.

---

## Site structure

```
/                       → Landing page (hero + quick start + install)
/docs/                  → Getting started guide
/docs/commands/         → Command reference (one page per category)
  /docs/commands/io/    → use, save, import, export, append, do, clear
  /docs/commands/transform/ → keep, drop, generate, replace, rename, sort, order, egen, collapse, mvencode, reshape, merge, duplicates, expand
  /docs/commands/inspect/   → list, count, head, tail, describe, summarize, tabulate, history
  /docs/commands/state/     → undo, redo, preserve, restore, tempfile, xtset, tsset, label
  /docs/commands/control/   → bysort, by
/docs/expressions/      → Expression translation (Stata → SQL)
/docs/do-files/         → Running .do files (comments, continuation, structure)
/docs/ui/               → DuckDB UI integration (live view, history table)
/docs/how-it-works/     → CTE chain architecture, materialization, lazy mode
/docs/migration/        → Guide for Stata users moving to dodo
/docs/limitations/      → Known gaps, .dta support, keyword conflicts
```

Each command reference page should follow a consistent template:

```
# command_name

One-line description.

## Syntax
  command_name arguments [if condition] [, options]

## Examples
  (2–3 runnable code blocks)

## SQL translation
  (what SQL dodo generates under the hood)

## Notes
  (edge cases, gotchas)
```

---

## Landing page

### Hero section

```
dodo

Old scripts. New pond.

Run .do files in DuckDB.
```

Below the hero, a single runnable code block:

```sql
INSTALL dodo;
LOAD dodo;

use "firms.csv", clear;
keep if year >= 2020;
generate profit = revenue - cost;
collapse (mean) avg_profit = profit, by(sector);
list;
```

### Three feature cards

1. **Familiar syntax** — Write `keep`, `generate`, `collapse` instead of SQL. Your muscle memory works.
2. **DuckDB speed** — Parquet, CSV, JSON at analytical engine speed. No row-by-row execution.
3. **Undo everything** — Made a mistake? `undo`. Changed your mind? `redo`. Full `history` of every step.

### Install section

```sql
INSTALL dodo;
LOAD dodo;
```

Or build from source:

```bash
git clone --recurse-submodules https://github.com/codedthinking/dodo.git
cd dodo && make release
./build/release/duckdb
```

---

## Command reference — complete list with examples

### Data I/O

#### `use`
Load data from a file or table. Materializes into `dodo._current` by default.

```sql
use "data/firms.csv", clear;
use "data/firms.parquet", clear;
use existing_table, clear;
use "data/firms.csv", lazy;       -- no materialization, CTE-only
```

#### `save`
Write the current data to disk or to an in-memory table.

```sql
save "output.csv", replace;
save "output.parquet", replace;
save my_table, replace table;     -- in-memory DuckDB table
```

#### `import delimited`
Read a CSV file (alias for `use` with CSV).

```sql
import delimited "data/survey.csv", clear;
```

#### `export delimited`
Write the current data to a CSV file.

```sql
export delimited using "output.csv", replace;
```

#### `append`
Stack another dataset below the current one.

```sql
append using "more_firms.csv";
```

#### `do`
Run a `.do` file. No semicolons needed inside the file.

```sql
do "analysis/clean.do";
list;    -- inspect results after the script runs
```

#### `clear`
Drop the current dataset and all associated state.

```sql
clear;
```

---

### Data manipulation

#### `keep`
Keep specific columns, or keep rows matching a condition.

```sql
keep id revenue year;
keep if year >= 2020;
keep id revenue if year >= 2020;
```

#### `drop`
Drop columns by name, or drop rows matching a condition.

```sql
drop temp_var debug_flag;
drop if missing(revenue);
```

#### `generate`
Create a new column.

```sql
generate profit = revenue - cost;
generate ln_rev = log(revenue);
generate high_rev = revenue > 1000 if year >= 2020;
```

#### `replace`
Overwrite an existing column's values.

```sql
replace revenue = 0 if missing(revenue);
replace name = "Unknown" if missing(name);
```

#### `rename`
Rename a column.

```sql
rename old_name new_name;
```

#### `sort`
Sort rows.

```sql
sort year;
sort revenue, desc;
```

#### `order`
Reorder columns (listed columns move to front).

```sql
order year id name;
```

#### `egen`
Create a column using a window/aggregate function, optionally grouped.

```sql
egen mean_rev = mean(revenue), by(sector);
egen row_num = seq(), by(sector);
egen total = sum(revenue);
```

#### `collapse`
Aggregate data, reducing rows to one per group.

```sql
collapse (mean) avg_rev = revenue (sum) total = revenue (count) n = id, by(sector year);
collapse (mean) revenue profit, by(sector);
```

#### `mvencode`
Replace missing values.

```sql
mvencode revenue profit, mv(0);
```

#### `reshape`
Pivot between long and wide formats.

```sql
-- Wide to long
reshape long revenue, i(id) j(year);

-- Long to wide
reshape wide revenue, i(id) j(year);
```

#### `merge`
Join two datasets.

```sql
merge 1:1 id year using "other_data.csv";
merge m:1 sector using "sector_names.csv", keep(match);
merge 1:1 id using "extra.csv", keepusing(new_var) nogenerate;
```

#### `duplicates drop`
Remove duplicate rows.

```sql
duplicates drop;              -- all columns
duplicates drop id year;      -- by specific columns
```

#### `expand`
Replicate rows.

```sql
expand 3;                          -- triple every row
expand count, generate(copy);      -- variable expansion with indicator
```

---

### Inspection (terminal commands)

These commands execute the full CTE chain and return results.

#### `list`
Show the data.

```sql
list;
list id revenue if year >= 2020;
```

#### `count`
Count rows.

```sql
count;
count if revenue > 1000;
```

#### `head` / `tail`
Show first or last N rows.

```sql
head 10;
tail 5;
```

#### `describe`
Show column names and types. Alias: `codebook`.

```sql
describe;
codebook;    -- same thing, avoids SQL keyword conflict
```

#### `summarize`
Compute summary statistics for a variable.

```sql
summarize revenue;
summarize revenue if year >= 2020;
```

Output: N, mean, sd, min, p25, p50, p75, max.

#### `tabulate`
Frequency table or cross-tabulation.

```sql
tabulate sector;
tabulate sector year;
```

#### `history`
Show the command history for the current session.

```sql
history;
```

Output: step_id, command text, undone flag. Also queryable as `SELECT * FROM dodo._history`.

---

### State management

#### `undo` / `redo`
Roll back or re-apply the last transformation step.

```sql
use "firms.csv", clear;
keep if year >= 2020;
generate profit = revenue - cost;
undo;           -- profit column is gone
redo;           -- profit column is back
undo 2;         -- undo two steps at once
```

A new transformation after `undo` clears the redo stack.

#### `preserve` / `restore`
Save and restore a checkpoint.

```sql
preserve;
drop if revenue < 100;
list;           -- filtered data
restore;
list;           -- original data is back
```

#### `tempfile`
Register a name for temporary in-memory storage.

```sql
tempfile cleaned;
save cleaned, replace;
-- ... do other work ...
use cleaned, clear;
```

#### `xtset` / `tsset`
Declare panel or time-series structure for lag/lead operators.

```sql
xtset firm_id year;
generate lag_rev = L.revenue;
generate lead_rev = F.revenue;
generate diff_rev = D.revenue;
```

#### `label`
Attach metadata to columns and values.

```sql
label variable revenue "Annual revenue in USD";
label define yesno 0 "No" 1 "Yes";
label values employed yesno;
```

---

### Prefix commands

#### `bysort` / `by`
Run a command within groups, with optional sort order.

```sql
bysort sector (year): generate row = _n;
bysort firm_id (year): generate cum_rev = sum(revenue);
bysort firm_id (year): generate prev_rev = revenue[_n-1];
```

---

## Expression translation

| `.do` syntax | SQL equivalent |
|---|---|
| `log(x)` | `LN(x)` |
| `abs(x)` | `ABS(x)` |
| `round(x, 2)` | `ROUND(x, 2)` |
| `missing(x)` | `x IS NULL` |
| `cond(a, b, c)` | `CASE WHEN a THEN b ELSE c END` |
| `inrange(x, 1, 10)` | `x BETWEEN 1 AND 10` |
| `inlist(x, 1, 2, 3)` | `x IN (1, 2, 3)` |
| `substr(s, 1, 3)` | `SUBSTRING(s, 1, 3)` |
| `strlen(s)` | `LENGTH(s)` |
| `strlower(s)` | `LOWER(s)` |
| `strupper(s)` | `UPPER(s)` |
| `strtrim(s)` | `TRIM(s)` |
| `real(s)` | `CAST(s AS DOUBLE)` |
| `_n` | `ROW_NUMBER() OVER (...)` |
| `_N` | `COUNT(*) OVER (...)` |
| `L.x` | gap-aware `LAG(x) OVER (...)` |
| `F.x` | gap-aware `LEAD(x) OVER (...)` |
| `D.x` | `x - L.x` |
| `x[_n-1]` | `LAG(x, 1) OVER (...)` (positional, no gap check) |

---

## How it works — for the architecture page

### Two modes

1. **Materialized (default):** `use "file.csv", clear` reads the file once into `dodo._current` table. Subsequent commands build a lazy CTE chain on top of that table.

2. **Lazy:** `use "file.csv", lazy` skips materialization. The file is re-read every time a terminal command executes.

### The CTE chain

Each transformation appends a step. Nothing executes until a terminal command.

```
use "firms.csv", clear     →  CREATE TABLE dodo._current AS SELECT * FROM read_csv(...)
                               _s0 AS (SELECT * FROM dodo._current)
keep if year >= 2020       →  _s1 AS (SELECT * FROM _s0 WHERE year >= 2020)
generate profit = rev-cost →  _s2 AS (SELECT *, rev-cost AS profit FROM _s1)
list                       →  WITH _s0 AS (...), _s1 AS (...), _s2 AS (...)
                               SELECT * FROM _s2   ← executes here
```

### DuckDB UI integration

Set `SET dodo_live_view = true` to create a `_dodo_data` view after each command. The DuckDB UI data panel auto-refreshes when this view changes.

The `dodo._history` table is also visible in the UI, showing every command and its undo status.

---

## `.do` file features — for the do-files page

`.do` files use newlines instead of semicolons. All standard comment styles work:

```stata
* This is a line comment
// This is also a comment
/* This is a
   block comment */

use "data.csv", clear

keep if year >= 2020   // inline comment
generate profit = ///
    revenue - cost     // line continuation with ///
```

Run with:

```sql
do "analysis/clean.do";
```

Terminal commands inside `.do` files are skipped — inspect results interactively after the script finishes.

---

## Tone notes for the design agent

- The site should feel like DuckDB docs meets a slightly irreverent OSS project.
- Code examples are the primary content. Prose should be minimal.
- Every command page should be scannable in under 10 seconds.
- Use the warm ochre/fossil palette from `BRANDING.md`, not DuckDB yellow.
- The landing page should make someone say "oh cool, I can just write `keep if year > 2020` instead of SQL?"
- Dark mode is required. The fossil ivory / dark background pairing from the brand guide should work.
- Navigation should be a left sidebar with collapsible command categories.
- Mobile: the sidebar collapses to a hamburger. Code blocks must not overflow.
