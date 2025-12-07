+++
title = "WalJet: Building a High-Performance CDC Pipeline"
date = "2025-12-07"
description = "A deep dive into building a Change Data Capture pipeline from Postgres WAL to Redis Streams using Go"
tags = [
    "golang",
    "postgres",
    "cdc",
    "distributed-systems",
    "redis",
]
+++

Change Data Capture (CDC) sounds simple on paper: take database changes, stream them somewhere else. How hard could it be?

Very hard.

Here's what I hacked away when i was bored at work.

## The Problem

Modern applications often need to react to database changes in real-time. Whether it's updating a cache, triggering workflows, or syncing data across services, you need a reliable way to capture and stream database changes.

Postgres has a powerful feature called Write-Ahead Logging (WAL) that records every change before it's applied. The challenge is turning that low-level stream into something useful.

## Architecture

WalJet's architecture is straightforward but carefully designed:

<img src="../images/waljet_arch.png" alt="Waljet architecture" width="600"/>

**Key components:**

1. **WAL Listener**: Connects to Postgres using logical replication protocol (`pgoutput`)
2. **Decoder**: Parses binary WAL messages into structured change events
3. **Encoder Pool**: Parallel workers that serialize events to Protobuf
4. **Publisher Pool**: Batches and publishes events to Redis Streams
5. **Checkpoint Manager**: Tracks replication progress with dual storage (Redis + file)

## Why This Stack?

**Postgres WAL + pglogrepl**: Direct access to the write-ahead log using Go's `pglogrepl` library. No JVM overhead, no Kafka Connect complexity.

**Protobuf encoding**: Smaller payloads, type safety, and faster serialization than JSON. When you're processing thousands of events per second, this matters.

**Redis Streams**: Ordered delivery, consumer groups, and sub-millisecond latency. Perfect for real-time event streaming without the operational overhead of Kafka.

The entire pipeline is designed around one goal: get database changes to downstream consumers as fast as possible, with minimal resource usage.

At no point this is a debezium replacer , debezium is a much more complex system, waljet is just a simple hack to get things done.


## The Tricky Parts

### Graceful Shutdown

This one caught me off guard. You can't just kill a CDC process—you'll leak replication slots and lose checkpoint data. 

```
1. Stop accepting new events
2. Close event channels
3. Wait for workers to finish (flush batches)
4. Save final checkpoint (Redis + file)
5. Close replication connection
6. Open regular connection
7. Drop replication slot
8. Close regular connection
9. Close Redis connection
```

The gotcha? You can't drop a replication slot while you're still using the replication connection. You need to:
1. Close the replication connection
2. Open a regular Postgres connection
3. Drop the slot
4. Close the regular connection

Miss this, and you leak storage in Postgres. The WAL files pile up, and eventually, you run out of disk space.

### Checkpoint Durability

When WalJet crashes (and it will), where do you resume? You need to track the last processed Log Sequence Number (LSN).

I went with a dual checkpoint system:
- **Primary**: Redis key (`waljet:checkpoint:waljet_slot`)
- **Backup**: Local file (`.waljet_checkpoint_waljet_slot`)

Why both? If Redis restarts, you still have the local file. If the container dies, Redis has the checkpoint. On startup, WalJet checks both and uses whichever is more recent.

Simple, but it works.

## Why Build This?

The idea was simple: what if you could react to database changes in real-time without the complexity of Debezium or Kafka?

WalJet is designed for use cases where you need fast CDC with minimal infrastructure:
- **Cache invalidation**: Update Redis/Memcached the moment data changes
- **Webhook triggers**: Call external APIs when specific rows are modified
- **Event-driven architectures**: Stream changes to downstream services

It's not trying to replace Kafka or Debezium for large-scale data pipelines. It's for when you need something lightweight, fast, and easy to deploy.

## Benchmark

I ran a 5-minute benchmark comparing WalJet with Debezium under identical conditions:

| Metric | Debezium | WalJet | Improvement |
|--------|----------|--------|-------------|
| **Events/sec** | 92.32 | 95.07 | +3% |
| **P50 Latency** | 541.44ms | 23.44ms | **23x faster** |
| **P95 Latency** | 871.11ms | 50.16ms | **17x faster** |
| **P99 Latency** | 952.98ms | 53.15ms | **18x faster** |
| **Avg Latency** | 541.08ms | 25.17ms | **21x faster** |
| **Max Latency** | 1131.87ms | 171.33ms | **6.6x faster** |
| **Events Lost** | 0 | 0 | ✓ |

The throughput is similar, but latency tells the real story. WalJet's P99 latency is 53ms compared to Debezium's 953ms—nearly 20x faster. 

> "When you're invalidating a cache or triggering a webhook, waiting 950ms vs 50ms is the difference between a system that feels instant and one that feels sluggish."
> 
> — Me, after running this benchmark

Remember [*The Hummingbird Project*](https://letterboxd.com/film/the-hummingbird-project/)? The movie where they're trying to build a fiber optic cable in a straight line from Kansas to New Jersey just to shave off a few milliseconds for high-frequency trading. Every millisecond was worth millions.

WalJet isn't for HFT, but the principle is the same: in systems where you need to react to changes immediately like cache invalidation, real-time webhooks, event-driven architectures—latency isn't just a nice-to-have. It's the whole point.

## Why Not Open Source (Yet)?

WalJet started as a fun experiment—something I hacked for fun,  It works, the benchmarks are promising, but it's not production-ready in the way an OSS project should be.

There's still work to do:
- **More performance testing**: The benchmark above is just one scenario. Need to test under different loads, failure modes, and edge cases
- **Better error handling**: Right now it works for the happy path, but production systems need to handle every unhappy path too
- **Documentation**: The code needs better docs, examples, and a proper getting-started guide
- **Battle testing**: It hasn't been run in production long enough to discover all the weird bugs

Maybe it'll be open source someday. For now, it's a proof of concept that taught me a lot about Postgres internals and CDC pipelines. And honestly? That was the whole point.
