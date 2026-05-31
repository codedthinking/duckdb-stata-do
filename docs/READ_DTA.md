# Plan: `read_dta` Module

## Context

The existing `duckdb-read-stat` extension uses ReadStat and has O(N^2) performance — it re-parses the .dta file from the beginning on every 2048-row chunk. We'll build a from-scratch .dta reader as a new module in this extension, supporting formats 117-121 (Stata 13–18). Format 115 (Stata 10–12) uses a completely different binary layout (no XML tags, 1-byte type codes, no strL, no `<map>`) and is excluded from the initial implementation.

## File Structure

```
src/read_dta/
  dta_reader.hpp      -- DtaReader class (pure C++17, no DuckDB dependency)
  dta_reader.cpp      -- Implementation: header parsing, data scanning, strL, value labels
  read_dta_function.hpp  -- DuckDB table function declarations
  read_dta_function.cpp  -- bind/init/scan callbacks, row-to-column transposition
src/include/
  dta_reader.hpp      -- proxy header
```

## Architecture

### Layer 1: `DtaReader` (pure C++17, no DuckDB)

A stateful reader class that owns a file handle and provides:

```cpp
struct DtaColumn {
    std::string name;
    uint16_t type_code;     // 1-2045=str, 32768=strL, 65526-65530=numeric
    uint16_t byte_width;    // bytes per observation for this variable
    std::string format;     // display format (e.g. "%td", "%tc", "%9.0g")
    std::string value_label_name;
    std::string label;
};

struct DtaVersionParams {
    int version;            // 117-121
    uint32_t varname_len;   // 33 or 129
    uint32_t sortlist_entry_size; // 2 or 4
    uint32_t fmt_len;       // 49 or 57
    uint32_t label_name_len;// 33 or 129
    uint32_t var_label_len; // 81 or 321
    uint32_t k_field_size;  // 2 or 4
    uint32_t n_field_size;  // 4 or 8
    uint32_t dataset_label_len_size; // 1 or 2
    bool has_alias_vars;    // true for 120, 121
};

// Version → parameter mapping:
//
// | Field                  | 117       | 118    | 119    | 120    | 121    |
// |------------------------|-----------|--------|--------|--------|--------|
// | varname_len            | 33        | 129    | 129    | 129    | 129    |
// | sortlist_entry_size    | 2         | 2      | 4      | 2      | 4      |
// | fmt_len                | 49        | 57     | 57     | 57     | 57     |
// | label_name_len         | 33        | 129    | 129    | 129    | 129    |
// | var_label_len          | 81        | 321    | 321    | 321    | 321    |
// | k_field_size           | 2         | 2      | 4      | 2      | 4      |
// | n_field_size           | 4         | 8      | 8      | 8      | 8      |
// | dataset_label_len_size | 1         | 2      | 2      | 2      | 2      |
// | has_alias_vars         | false     | false  | false  | true   | true   |
// | string encoding        | ASCII     | UTF-8  | UTF-8  | UTF-8  | UTF-8  |
//
// Formats 119/121 exist for datasets with >32,767 variables (K and sortlist use 4 bytes).
// Formats 120/121 exist for datasets with alias variables (type code 65525).

struct DtaValueLabel {
    std::string name;
    std::unordered_map<int32_t, std::string> mappings; // val -> text
};

class DtaReader {
public:
    DtaReader(const std::string &path);
    ~DtaReader();

    // Metadata (available after construction)
    int Version() const;
    bool IsMSF() const;
    uint64_t NumObs() const;
    uint32_t NumVars() const;
    const std::vector<DtaColumn> &Columns() const;
    const std::string &DatasetLabel() const;
    const std::vector<DtaValueLabel> &ValueLabels() const;

    // Data reading
    uint32_t RowWidth() const;           // bytes per observation
    uint64_t DataOffset() const;         // file offset of <data> contents
    size_t ReadRows(uint64_t start_row, uint32_t count,
                    std::vector<char> &buffer);  // bulk read into buffer

    // strL support
    void LoadStrLs();  // parse <strls> section into lookup table
    std::string ResolveStrL(uint16_t v, uint64_t o) const;

    // Value labels
    void LoadValueLabels();

private:
    FILE *fp_;
    DtaVersionParams params_;
    bool msf_;  // byte order
    uint64_t n_obs_;
    uint32_t n_vars_;
    std::string dataset_label_;
    std::vector<DtaColumn> columns_;
    uint32_t row_width_;
    uint64_t data_offset_;     // from <map>
    uint64_t strls_offset_;    // from <map>
    uint64_t value_labels_offset_;
    std::unordered_map<uint64_t, std::string> strl_table_; // packed(v,o) -> string
    std::vector<DtaValueLabel> value_labels_;

    void ParseHeader();
    void ParseMap();
    void ParseVariableTypes();
    void ParseVarnames();
    void ParseFormats();
    void ParseValueLabelNames();
    void ParseVariableLabels();
    void SkipCharacteristics();

    // Byte-swap helpers
    template<typename T> T SwapIfNeeded(T val) const;
    void ReadTag(const char *expected);
    void ReadBytes(void *buf, size_t n);
};
```

### Layer 2: DuckDB Table Function (`read_dta_function.cpp`)

Registers `read_dta(path VARCHAR)` with optional named parameters:
- `value_labels` (BOOLEAN, default false) — apply value labels as ENUM types

**Bind**: opens `DtaReader`, reads metadata, maps columns to DuckDB types:

| Stata type | Format | DuckDB type |
|---|---|---|
| byte (65530) | — | TINYINT |
| int (65529) | — | SMALLINT |
| long (65528) | — | INTEGER |
| float (65527) | — | FLOAT |
| double (65526) | — | DOUBLE |
| double (65526) | %td, %d | DATE |
| double (65526) | %tc, %tC | TIMESTAMP |
| str1-str2045 | — | VARCHAR |
| strL (32768) | — | VARCHAR |
| any numeric | — with value_labels=true and label exists | ENUM |

Missing values (sentinels above valid max) → NULL for all numeric types. Stata has 27 missing value codes (`.`, `.a`–`.z`) per type; currently all are mapped to a single NULL. Future enhancement: preserve extended missing value codes (e.g. as a separate column or metadata).

Date conversion: Stata epoch is 1960-01-01. For %td: `duckdb_date = stata_days - 3653`. For %tc: stata milliseconds since 1960 → microseconds since 1970.

**Init**: stores current row offset (starts at 0), allocates row buffer.

**Scan**:
1. Compute bytes to read: `min(STANDARD_VECTOR_SIZE, remaining_rows) * row_width`
2. `fseek` to `data_offset + current_row * row_width`
3. `fread` the entire chunk into a buffer
4. Row-to-column transpose: for each column, extract bytes at known offsets within each row, write into DuckDB vectors
5. Handle byte swapping, missing value detection, string trimming, strL resolution
6. Advance row offset

**Projection pushdown**: bind stores which columns are projected; scan only processes those columns (still reads full rows but skips unprojected columns during transpose).

### Alias Variables (v120/v121)

For the initial implementation, alias variables (type code 65525) will be **skipped** — they are references to variables in other frames and have no data in the .dta file itself. We detect them during type parsing and exclude them from the column list. This is safe because alias variables are only meaningful within Stata's frame system.

### String Encoding

Format 117 uses ASCII encoding for all strings (variable names, data, labels). Formats 118+ use UTF-8. Since ASCII is a subset of UTF-8, the reader treats all strings as UTF-8 and no special handling is needed.

## Build System Changes

`CMakeLists.txt`:
```cmake
include_directories(... ${CMAKE_CURRENT_SOURCE_DIR}/src/read_dta)

set(EXTENSION_SOURCES
    src/extension/dodo_extension.cpp
    src/core/dodo_core.cpp
    src/read_dta/dta_reader.cpp
    src/read_dta/read_dta_function.cpp)
```

## Integration Point

`src/extension/dodo_extension.cpp` in `LoadInternal()`:
```cpp
// After existing registration code:
RegisterReadDtaFunction(loader);
```

`src/core/dodo_core.cpp` line 331: change `st_read` → `read_dta`.

## Implementation Order

1. `dta_reader.hpp` — structs, class declaration, version params
2. `dta_reader.cpp` — header parsing (format detection, map, types, names, formats)
3. `dta_reader.cpp` — data reading (bulk row read, byte swap, missing values)
4. `dta_reader.cpp` — strL support (parse GSOs, resolve references)
5. `dta_reader.cpp` — value labels
6. `read_dta_function.hpp/cpp` — DuckDB table function (bind/init/scan)
7. CMakeLists.txt + extension registration
8. Proxy header + core integration (`st_read` → `read_dta`)
9. Tests

## Verification

1. Build: `make` (or `GEN=ninja make`)
2. Test with known .dta files:
   ```sql
   SELECT * FROM read_dta('test/data/auto.dta');
   SELECT * FROM read_dta('test/data/auto.dta', value_labels=true);
   ```
3. Compare output with Stata or pandas `read_stata()` for correctness
4. Benchmark against `duckdb-read-stat` on a large .dta file
5. Run existing test suite to verify no regressions: `make test`
