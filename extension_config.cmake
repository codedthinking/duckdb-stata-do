# This file is included by DuckDB's build system. It specifies which extension to load

# Load dta extension first (dependency)
duckdb_extension_load(dta
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/duckdb-dta
)

# Extension from this repo
duckdb_extension_load(dodo
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)
