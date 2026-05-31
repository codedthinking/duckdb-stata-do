#!/usr/bin/env bash
# E2E test: DuckDB CLI with dodo extension
# Test cases are defined in cases.json; this script runs each via the CLI.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
DUCKDB="$PROJECT_DIR/build/release/duckdb"
CASES="$SCRIPT_DIR/cases.json"
DATA_DIR="$PROJECT_DIR/test/data"

if [ ! -x "$DUCKDB" ]; then
    echo "FAIL: DuckDB binary not found at $DUCKDB"
    exit 1
fi

FAILURES=0
N_CASES=$(jq length "$CASES")

echo "=== DuckDB CLI e2e tests ==="

for i in $(seq 0 $((N_CASES - 1))); do
    name=$(jq -r ".[$i].name" "$CASES")
    setup=$(jq -r ".[$i].setup // empty" "$CASES")
    # Build input: setup SQL + commands, each terminated with semicolon
    input=""
    if [ -n "$setup" ]; then
        input+="$setup;"$'\n'
    fi
    while IFS= read -r cmd; do
        cmd="${cmd//\{data\}/$DATA_DIR}"
        input+="$cmd;"$'\n'
    done < <(jq -r ".[$i].commands[]" "$CASES")

    actual=$(echo "$input" | "$DUCKDB" -noheader -list 2>&1) || true

    # Check expectations based on type
    expect_type=$(jq -r ".[$i].expect.type" "$CASES")
    ok=true

    case "$expect_type" in
        scalar)
            expected=$(jq -r ".[$i].expect.value" "$CASES")
            # The last line of output should contain the scalar value
            if ! echo "$actual" | grep -qF "$expected"; then
                ok=false
            fi
            ;;
        contains_column)
            for inc in $(jq -r ".[$i].expect.includes[]" "$CASES" 2>/dev/null); do
                if ! echo "$actual" | grep -qF "$inc"; then
                    ok=false
                fi
            done
            for exc in $(jq -r ".[$i].expect.excludes[]" "$CASES" 2>/dev/null); do
                if echo "$actual" | grep -qF "$exc"; then
                    ok=false
                fi
            done
            ;;
        cell)
            expected=$(jq -r ".[$i].expect.value" "$CASES")
            if ! echo "$actual" | grep -qF "$expected"; then
                ok=false
            fi
            ;;
        row_count_and_cell)
            expected=$(jq -r ".[$i].expect.value" "$CASES")
            if ! echo "$actual" | grep -qF "$expected"; then
                ok=false
            fi
            ;;
    esac

    if $ok; then
        echo "  PASS: $name"
    else
        echo "  FAIL: $name"
        echo "    actual: $actual"
        FAILURES=$((FAILURES + 1))
    fi
done

if [ "$FAILURES" -gt 0 ]; then
    echo "=== $FAILURES test(s) FAILED ==="
    exit 1
fi

echo "=== All CLI tests passed ==="
