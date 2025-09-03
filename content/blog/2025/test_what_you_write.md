+++
title = "Test what you write"
date = "2025-07-30"
description = "Another day another bug"
tags = [
    "Postgres",
    "Testing"
]
+++

Yesterday , I noticed this weird error in logs.

```
2025-07-30 10:00:00.000 UTC [1] LOG:  operator does not exist: uuid = record at character 50
2025-07-30 10:00:00.000 UTC [1] LOG:  statement: SELECT (orders.id) FROM order WHERE orders = ($1, $2)
```

It was immediately clear this was a bug the query attempts to compare a column to a record, which is not valid syntax in Postgres using the `=` operator.

When I brought it up with the developers and QA responsible for the feature, they insisted it had been tested and was working as expected.

This prompted me to take a closer look. My assumption was that the intent here was to check whether `orders` matched *any* of the values, which would require using the `IN` operator, like:

```
SELECT orders.id FROM order WHERE orders IN ($1, $2)
```

Turns out they didn't expect multiple values and the use case only expected one entry in array , which of course will work fine. But the language in code was something that did expect array. 

What was not clear to me is who is to blame?

Lessons learned:
- If your query supports only one value, enforce it.
- If your query supports multiple values, use the right operator.
- And most importantly: test what you write, especially with different shapes of input.





