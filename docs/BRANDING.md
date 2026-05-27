# dodo Branding Brief

## Project

`dodo` is a DuckDB extension that reads and executes `.do` files.

Core idea:

> Legacy `.do` workflows, reborn in DuckDB.

The name works because it carries several meanings at once:

- `do`: direct resonance with `.do` files
- `dodo`: memorable, short, package-friendly
- DuckDB ecosystem: playful bird/duck adjacency
- Duck test: compatibility behavior matters more than ancestry
- Extinct bird: old analytical workflows brought back to life on a modern engine

## Brand Positioning

`dodo` should feel like a serious data infrastructure tool with a playful surface.

It is not a Stata clone, and the Stata trademark should not appear in the name or primary branding. It is a compatibility/runtime layer for `.do`-style workflows, implemented for DuckDB.

The brand should sit naturally next to playful DuckDB ecosystem names such as DuckLake, Quack, MotherDuck, and other duck-adjacent tooling.

## Personality

Tone:

- playful
- precise
- OSS-native
- hackerish
- warm but not cute
- technically credible
- slightly absurd in the DuckDB tradition

Avoid:

- enterprise BI blandness
- academic econometrics seriousness
- startup mascot over-polish
- legal/trademark proximity to Stata branding
- directly copying or modifying the official DuckDB mascot/logo

## Core Message

Primary one-liner:

> Run `.do` files in DuckDB.

More playful line:

> Walks like `.do`. Quacks like DuckDB.

Technical description:

> `dodo` is a DuckDB extension that reads legacy `.do` workflows and executes them on DuckDB, preserving familiar data-cleaning scripts while moving execution to a modern analytical engine.

Website hero option:

> Old scripts. New pond.

README subtitle:

> A DuckDB extension for running `.do`-style data workflows.

## Tagline Options

Best short taglines:

- Walks like `.do`. Quacks like DuckDB.
- Run `.do` files in DuckDB.
- Old scripts. New pond.
- Legacy `.do`, modern DuckDB.
- `.do` files, DuckDB speed.
- The duck test for `.do` files.
- Extinct scripts, live queries.
- Bring old `.do` workflows back to life.
- Quack-compatible `.do` execution.
- Your `.do` files found a new pond.

More playful variants:

- If it walks like `.do` and quacks like DuckDB...
- The dodo is back, and it runs your scripts.
- Old flock, new engine.
- Prehistoric scripts. Modern analytics.
- Looks like `.do`, runs like DuckDB.
- Less extinct than your legacy workflow.

Recommended public stack:

```text
dodo
Run .do files in DuckDB.
Walks like .do. Quacks like DuckDB.
```

## Naming Guidance

Preferred name:

```text
dodo
```

Use lowercase in most contexts.

Good usage:

- `dodo`
- `duckdb-dodo`
- `dodo_run('analysis.do')`
- `LOAD dodo;`
- `INSTALL dodo;`

Avoid:

- putting `Stata` in the extension name
- using `stata` in package identifiers
- implying official affiliation with StataCorp
- implying official affiliation with DuckDB Labs unless approved

Possible repository names:

- `duckdb-dodo`
- `dodo-duckdb`
- `dodo`

Recommended:

```text
duckdb-dodo
```

Reason: clear ecosystem discoverability, while the extension itself can still be called `dodo`.

## Visual Identity

### Overall Direction

The logo should be a dodo, not a duck.

It should be DuckDB-adjacent in spirit but visually distinct:

- geometric
- flat colors
- thick black outline
- strong silhouette
- works at 16px
- playful but infrastructural
- no gradients
- no realistic bird rendering

Avoid:

- modifying the DuckDB duck
- reusing the DuckDB mascot shape
- matching DuckDB yellow exactly
- creating a logo that looks like an official DuckDB sub-brand

### Primary Logo Concept

Use a side-profile dodo head/body inside a circular badge.

Key visual hook:

> The beak should read as a terminal prompt: `>`.

This makes the logo connect to:

- command execution
- scripting
- UNIX/tooling culture
- `.do` execution

Suggested simplifications:

- remove feet from the primary mark
- simplify or remove tail puff
- enlarge the beak by 15-25 percent
- make the `>` shape visually dominant
- keep the eye simple and dark
- avoid small internal details that collapse at favicon size

The primary icon should work as:

- GitHub organization/repository avatar
- DuckDB extension icon
- favicon
- documentation logo
- slide badge
- package registry icon

### Secondary Logo Concept

Use a railroad/pipeline diagram:

```text
.do -> dodo -> DuckDB
```

This should be used in:

- documentation landing page
- README architecture section
- slides
- explainer diagrams

It should not be the primary logo because it is too wide and too explanatory.

### Dark Mode Variant

A simplified circular dodo head can be used for dark mode.

Possible idea:

- dark circular background
- ochre dodo face
- one eye as `.`
- one eye or facial detail referencing `do`

This is clever but should remain secondary. The primary mark should be the terminal-beak dodo.

## Color Palette

Use warm fossil/ochre colors, distinct from DuckDB's exact official yellow.

Suggested palette:

| Role | Hex |
|---|---|
| Primary ochre | `#D8A44C` |
| Mid ochre | `#C49A42` |
| Accent brown | `#8C5A2B` |
| Fossil ivory | `#F3E7C9` |
| Outline black | `#1A1A1A` |
| Dark background | `#111111` |
| Warm page background | `#F7F2E8` |

Rationale:

- keeps bird/duck warmth
- suggests fossil/revival/dodo
- distinct enough from DuckDB
- works well with black outlines
- readable in light and dark documentation themes

## Typography

Recommended:

- Wordmark: JetBrains Mono
- Body/UI: IBM Plex Sans, Geist, or system sans
- Code examples: JetBrains Mono

Wordmark:

```text
dodo
```

Style:

- lowercase
- monospace
- bold
- slight negative tracking
- no cartoon typeface

Avoid:

- overly rounded startup fonts
- fake retro display fonts
- type that makes the project look like a toy

## Logo Lockups

### Primary lockup

```text
[dodo icon] dodo
            run .do files in duckdb
```

### Compact lockup

```text
[dodo icon] dodo
```

### README header

```text
# dodo

Run `.do` files in DuckDB.

Walks like `.do`. Quacks like DuckDB.
```

### CLI/SQL examples

```sql
INSTALL dodo;
LOAD dodo;

SELECT dodo_run('analysis.do');
```

Possible function names:

```text
dodo_run()
dodo_parse()
dodo_translate()
dodo_exec()
```

Recommended primary function:

```text
dodo_run()
```

It is obvious, discoverable, and playful enough.

## Copy Examples

### README Intro

```markdown
# dodo

Run `.do` files in DuckDB.

dodo reads legacy `.do` workflows and executes them on DuckDB. It is built for analysts, economists, and data teams who want to keep familiar script-shaped workflows while moving execution to a modern analytical engine.

Walks like `.do`. Quacks like DuckDB.
```

### Website Hero

```text
dodo

Old scripts. New pond.

Run `.do` files directly in DuckDB with a playful compatibility layer for legacy data workflows.
```

### Short Product Description

```text
dodo is a DuckDB extension for running `.do`-style data workflows on DuckDB.
```

### More Playful Description

```text
The dodo is back. This time it runs your old `.do` scripts on DuckDB.
```

## Design Checklist

Before accepting a logo, test it in these contexts:

- 16px favicon
- 32px GitHub avatar
- 64px README badge
- 128px documentation header
- dark mode
- monochrome
- printed slide
- next to DuckDB logo
- next to SQL code examples

Acceptance criteria:

- recognizable as a dodo or bird-like mascot
- not confusable with the DuckDB mascot
- beak/prompt idea survives at 32px
- silhouette survives in monochrome
- no thin details required for recognition
- feels like tooling, not a children's app
- playful enough for the DuckDB ecosystem

## Final Recommendation

Use:

```text
dodo
```

with the primary tagline:

```text
Run .do files in DuckDB.
```

and the playful secondary line:

```text
Walks like .do. Quacks like DuckDB.
```

Primary logo:

> A simplified side-profile dodo in a circular badge, with an enlarged `>` terminal-prompt beak, warm ochre/fossil palette, and thick black outline.

Secondary graphic system:

> `.do -> dodo -> DuckDB` railroad/pipeline diagrams for docs and presentations.
