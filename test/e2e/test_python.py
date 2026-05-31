"""E2E test: Python duckdb client with dodo extension."""
import json
import os
import sys

import duckdb

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_PATH = os.path.join(PROJECT_DIR, "build", "release", "extension", "dodo", "dodo.duckdb_extension")
DATA_DIR = os.path.join(PROJECT_DIR, "test", "data")
CASES_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cases.json")


def fresh_conn():
    con = duckdb.connect(":memory:", config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{EXT_PATH}'")
    return con


def check(expect, columns, rows):
    t = expect["type"]
    if t == "scalar":
        assert rows[0][0] == expect["value"], f"expected {expect['value']}, got {rows[0][0]}"
    elif t == "contains_column":
        col_idx = columns.index(expect["column"]) if isinstance(expect["column"], str) else expect["column"]
        values = [row[col_idx] for row in rows]
        for inc in expect.get("includes", []):
            assert inc in values, f"expected {inc} in {values}"
        for exc in expect.get("excludes", []):
            assert exc not in values, f"unexpected {exc} in {values}"
    elif t == "cell":
        col_idx = columns.index(expect["column"]) if isinstance(expect["column"], str) else expect["column"]
        assert rows[expect["row"]][col_idx] == expect["value"], \
            f"expected {expect['value']}, got {rows[expect['row']][col_idx]}"
    elif t == "row_count_and_cell":
        assert len(rows) == expect["row_count"], f"expected {expect['row_count']} rows, got {len(rows)}"
        col_idx = columns.index(expect["column"]) if isinstance(expect["column"], str) else expect["column"]
        assert rows[expect["row"]][col_idx] == expect["value"], \
            f"expected {expect['value']}, got {rows[expect['row']][col_idx]}"


def run_case(case):
    con = fresh_conn()
    if "setup" in case:
        con.execute(case["setup"])
    for cmd in case["commands"][:-1]:
        con.execute(cmd.replace("{data}", DATA_DIR))
    result = con.execute(case["commands"][-1].replace("{data}", DATA_DIR))
    columns = [d[0] for d in result.description]
    rows = result.fetchall()
    check(case["expect"], columns, rows)


if __name__ == "__main__":
    if not os.path.exists(EXT_PATH):
        print(f"FAIL: Extension not found at {EXT_PATH}")
        sys.exit(1)

    with open(CASES_PATH) as f:
        cases = json.load(f)

    failures = 0
    print("=== Python e2e tests ===")
    for case in cases:
        try:
            run_case(case)
            print(f"  PASS: {case['name']}")
        except Exception as e:
            print(f"  FAIL: {case['name']}")
            print(f"    {e}")
            failures += 1

    if failures > 0:
        print(f"=== {failures} test(s) FAILED ===")
        sys.exit(1)
    print("=== All Python tests passed ===")
