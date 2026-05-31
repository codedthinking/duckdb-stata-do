# dodo -- Columnar Version Control for Interactive SQL
## Implementation Plan

---

## Overview

dodo is a content-addressable version control system for interactive SQL
work on columnar data. It follows the git object model -- commits, refs,
branches, tags -- but applied to dataframes: SQL transforms are commits,
the current dataframe is HEAD, undo is reset, alternatives are branches.

dodo already has a working CTE chain (`DodoState::cte_steps`) with linear
undo/redo, preserve/restore, and a command history table. This plan
extends that foundation with a persistent DAG of commits, branches, tags,
and a branch stack -- turning dodo's linear history into git-style
nonlinear version control.

The backing store is DuckDB itself: metadata tables live in the `dodo`
schema alongside the user's data. No external dependencies beyond what the
extension already links.

---

## Current State (what exists)

```
DodoState
  cte_steps[]       -- vector of inner SQL strings, one per transform
  cte_commands[]    -- original command text, parallel to cte_steps
  redo_stack[]      -- (command, sql) pairs popped by undo
  step_counter      -- next CTE index (_s0, _s1, ...)
  preserve/restore  -- checkpoint into cte_steps by index
```

Linear history. Undo pops from `cte_steps`, pushes to `redo_stack`. A new
command after undo clears the redo stack (destructive). No branches, no
tags, no persistence across sessions.

---

## Target State (what we are building)

```
DodoState
  cte_steps[]       -- unchanged: in-session CTE chain
  cte_commands[]    -- unchanged: parallel command text
  step_counter      -- unchanged: CTE naming (_sN)

  commits table     -- persistent DAG of all transforms
  branches table    -- named mutable pointers to commits
  tags table        -- named immutable pointers to commits
  head table        -- current branch (or detached commit)
  branch_stack      -- saved positions for /btw and /back
```

The CTE chain remains the live execution mechanism. The commit DAG is a
parallel persistent record. Every `AddStep()` also writes a commit.
Undo moves HEAD back along the parent chain instead of popping a stack.
Branching is automatic when the user diverges after undo.

---

## The Git Analogy

| Git                    | dodo                                      |
|------------------------|-------------------------------------------|
| Working tree           | Current query result (the dataframe)      |
| Commit                 | One dodo command (SQL transform)          |
| Commit message         | Original command text                     |
| Commit tree            | Output schema (column names + types)      |
| Commit parent(s)       | Input dataframe(s) -- one for most commands, two for merge |
| HEAD                   | Current position in the DAG               |
| Branch                 | Named pointer to a commit, advances on new commands |
| Tag                    | Named immutable snapshot                  |
| `git reset --soft`     | `/undo` -- move HEAD back, keep commits   |
| `git checkout -b`      | Auto-branch on divergence after undo      |
| `git stash` + `git stash pop` | `/btw` + `/back`                 |
| `git log`              | `/log`                                    |
| `__head__` view        | `_dodo_data` live view                    |
| `.git/objects/`        | `dodo.commits`, `dodo.blobs`              |
| `.git/refs/`           | `dodo.branches`, `dodo.tags`, `dodo.head` |

---

## Object Model

### Commit

The core object. Each dodo command that transforms data creates one commit.

```
commit
  hash          -- SHA-256 of (sorted parent hashes + canonical_sql)
  parents[]     -- hashes of parent commits (empty for root)
  sql_text      -- the SQL that was executed (a pure function: parents -> sql -> output)
  command_text  -- original dodo command text (for display)
  input_tree    -- schema hash before transform (deferred, may be NULL)
  output_tree   -- schema hash after transform (deferred, may be NULL)
  row_count     -- number of rows in result
  duration_ms   -- execution time
  created_at    -- timestamp
```

Each SQL transform is a pure function: it takes one or more input
dataframes (the parents) and produces one output dataframe. The commit
hash is `H(sorted_parent_hashes + sql)`, so it encodes the full lineage
of every input. Two users who run the same commands on the same inputs
get the same hashes -- a Merkle DAG.

Parent commits are derived automatically from the SQL AST. Parsing the
generated SQL with `json_serialize_sql()` exposes all table references
in the `from_table` nodes (type `BASE_TABLE`, field `table_name`). Each
referenced table that maps to a known branch or CTE step becomes a
parent edge. This replaces hardcoded per-command parent logic:

- A query referencing only `_dodo_data` (the current HEAD view) has one
  parent: the current HEAD commit.
- A query joining `_dodo_data` with another branch's view or an external
  file has two parents.
- A `use "file.csv"` that references no existing tables has zero parents
  (root commit).

This means the DAG structure is derived from the SQL itself, not from
command type conventions.

The parent pointers form a DAG. Linear history is a chain. Branching
creates a fork (two commits share the same parent). Merging creates a
join (one commit has two parents).

### Tree (deferred)

A schema snapshot: sorted list of (column_name, dtype) pairs, hashed for
identity. Two commits with the same output_tree have identical schemas.

Trees are cheap to compute (just read DuckDB column metadata) but not
essential for the core workflow. Implementation is deferred to Phase 3.

### Blob (deferred)

Content hash of column data or SQL text. Enables Merkle tree integrity:
verify that the entire chain is intact by checking the root hash. Also
enables deduplication: identical SQL transforms share a blob.

Deferred to Phase 3. The commit stores `sql_text` inline for now.

### Tag

An immutable named pointer to a commit.

```
tag
  name          -- user-chosen name
  target        -- commit hash
  message       -- optional annotation
  created_at    -- timestamp
```

### Branch

A mutable named pointer to a commit. Advances when a new commit is made
on this branch.

```
branch
  name          -- user-chosen or auto-generated name
  target        -- commit hash (moves forward on commit)
  created_at
  updated_at
```

### HEAD

Singleton. Points to either a branch name (attached) or a commit hash
(detached).

```
head
  branch_name   -- non-NULL when attached
  commit_hash   -- non-NULL when detached
  (exactly one is set)
```

Resolving HEAD: if attached, follow `branch_name` to get the commit hash.
If detached, use `commit_hash` directly.

---

## DuckDB Schema

All metadata lives in the `dodo` schema, which already exists for
`dodo._current` and `dodo._history`. New tables:

```sql
CREATE TABLE IF NOT EXISTS dodo.commits (
    hash         VARCHAR PRIMARY KEY,
    sql_text     VARCHAR,
    command_text VARCHAR,
    input_tree   VARCHAR,     -- NULL until Phase 4
    output_tree  VARCHAR,     -- NULL until Phase 4
    row_count    BIGINT,
    duration_ms  BIGINT,
    created_at   TIMESTAMPTZ DEFAULT now()
);

-- Parent edges, derived from table references in the SQL AST.
-- Most commits have one parent. Joins across branches have two.
-- Root commits (file loads) have zero rows here.
-- parent_index: 0 = primary parent (first table reference),
--               1 = secondary parent (second table reference), etc.
CREATE TABLE IF NOT EXISTS dodo.commit_parents (
    commit_hash  VARCHAR NOT NULL REFERENCES dodo.commits(hash),
    parent_hash  VARCHAR NOT NULL REFERENCES dodo.commits(hash),
    parent_index INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (commit_hash, parent_index)
);

CREATE TABLE IF NOT EXISTS dodo.tags (
    name         VARCHAR PRIMARY KEY,
    target       VARCHAR NOT NULL REFERENCES dodo.commits(hash),
    message      VARCHAR,
    created_at   TIMESTAMPTZ DEFAULT now()
);

CREATE TABLE IF NOT EXISTS dodo.branches (
    name         VARCHAR PRIMARY KEY,
    target       VARCHAR NOT NULL REFERENCES dodo.commits(hash),
    created_at   TIMESTAMPTZ DEFAULT now(),
    updated_at   TIMESTAMPTZ DEFAULT now()
);

CREATE TABLE IF NOT EXISTS dodo.head (
    id           INTEGER PRIMARY KEY DEFAULT 1,
    branch_name  VARCHAR REFERENCES dodo.branches(name),
    commit_hash  VARCHAR REFERENCES dodo.commits(hash),
    CHECK (
        (branch_name IS NOT NULL AND commit_hash IS NULL) OR
        (branch_name IS NULL     AND commit_hash IS NOT NULL)
    )
);

CREATE TABLE IF NOT EXISTS dodo.branch_stack (
    stack_depth  INTEGER PRIMARY KEY,
    branch_name  VARCHAR NOT NULL,
    head_at_push VARCHAR NOT NULL REFERENCES dodo.commits(hash),
    pushed_at    TIMESTAMPTZ DEFAULT now()
);
```

### Views

```sql
-- Resolved HEAD: actual commit hash regardless of attached/detached.
CREATE OR REPLACE VIEW dodo.head_commit AS
SELECT
    COALESCE(b.target, h.commit_hash) AS commit_hash,
    h.branch_name,
    h.commit_hash IS NOT NULL AS is_detached
FROM dodo.head h
LEFT JOIN dodo.branches b ON b.name = h.branch_name;

-- Log: recursive walk from HEAD along primary parent chain (parent_index=0).
CREATE OR REPLACE VIEW dodo.log AS
WITH RECURSIVE chain AS (
    SELECT c.*, 0 AS depth
    FROM dodo.commits c
    JOIN dodo.head_commit h ON h.commit_hash = c.hash
    UNION ALL
    SELECT c.*, chain.depth + 1
    FROM dodo.commits c
    JOIN dodo.commit_parents p ON p.parent_hash = c.hash
    JOIN chain ON p.commit_hash = chain.hash
    WHERE p.parent_index = 0
)
SELECT chain.*, t.name AS tag_name
FROM chain
LEFT JOIN dodo.tags t ON t.target = chain.hash
ORDER BY depth;

-- Children index: detect forks (via primary parent only).
CREATE OR REPLACE VIEW dodo.children AS
SELECT parent_hash AS commit_hash, commit_hash AS child_hash
FROM dodo.commit_parents WHERE parent_index = 0;

-- Branch overview.
CREATE OR REPLACE VIEW dodo.branch_status AS
SELECT
    br.name,
    br.target,
    c.created_at AS last_commit_at,
    c.command_text AS last_command,
    c.row_count,
    br.name = h.branch_name AS is_current
FROM dodo.branches br
JOIN dodo.commits c ON c.hash = br.target
JOIN dodo.head h ON TRUE
ORDER BY is_current DESC, last_commit_at DESC;
```

---

## Commit Hashing

A commit hash is computed from its parents and its SQL:

```
SHA-256(sorted_parent_hashes + "\0" + canonical_sql)
```

Parent hashes are sorted lexicographically and concatenated with `\0`
separators before the SQL. This ensures the hash is deterministic
regardless of the order parents are listed.

- **Zero parents** (root commit): hash is `SHA-256("\0" + source_description)`.
- **One parent** (most commands): hash is `SHA-256(parent_hash + "\0" + sql)`.
- **Two parents** (merge): hash is `SHA-256(min(p1,p2) + "\0" + max(p1,p2) + "\0" + sql)`.

Since each parent hash encodes its own full ancestry, the commit hash is
a Merkle DAG: changing any ancestor changes all descendant hashes. Two
users who run the same commands on the same inputs get the same hashes.

### SQL analysis with `json_serialize_sql()`

DuckDB's `json_serialize_sql()` parses SQL into a JSON AST. This serves
two purposes: canonicalization and dependency extraction.

#### Canonicalization

Round-tripping through `json_deserialize_sql(json_serialize_sql(sql))`
produces a canonical form: explicit keywords, consistent casing,
normalized whitespace. For example, `from services limit 5` becomes
`SELECT * FROM services LIMIT 5`.

Use the round-tripped SQL as the canonical form before hashing:

```sql
SELECT sha256(
    parent_hash || E'\0' ||
    json_deserialize_sql(json_serialize_sql(sql_text))
) AS hash
```

This is a syntactic normalization, not a semantic one: different column
orders or alias names produce different hashes, which is correct since
they produce different dataframes. Correctness does not depend on
perfect canonicalization -- it only affects deduplication.

#### Dependency extraction (parent detection)

The same JSON AST exposes all table references in the query. Nodes with
`"type": "BASE_TABLE"` contain a `"table_name"` field. Extracting these
gives the set of tables a transform reads from:

```sql
-- Extract table references from a query's AST
SELECT DISTINCT json_extract_string(node, '$.table_name') AS table_name
FROM (
    SELECT unnest(
        json_extract(json_serialize_sql(sql_text), '$.statements[*].node.from_table')
    ) AS node
)
WHERE json_extract_string(node, '$.type') = 'BASE_TABLE';
```

Each referenced table that maps to a known dodo object (branch view,
CTE step name, or external file) becomes a parent edge in
`dodo.commit_parents`. This replaces hardcoded per-command parent logic
-- the DAG structure is derived from the SQL itself.

For joins and subqueries, the AST contains nested `from_table` nodes
that can be extracted recursively. The extraction query may need to walk
`join` nodes and subselects to collect all table references.

---

## Data Flow: How a Command Becomes a Commit

Current flow (unchanged):
```
user types command
    |
    v
TokenizeCommand()           -- parse into DodoCommand
    |
    v
ProcessCommand()            -- generate SQL, call AddStep()
    |
    v
AddStep(inner_sql)          -- append to cte_steps[], increment step_counter
    |
    v
BuildQuery() + execute      -- run the full CTE chain in DuckDB
    |
    v
display result
```

New flow (additions marked with *):
```
user types command
    |
    v
TokenizeCommand()
    |
    v
ProcessCommand()            -- generate SQL, call AddStep()
    |
    v
AddStep(inner_sql)
    |
    v
* parse SQL AST             -- json_serialize_sql(inner_sql)
*   extract table refs      -- find BASE_TABLE nodes -> parent commits
    |
    v
* head_has_children()?      -- does current HEAD have child commits?
*   YES -> auto_branch()    -- create new branch before committing
    |
    v
* compute commit hash       -- SHA-256(sorted parents + canonical sql)
    |
    v
* INSERT INTO dodo.commits  -- persist the commit
    |
    v
* INSERT INTO dodo.commit_parents -- record parent edges from AST
    |
    v
* UPDATE dodo.branches      -- advance current branch target
    |
    v
BuildQuery() + execute
    |
    v
* UPDATE dodo.commits       -- record row_count, duration_ms
    |
    v
display result
```

The key principle: the CTE chain (`_s0`, `_s1`, ...) remains the execution
mechanism. The commit table is a parallel persistent log. The two are kept
in sync but serve different purposes: CTE chain for query execution,
commit DAG for history navigation.

---

## CTE Chain and Commit DAG: How They Relate

The CTE chain is an in-session array of SQL strings with sequential names.
The commit DAG is a persistent graph of commit objects linked by parent
edges.

When the session starts, the CTE chain is empty and HEAD points to
whatever commit was current when the session ended (or nothing, for a
fresh database).

**On session start (if commits exist):**
Walk the commit DAG from HEAD back to root, rebuild the CTE chain from
each commit's `sql_text`. This restores the full transformation pipeline.

**On each command:**
`AddStep()` appends to the CTE chain *and* writes a commit to
`dodo.commits`. The CTE name `_sN` corresponds to commit depth N from
root in the current branch.

**On undo:**
Move HEAD back N commits along the parent chain. Truncate the CTE chain
to match. Old commits remain in `dodo.commits` (they dangle, like
unreferenced git commits). No redo stack needed -- the commits are still
there, reachable by hash or branch name.

**On branch switch:**
Walk the target branch's commit chain from its target back to root. Rebuild
the CTE chain from scratch. This is O(chain length) but chains are
typically short (tens to low hundreds of commands).

---

## User-Facing Commands

### New slash commands

| Command                   | Git equivalent         | Action |
|---------------------------|------------------------|--------|
| `/undo [N]`               | `git reset HEAD~N`     | Move HEAD back N commits. Truncate CTE chain. |
| `/branch <name>`          | `git checkout -b`      | Create branch at HEAD, switch to it. |
| `/switch <name\|hash>`    | `git checkout`         | Switch to branch, tag, or commit hash prefix. Rebuild CTE chain. |
| `/tag <name> [message]`   | `git tag -a`           | Create immutable named pointer at HEAD. |
| `/btw [name]`             | `git stash` + `git checkout -b` | Push current branch to stack, start new branch. |
| `/back`                   | `git stash pop`        | Pop branch stack, restore previous branch. |
| `/log [N]`                | `git log`              | Show commit history from HEAD. |
| `/branches`               | `git branch`           | List all branches with status. |
| `/diff [target]`          | `git diff`             | Column-level diff between HEAD and target. |
| `/export`                 | `git format-patch`     | Emit self-contained SQL script from commit chain. |

### Modified existing commands

| Command       | Current behavior              | New behavior |
|---------------|-------------------------------|--------------|
| `undo [N]`    | Pop cte_steps to redo_stack   | Move HEAD back N commits. Old commits dangle. |
| `redo [N]`    | Pop redo_stack to cte_steps   | Replaced by `/switch` to the dangling commit or branch. |
| `history`     | Show linear step list         | Show `/log` (DAG-aware, shows branches and tags). |
| `preserve`    | Save index into cte_steps     | Create anonymous tag at HEAD. |
| `restore`     | Truncate cte_steps to index   | Switch HEAD to the preserve tag. |

### Automatic branching

When the user undoes and then types a new command, the current HEAD has
children (the commits that were "undone"). Before committing, dodo
automatically:

1. Creates a new branch with an auto-generated name (e.g., `alt-1`,
   `alt-2`).
2. Points the new branch at the current HEAD commit.
3. Switches HEAD to the new branch.
4. Commits the new command on the new branch.

The old branch still points to its tip. The user can `/switch` back at
any time. This replaces the destructive redo_stack with nondestructive
branching.

```
main:    use -> keep -> generate -> collapse
                                       ^
                                       | HEAD was here, user did /undo 2
                        HEAD is here --+
                                       |
                                       v
alt-1:                  keep -> sort -> ...  (user typed new commands)
```

### /btw and /back (branch stack)

For "by the way" tangents during interactive work.

`/btw aggregate_employment`:
1. Push current (branch_name, HEAD commit) onto `dodo.branch_stack`.
2. Create new branch `aggregate_employment` at HEAD (or from scratch if
   given a file argument).
3. Switch HEAD to the new branch.

`/back`:
1. Pop top of `dodo.branch_stack`.
2. Switch HEAD to the saved branch at the saved commit.
3. Rebuild CTE chain.

The stack supports nesting: `/btw` inside a `/btw` pushes another frame.

---

## Integration with Existing Architecture

### Where the new code lives

The commit/branch/tag logic is purely data manipulation (INSERT, UPDATE,
SELECT on `dodo.*` tables). It belongs in `dodo_core.hpp` / `dodo_core.cpp`,
not in the extension layer. The extension layer (`dodo_extension.cpp`)
only needs to ensure the schema is created on load.

New additions to `DodoState`:

```cpp
struct DodoState {
    // ... existing fields unchanged ...

    //! Current branch name (empty string = detached HEAD)
    std::string current_branch;

    //! Current HEAD commit hash (empty string = no commits yet)
    std::string head_commit;

    //! Whether the dodo.commits schema has been initialized
    bool vcs_initialized = false;
};
```

New free functions in `dodo_core.hpp`:

```cpp
//! Initialize the VCS schema (commits, branches, tags, head tables).
//! Idempotent: CREATE TABLE IF NOT EXISTS.
//! Called on first use command when vcs is not yet initialized.
std::string InitVcsSchema();

//! Create a commit record. Returns the commit hash.
//! Called from AddStep() after appending to cte_steps.
std::string CreateCommit(DodoState &state, const std::string &sql_text,
                         const std::string &command_text);

//! Create a root commit (file load, no parent, no SQL).
std::string CreateRootCommit(DodoState &state,
                             const std::string &source_description);

//! Move HEAD back N commits. Returns SQL to execute.
std::string UndoCommits(DodoState &state, int n);

//! Create a new branch at HEAD. Returns SQL to execute.
std::string CreateBranch(DodoState &state, const std::string &name);

//! Switch to a branch, tag, or commit hash prefix.
//! Returns SQL to rebuild the CTE chain from the target commit.
std::string SwitchTo(DodoState &state, const std::string &name_or_hash);

//! Create a tag at HEAD.
std::string CreateTag(DodoState &state, const std::string &name,
                      const std::string &message);

//! Push current branch to stack, create new branch.
std::string Btw(DodoState &state, const std::string &new_branch);

//! Pop branch stack, restore previous branch.
std::string Back(DodoState &state);

//! Generate SQL for the log view (delegates to dodo.log).
std::string ShowLog(DodoState &state, int max_entries);

//! Generate SQL for branch listing.
std::string ShowBranches(DodoState &state);

//! Walk commit chain from HEAD, emit self-contained SQL script.
std::string ExportSql(DodoState &state);

//! Check whether HEAD's commit has children in the DAG.
//! Used to trigger auto-branching before commit.
std::string CheckForFork(DodoState &state);

//! Generate an auto-branch name ("alt-1", "alt-2", ...).
std::string AutoBranchName(DodoState &state);
```

### CTE naming stays as-is

The in-session CTE chain uses `_s0`, `_s1`, ... (sequential, simple,
fast). Commit hashes are used only in the persistent `dodo.commits` table
for content-addressable lookup. The two naming schemes serve different
purposes and do not conflict.

### _dodo_data view stays as-is

The live view (`_dodo_data`) is already refreshed after each command by
`BuildLiveViewSQL()` in the extension layer. It continues to point to the
latest CTE step. When HEAD moves (undo, switch), the CTE chain is rebuilt
and the view is refreshed -- same mechanism, different trigger.

### History table becomes a view over commits

The current `dodo._history` table is rebuilt from `cte_commands[]` after
every command. Once commits are persisted, `_history` becomes a simple
view over `dodo.log`:

```sql
CREATE OR REPLACE VIEW dodo._history AS
SELECT depth AS step_id, command_text AS command, FALSE AS undone
FROM dodo.log
ORDER BY depth DESC;
```

This preserves backward compatibility for users querying `dodo._history`.

---

## Implementation Phases

### Phase 1: Persist linear history (commits only)

**Goal:** Every command writes a commit. History survives session restart.

1. Add `dodo.commits` and `dodo.commit_parents` tables to schema
   initialization.
2. On `use` (root commit): INSERT into `dodo.commits` with no parent
   edges.
3. On each `AddStep()`: parse the SQL with `json_serialize_sql()`,
   extract all `BASE_TABLE` references from the AST, resolve each to
   a parent commit hash, INSERT into `dodo.commits`, then INSERT into
   `dodo.commit_parents` with one row per resolved parent.
4. Compute commit hash using DuckDB's built-in `sha256()` and
   `json_serialize_sql()` functions for canonicalization:
   ```sql
   -- single parent (most commands):
   SELECT sha256(parent_hash || E'\0' || json_deserialize_sql(json_serialize_sql(sql_text))) AS hash
   -- two parents (merge):
   SELECT sha256(least(p1, p2) || E'\0' || greatest(p1, p2) || E'\0' || json_deserialize_sql(json_serialize_sql(sql_text))) AS hash
   -- root (no parents):
   SELECT sha256(E'\0' || source_description) AS hash
   ```
5. Store `head_commit` in `DodoState` (in-memory tracking).
6. On session start with existing commits: rebuild CTE chain from the
   latest commit's ancestor chain.
7. Replace `history` command to read from `dodo.commits`.
8. Existing `undo`/`redo` behavior unchanged in this phase.

**Test:** Run a sequence of commands, close DuckDB, reopen, verify
`history` shows the same commands. Run `SELECT * FROM dodo.commits`.

### Phase 2: Branches and HEAD

**Goal:** Named branches. HEAD tracks current position. Undo moves HEAD
instead of popping a stack.

1. Add `dodo.branches`, `dodo.head` tables.
2. On `use`: create `main` branch, set HEAD to attached.
3. On each commit: advance branch target.
4. Rewrite `undo` to move HEAD back N commits (UPDATE branch target).
   Rebuild CTE chain from new HEAD.
5. Remove `redo_stack` from `DodoState`. Redo is replaced by `/switch`
   to the dangling commit.
6. Add `/branch <name>` command: create branch at HEAD, switch to it.
7. Add `/switch <name>` command: resolve name (branch, tag, or hash
   prefix), walk commit chain, rebuild CTE chain.
8. Add auto-branching: before commit, check if HEAD's commit has children.
   If yes, create `alt-N` branch and switch to it before committing.

**Test:** Undo 2, type new command, verify auto-branch created. `/switch`
back to main, verify CTE chain restored. `/branch experiment`, add
commands, `/switch main`, verify independent histories.

### Phase 3: Tags, branch stack, export

**Goal:** Full interactive workflow with tags, /btw, /back, and SQL export.

1. Add `dodo.tags` table.
2. Add `/tag <name> [message]` command.
3. Rewrite `preserve` as `/tag _preserve_N` (auto-named tag).
4. Rewrite `restore` as `/switch _preserve_N`.
5. Add `dodo.branch_stack` table.
6. Add `/btw [name]` command: push current state, create/switch branch.
7. Add `/back` command: pop stack, restore state.
8. Add `/log` command: query `dodo.log` view.
9. Add `/branches` command: query `dodo.branch_status` view.
10. Add `/export` command: walk commit chain from HEAD, emit `WITH` CTE
    chain as standalone SQL script with commit hash comments.

**Test:** `/btw side_task`, do work, `/back`, verify main branch restored.
Nested `/btw` inside `/btw`. `/tag checkpoint`, modify, `/switch checkpoint`.
`/export` produces valid SQL that can be piped to `duckdb`.

### Phase 4: Trees, blobs, Merkle integrity (future)

**Goal:** Full content-addressable object model. Schema snapshots. Column-
level diff. Deduplication.

1. Add `dodo.trees` table (schema snapshots).
2. On each commit, derive tree from DuckDB result metadata.
3. Add `dodo.blobs` table for SQL text deduplication.
4. Add column-level `/diff` between any two commits (compare trees).
5. Optionally: hash column data (Arrow buffers) for full Merkle integrity.
   Defer this to `/tag` or `/save` to keep the REPL responsive.

This phase is speculative. Phases 1-3 deliver the full interactive
workflow. Phase 4 adds integrity and provenance guarantees.

---

## Non-Goals (explicitly deferred)

- Remote sync / shared repos.
- Full SQL parser (use EXPLAIN canonicalization or raw text).
- GUI / TUI beyond the DuckDB shell.
- Arrow buffer hashing on every commit (Phase 4, deferred to /tag).
