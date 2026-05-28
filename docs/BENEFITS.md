# Benefits of dodo

Four core benefits for marketing materials and the website. Benefits 1--3 are available now in the open-source compiler and DuckDB extension. Benefit 4 is planned for dodo Studio (coming soon).

## 1. Open source and free

The dodo compiler (`dodoc`) and the DuckDB extension are both open source under the MIT license. You can use them anywhere DuckDB runs --- on your laptop, on a server, in the cloud, embedded in an application. There is no vendor lock-in and no license fees. The compiler and extension will always remain open source.

## 2. Speed

Your data pipeline runs in seconds, not minutes. On datasets of 10 million rows, dodo is at least 10--20x faster than legacy tools. This speed difference changes how you work: you can rerun your entire analysis every time something changes, catching errors immediately rather than waiting until the end. Fast iteration enables a continuous-integration mindset for data analysis --- you can be confident that your results are correct at every stage.

*(Precise benchmarks with real-world datasets are forthcoming.)*

## 3. Scale

dodo handles datasets of enormous size. You can work with billions of rows in modern columnar formats like Parquet, connect directly to relational databases, and take advantage of DuckDB's optimized analytical engine. Lazy loading means that if you have a Parquet file with billions of rows, the data is not materialized until the final result is needed --- and DuckDB's query optimizer fuses the entire CTE chain, so intermediate steps are never computed unnecessarily.

## 4. Iterative and reproducible workflows

*(Coming soon --- planned for dodo Studio.)*

Work iteratively while remaining fully reproducible. dodo will support named checkpoints (beyond the current undo/redo), so you can snapshot the state of your data at any point, keep working, and return to a named checkpoint at any time. Checkpoints do not slow you down: you can inspect how data looked at any prior stage while continuing to make changes, and see results update in near real time as you revise code.

The entire pipeline can be recorded and replayed for reproducibility. Once finalized, it can be executed in parallel at high speed. Named checkpoints also support an experimental workflow: start exploring, name a checkpoint, try something, and roll back if it does not work --- all without losing track of what you did.
