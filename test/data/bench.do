* Benchmark: collapse on 10M rows
* Tests: CSV read, filter, generate, collapse with by()
use "test/data/large.csv", clear
keep if year >= 2020
generate profit = revenue - cost
collapse (mean) avg_revenue = revenue avg_profit = profit (sum) total_revenue = revenue (count) n = id, by(sector year)
sort sector year
