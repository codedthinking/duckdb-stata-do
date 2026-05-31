# E2E test: R DBI client with dodo extension
library(DBI)
library(duckdb)
library(jsonlite)

args <- commandArgs(trailingOnly = TRUE)
project_dir <- if (length(args) > 0) args[1] else getwd()
ext_path <- file.path(project_dir, "build", "release", "extension", "dodo", "dodo.duckdb_extension")
data_dir <- file.path(project_dir, "test", "data")
cases_path <- file.path(dirname(sys.frame(1)$ofile), "cases.json")

if (!file.exists(ext_path)) {
    cat("FAIL: Extension not found at", ext_path, "\n")
    quit(status = 1)
}

fresh_conn <- function() {
    con <- dbConnect(duckdb::duckdb(), dbdir = ":memory:")
    dbExecute(con, "SET allow_unsigned_extensions = true")
    dbExecute(con, sprintf("LOAD '%s'", ext_path))
    con
}

check_expect <- function(expect, result) {
    t <- expect$type
    if (t == "scalar") {
        stopifnot(result[[1]][1] == expect$value)
    } else if (t == "contains_column") {
        col <- if (is.character(expect$column)) expect$column else names(result)[expect$column + 1]
        values <- result[[col]]
        for (inc in expect$includes) stopifnot(inc %in% values)
        for (exc in expect$excludes) stopifnot(!(exc %in% values))
    } else if (t == "cell") {
        col <- if (is.character(expect$column)) expect$column else names(result)[expect$column + 1]
        stopifnot(result[[col]][expect$row + 1] == expect$value)
    } else if (t == "row_count_and_cell") {
        stopifnot(nrow(result) == expect$row_count)
        col <- if (is.character(expect$column)) expect$column else names(result)[expect$column + 1]
        stopifnot(result[[col]][expect$row + 1] == expect$value)
    }
}

cases <- fromJSON(cases_path, simplifyVector = FALSE)

failures <- 0L
cat("=== R e2e tests ===\n")

for (case in cases) {
    tryCatch({
        con <- fresh_conn()
        if (!is.null(case$setup)) dbExecute(con, case$setup)
        cmds <- unlist(case$commands)
        for (cmd in cmds[-length(cmds)]) {
            dbExecute(con, gsub("\\{data\\}", data_dir, cmd))
        }
        last_cmd <- gsub("\\{data\\}", data_dir, cmds[length(cmds)])
        result <- dbGetQuery(con, last_cmd)
        check_expect(case$expect, result)
        dbDisconnect(con, shutdown = TRUE)
        cat("  PASS:", case$name, "\n")
    }, error = function(e) {
        cat("  FAIL:", case$name, "\n")
        cat("   ", conditionMessage(e), "\n")
        failures <<- failures + 1L
    })
}

if (failures > 0) {
    cat(sprintf("=== %d test(s) FAILED ===\n", failures))
    quit(status = 1)
}
cat("=== All R tests passed ===\n")
