+++
title = "How fast can Postgres insert 10 million rows?"
date = "2026-04-17"
description = "I benchmarked 10M-row insert strategies in Postgres"
category = "tech"
tags = [
    "postgres",
    "performance",
    "docker",
]
+++

I kept hearing that Postgres is slow for bulk inserts.
So I wanted to test it myself and see how fast I could get 10 million rows in.

## Test setup

- Postgres 16 (Docker image: `postgres:16`)
- Data shape: `id BIGINT, payload TEXT`
- Benchmark machine:
  - MacBook Pro (Mac14,7)
  - Apple M2 (8 cores)
  - 8 GB RAM
  - macOS 26.3.1
- Docker runtime during test:
  - Docker 29.1.2
  - 8 CPUs
  - ~4.1 GB memory available to Docker

I measured wall-clock time and computed rows/sec.

## 10 million row bulk load results

For this benchmark, each run inserted **10,000,000 rows**.

I ran each strategy in a **fresh Postgres container**, randomized order every round, and repeated for **5 rounds** to reduce warm-cache and order bias.

### What I tested

1. Logged table + `INSERT ... SELECT generate_series(...)`
2. Logged table with PK + `INSERT ... SELECT`
3. UNLOGGED table + `INSERT ... SELECT`
4. UNLOGGED table + `COPY FROM PROGRAM`

### Results (isolated runs, n=5)

| Strategy | Min (s) | Median (s) | P95 (s) | Max (s) |
|---|---:|---:|---:|---:|
| logged insert-select | 12.505 | 13.720 | 17.205 | 17.205 |
| logged insert-select + primary key | 20.979 | 23.919 | 31.859 | 31.859 |
| unlogged insert-select | 4.753 | 6.347 | 10.510 | 10.510 |
| unlogged copy | **4.103** | **4.967** | **6.305** | **6.305** |

## Multi-row INSERT benchmark

Then I wanted to isolate one specific question: how much does batching help if you are using plain `INSERT ... VALUES` statements?

For this benchmark, each case inserted **1,000,000 rows** into a logged table with a primary key, all in one transaction.

I ran each strategy in a **fresh Postgres container**, randomized order every round, and repeated for **5 rounds**.

### What I tested

1. Single-row inserts (`VALUES (..)` one row at a time)
2. Batched inserts with 100 rows/statement
3. Batched inserts with 1,000 rows/statement
4. Batched inserts with 5,000 rows/statement

### Results (isolated runs, n=5)

| Strategy | Min (s) | Median (s) | P95 (s) | Max (s) |
|---|---:|---:|---:|---:|
| single-row insert | 86.297 | 90.726 | 97.220 | 97.220 |
| batch 100 rows | 4.170 | 5.306 | 9.657 | 9.657 |
| batch 1,000 rows | **3.081** | **5.275** | 9.850 | 9.850 |
| batch 5,000 rows | 4.797 | 5.383 | 6.439 | 6.439 |

Batching gave a massive jump over single-row inserts (~17x using median values).
At this scale, batch sizes 100 to 5000 were in a similar range, with 1,000 rows slightly ahead on median.

## What I learned

Biggest jump for bulk loading came from **UNLOGGED + COPY**.

After isolating runs, logged insert with primary key was clearly slower than logged insert without primary key, which is what you would expect from index maintenance overhead.

For normal application-style inserts, batching multiple rows per statement is still a huge win over one-row-per-statement, and isolated runs showed roughly a 17x median improvement.

Postgres is not "slow" here, it is doing durability and consistency work by default, and your write pattern decides how much overhead you pay.

## Practical takeaway

If you care about raw insert speed:

1. Prefer `COPY` for large bulk loads
2. Use `UNLOGGED` tables for staging/bulk pipelines
3. Batch inserts (`VALUES (...), (...), ...`) instead of single-row statements
4. Add indexes/constraints after loading (when possible)

For production flows where durability matters at all times, stick with logged tables and accept lower throughput.
