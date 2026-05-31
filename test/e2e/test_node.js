// E2E test: Node.js duckdb client with dodo extension
const duckdb = require("duckdb");
const path = require("path");
const fs = require("fs");

const PROJECT_DIR = path.resolve(__dirname, "..", "..");
const EXT_PATH = path.join(
  PROJECT_DIR, "build", "release", "extension", "dodo", "dodo.duckdb_extension",
);
const DATA_DIR = path.join(PROJECT_DIR, "test", "data");
const CASES_PATH = path.join(__dirname, "cases.json");

if (!fs.existsSync(EXT_PATH)) {
  console.log(`FAIL: Extension not found at ${EXT_PATH}`);
  process.exit(1);
}

function freshConn() {
  return new Promise((resolve, reject) => {
    const db = new duckdb.Database(":memory:", {
      allow_unsigned_extensions: "true",
    });
    const con = db.connect();
    con.run(`LOAD '${EXT_PATH}'`, (err) => {
      if (err) reject(err);
      else resolve({ db, con });
    });
  });
}

function exec(con, sql) {
  return new Promise((resolve, reject) => {
    con.run(sql, (err) => {
      if (err) reject(err);
      else resolve();
    });
  });
}

function query(con, sql) {
  return new Promise((resolve, reject) => {
    con.all(sql, (err, rows) => {
      if (err) reject(err);
      else resolve(rows);
    });
  });
}

function assert(condition, msg) {
  if (!condition) throw new Error(msg);
}

function checkExpect(expect, rows) {
  const columns = rows.length > 0 ? Object.keys(rows[0]) : [];
  const t = expect.type;

  if (t === "scalar") {
    const val = Object.values(rows[0])[0];
    assert(val === expect.value, `expected ${expect.value}, got ${val}`);
  } else if (t === "contains_column") {
    const col = typeof expect.column === "string" ? expect.column : columns[expect.column];
    const values = rows.map((r) => r[col]);
    for (const inc of expect.includes || [])
      assert(values.includes(inc), `expected ${inc} in ${JSON.stringify(values)}`);
    for (const exc of expect.excludes || [])
      assert(!values.includes(exc), `unexpected ${exc} in ${JSON.stringify(values)}`);
  } else if (t === "cell") {
    const col = typeof expect.column === "string" ? expect.column : columns[expect.column];
    assert(rows[expect.row][col] === expect.value,
      `expected ${expect.value}, got ${rows[expect.row][col]}`);
  } else if (t === "row_count_and_cell") {
    assert(rows.length === expect.row_count,
      `expected ${expect.row_count} rows, got ${rows.length}`);
    const col = typeof expect.column === "string" ? expect.column : columns[expect.column];
    assert(rows[expect.row][col] === expect.value,
      `expected ${expect.value}, got ${rows[expect.row][col]}`);
  }
}

async function runCase(testCase) {
  const { con } = await freshConn();
  if (testCase.setup) await exec(con, testCase.setup);
  const cmds = testCase.commands.map((c) => c.replace(/\{data\}/g, DATA_DIR));
  for (const cmd of cmds.slice(0, -1)) await exec(con, cmd);
  const rows = await query(con, cmds[cmds.length - 1]);
  checkExpect(testCase.expect, rows);
}

async function main() {
  const cases = JSON.parse(fs.readFileSync(CASES_PATH, "utf8"));
  let failures = 0;

  console.log("=== Node.js e2e tests ===");
  for (const testCase of cases) {
    try {
      await runCase(testCase);
      console.log(`  PASS: ${testCase.name}`);
    } catch (e) {
      console.log(`  FAIL: ${testCase.name}`);
      console.log(`    ${e.message}`);
      failures++;
    }
  }

  if (failures > 0) {
    console.log(`=== ${failures} test(s) FAILED ===`);
    process.exit(1);
  }
  console.log("=== All Node.js tests passed ===");
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
