-- Generate a 10M row test dataset for benchmarking
-- Run: ./build/release/duckdb < test/data/generate_large.sql
COPY (
    SELECT
        i AS id,
        (i % 1000) + 1 AS firm_id,
        2010 + (i % 15) AS year,
        CASE i % 5
            WHEN 0 THEN 'manufacturing'
            WHEN 1 THEN 'services'
            WHEN 2 THEN 'retail'
            WHEN 3 THEN 'finance'
            WHEN 4 THEN 'technology'
        END AS sector,
        round(random() * 10000 + 100, 2) AS revenue,
        round(random() * 5000 + 50, 2) AS cost,
        (random() * 500)::INTEGER + 1 AS employees
    FROM generate_series(1, 10000000) AS t(i)
) TO 'test/data/large.csv' (HEADER);
