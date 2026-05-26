# stata_do

A DuckDB extension that lets you write Stata `.do` file commands directly in DuckDB. Commands are translated to SQL under the hood using a lazy CTE chain — nothing executes until you ask for results.

Inspired by [Kezdi.jl](https://github.com/codedthinking/Kezdi.jl).

## Quick Start

```sql
-- Build from source (see below), then run:
-- ./build/release/duckdb

use "data/firms.csv", clear;
keep if year >= 2020;
generate profit = revenue - cost;
egen mean_profit = mean(profit), by(sector);
collapse (mean) avg_profit = profit (count) n = id, by(sector);
list;
```

Or run a `.do` file (no semicolons needed inside):

```sql
do "analysis/clean.do";
list;
```

## Supported Commands

### Data I/O
| Command | Example |
|---|---|
| `use` | `use "data.csv", clear` |
| `save` | `save "output.parquet", replace` |
| `append` | `append using "more_data.csv"` |
| `do` | `do "script.do"` |
| `clear` | `clear` |

### Data Manipulation
| Command | Example |
|---|---|
| `keep` | `keep var1 var2` or `keep if x > 0` |
| `drop` | `drop var1` or `drop if missing(x)` |
| `generate` | `generate ln_wage = log(wage)` |
| `replace` | `replace wage = 0 if missing(wage)` |
| `rename` | `rename old_name new_name` |
| `sort` | `sort year revenue, desc` |
| `order` | `order year name` |
| `egen` | `egen mean_rev = mean(revenue), by(sector)` |
| `collapse` | `collapse (mean) avg = x (sum) total = x, by(group)` |
| `mvencode` | `mvencode var1 var2, mv(0)` |
| `reshape` | `reshape long revenue, i(id) j(year)` |

### Inspection (terminal commands)
| Command | Example |
|---|---|
| `list` | `list` or `list var1 var2 if x > 0` |
| `count` | `count` or `count if year == 2020` |
| `head` | `head 10` |
| `tail` | `tail 5` |
| `describe` | `describe` |
| `summarize` | `summarize revenue` |
| `tabulate` | `tabulate sector year` |

### Special Variables
| Variable | Meaning |
|---|---|
| `_n` | Row number (within group if `by()` used) |
| `_N` | Total rows (within group if `by()` used) |

### Expression Translation
| Stata | SQL |
|---|---|
| `log(x)` | `LN(x)` |
| `missing(x)` | `x IS NULL` |
| `==` | `==` (DuckDB supports this) |

### `.do` File Features
- No semicolons needed
- `*` line-start comments
- `//` inline comments
- `/* ... */` block comments
- `///` line continuation

## How It Works

Each transformation command appends a CTE step to a chain stored in memory. Nothing executes until a terminal command (`list`, `count`, `summarize`, etc.) materializes the full chain:

```
use "firms.csv", clear     -->  _s0 AS (SELECT * FROM read_csv('firms.csv'))
keep if year == 2018        -->  _s1 AS (SELECT * FROM _s0 WHERE year == 2018)
generate ln_rev = log(rev)  -->  _s2 AS (SELECT *, LN(rev) AS ln_rev FROM _s1)
list                        -->  WITH _s0 AS (...), _s1 AS (...), _s2 AS (...)
                                 SELECT * FROM _s2   -- executes here
```

DuckDB optimizes the full CTE chain at once. File formats are auto-detected: `.csv`, `.parquet`, `.json`, `.dta`.

## Limitations

### `.dta` file support

Reading `.dta` files via `use "file.dta"` relies on the DuckDB community extension (`st_read()`), which is slow for large files. Writing `.dta` files is not supported.

**Recommended workflow:** convert `.dta` files to Parquet beforehand using [stata2ducklake](https://github.com/codedthinking/stata2ducklake) or similar tools, then use Parquet in your scripts:

```
# Convert once (outside DuckDB)
stata2ducklake input.dta --output data/

# Use Parquet in your .do files
use "data/input.parquet", clear
```

### SQL keyword conflicts

`describe` and `summarize` conflict with DuckDB SQL keywords. Use `codebook` as an alias for `describe` that always works. When all statements are on one line (e.g., `use "file.csv"; describe;`), the extension intercepts them correctly.

## Building

```bash
git clone --recurse-submodules https://github.com/korenmiklos/duckdb-stata-do.git
cd duckdb-stata-do
make release
./build/release/duckdb
```

## Testing

```bash
make test
```

## License

MIT License. See [LICENSE](LICENSE).
