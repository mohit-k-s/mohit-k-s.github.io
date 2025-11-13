+++
title = "Missing Entries"
date = "2025-11-13"
description = "When Postgres sequences skip values"
tags = ["postgres"]
+++

I ran into an odd issue recently — a service threw an error saying the **primary key max value had been reached**.  
The column was `serial4`, so its max value was **2,147,483,647**, but the last inserted ID was only **2,146,602,558**.

At first, I had no idea why.  
As a quick fix, I changed the column to `bigserial`, which solved the immediate problem, but I wanted to know **why IDs were missing**.

Listing rows in descending order showed **gaps** in the primary keys.  
There were only about 16k entries in the table, so it clearly wasn’t full.

Digging into the code confirmed my suspicion:  
it used an **INSERT ... ON CONFLICT DO UPDATE** pattern.

Here’s a minimal reproduction:

```sql
CREATE TABLE seq_test (
    id SERIAL PRIMARY KEY,
    device_id INT UNIQUE,
    status TEXT
);

INSERT INTO seq_test (device_id, status)
VALUES (1, 'active');

INSERT INTO seq_test (device_id, status)
VALUES (1, 'inactive')
ON CONFLICT (device_id)
DO UPDATE SET status = EXCLUDED.status;

INSERT INTO seq_test (device_id, status)
VALUES (2, 'active');
```
Now check what’s stored:

```sql
select * from seq_test;

"id","device_id","status"
1,1,inactive
3,2,active

```
Notice the jump — the id for the second device is 3, not 2.

That’s because Postgres still calls nextval() for every insert, even if it hits a conflict and performs an update instead.

A small detail, but a good one to remember.


