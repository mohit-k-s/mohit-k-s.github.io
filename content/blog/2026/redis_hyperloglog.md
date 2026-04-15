+++
title = "How Redis Counts Without Counting"
date = "2026-05-12"
description = "Redis HyperLogLog internals"
category = "tech"
tags = [
    "redis",
    "algorithms",
]
+++

HyperLogLog is one of those ideas that feels fake the first time you hear it:
count unique values without storing the values.

I was looking at how we track DAU at work and realized I had no idea how `PFADD` actually works under the hood. So I read the source.

## The tradeoff

The exact way:

```redis
SADD dau:2026-04-12 user:1 user:2 user:3 ...
SCARD dau:2026-04-12
```

Memory grows with every unique user. Not great when you have millions.

HyperLogLog gives you ~0.81% error in exchange for a fixed ~12KB. For analytics, that's a good deal.

## What actually happens inside PFADD

Redis keeps 16384 tiny registers (buckets). When you add a value:

1. Hash it to 64 bits
2. Top 14 bits pick which bucket
3. Remaining bits — count leading zeros + 1. That's the "rarity score"
4. Keep the max rarity per bucket

```text
|<--- 14 bits --->|<----------- rest ----------->|
  bucket index j       used to compute rarity
```

If the suffix starts with `1...`, rarity is 1. Common.\
If it starts with `000001...`, rarity is 6. Much rarer.

Each bucket only remembers the rarest thing it's ever seen.

## Why leading zeros work as a signal

For random bits, the probability of seeing `k` leading zeros is `1 / 2^k`.

So if your buckets keep seeing higher rarity values, you're probably looking at a larger stream. That's the entire probabilistic trick.

## How PFCOUNT estimates

The formula looks scary but reads simply:

`E = alpha_m * m^2 / sum(2^(-M[j]))`

Each bucket contributes `2^(-M[j])`. Bigger rarity stored means smaller contribution, which pushes the estimate higher.

After the raw estimate, Redis applies small-range and bias corrections. So `PFCOUNT` isn't one formula — it's a pipeline.

## Why PFMERGE is just max

```text
M_dest[j] = max(M_a[j], M_b[j], M_c[j])
```

Each bucket stores "rarest pattern seen so far." Union of streams means keeping the rarest from all inputs. Register-wise max is the right operator.

This is why HLL is so useful in distributed systems. You can merge regional stats without shipping raw user IDs.

```redis
PFADD dau:2026-04-12 user:1 user:2 user:3
PFCOUNT dau:2026-04-12

PFMERGE wau:2026-15 dau:2026-04-06 dau:2026-04-07 ... dau:2026-04-12
PFCOUNT wau:2026-15
```

## Sparse vs dense

Redis starts with a compact sparse encoding at low cardinality and switches to dense (~12KB) when there are enough entries. So memory starts small and stabilizes.

## When to use it

DAU/WAU/MAU, approximate uniques, anything where you care about memory predictability more than exactness.

Don't use it for billing or anything where you need to list actual members later.
