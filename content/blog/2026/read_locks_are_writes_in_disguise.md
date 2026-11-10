+++
title = "Read-Locks Are Writes in Disguise: Achieving 84x Concurrency on Apple M2"
date = "2026-11-10"
description = "Textbooks tell us that read-write locks allow concurrent readers without blocking. On real multi-core hardware, adding cores collapsed throughput by 74%. Here is the physics of why, and how we achieved an 84x speedup with Optimistic Lock Coupling."
category = "tech"
tags = [
    "concurrency",
    "systems",
    "c++",
    "arm64",
    "performance",
    "apple-silicon",
]
+++

Every computer science curriculum teaches the same comfortable truth about concurrency:

> *"Use a mutex when writing. Use a Read-Write Lock (`shared_mutex`, `sync.RWMutex`, or `pthread_rwlock`) when reading, because multiple readers can read concurrently without blocking each other."*

It sounds great on paper.

Yesterday, after optimizing binary search on the Apple M2, I decided to test this textbook wisdom on an in-memory B+ tree. I wrapped the root with a standard `std::shared_mutex`, spawned threads, and ran 10 million pure read queries across M2's 8 physical cores.

Here is what happened:
* On **1 core**: **23.1 million ops/sec**
* On **8 cores**: **6.0 million ops/sec**

Adding 7 more CPU cores made the database **nearly 4x slower**. A 74% throughput collapse on a workload that was doing **100% reads and zero writes**.

How can an algorithm doing zero writes get slower as you add more CPU cores?

Because at the silicon level, **read-locks are writes in disguise**.

---

## 1. What a "Read Lock" Actually Executes

Open the implementation of `pthread_rwlock_rdlock`, C++'s `std::shared_lock`, or Go's `sync.RWMutex.RLock()`.

To allow multiple readers while keeping writers out, the mutex has to keep count of how many readers are currently active.

So when your thread calls `shared_lock()`, the CPU executes:

```c
// Inside pthread_rwlock / std::shared_mutex:
atomic_fetch_add(&rwlock->reader_count, 1);  // <-- THIS IS AN ATOMIC WRITE
```

And when your thread is done reading:

```c
atomic_fetch_sub(&rwlock->reader_count, 1);  // <-- ANOTHER ATOMIC WRITE
```

You thought you were just reading data. In reality, **every single read query performs two atomic writes to shared memory**.

---

## 2. The Crime Scene: Cache Coherence & MESI

Why does writing an atomic integer matter if nobody is modifying the database keys?

Because modern multi-core processors do not share L1 caches. Every core on my Apple M2 has its own private, blazing-fast 64 KB L1 Data Cache.

To ensure all 8 cores see the same view of memory, the CPU hardware runs a cache coherence protocol called **MESI** (Modified, Exclusive, Shared, Invalid).

Here is what physically happens on the CPU bus when 8 cores try to acquire a "read lock":

```text
           ┌──────────────────────────────────────────────┐
           │      rwlock.reader_count (Atomic Integer)    │
           └──────────────────────┬───────────────────────┘
                                  │ Ping-Pong!
          ┌───────────────────────┴───────────────────────┐
          ▼                                               ▼
   ┌─────────────┐                                 ┌─────────────┐
   │ Core 0 (L1) │ ────[ BUS INVALIDATE Core 1 ]─> │ Core 1 (L1) │
   │ State: [M]  │ <───[ BUS INVALIDATE Core 0 ]── │ State: [M]  │
   └─────────────┘                                 └─────────────┘
      Atomic +1                                       Atomic +1
   (Pipeline Stalled)                              (Pipeline Stalled)
```

1. **Core 0** wants to increment `reader_count`. To write to that memory address, Core 0 must acquire **Exclusive / Modified (M)** ownership of that cache line. It broadcasts an `INVALIDATE` signal across the Apple M2 interconnect.
2. **Cores 1 through 7** are forced to evict that cache line from their private L1 caches.
3. One nanosecond later, **Core 1** wants to acquire its read-lock. It suffers an **L1 Cache Miss** because Core 0 just invalidated its line.
4. Core 1 stalls its instruction pipeline, waits for Core 0 to flush the cache line, and broadcasts its own `INVALIDATE` signal back across the crossbar.
5. Repeat across all 8 cores, 20 million times.

This phenomenon is known in database research as **Cache-Line Bouncing** or the **Reader-Lock Scalability Collapse**.

Even though your threads only wanted to read tree nodes, your CPU cores spent 90% of their clock cycles stalled, waiting for ownership of that single 128-byte cache line to ping-pong across the inter-core interconnect.

---

## 3. The Fix: Optimistic Lock Coupling (OLC)

The solution was popularized by the "German database school" (Viktor Leis, Alfons Kemper, and Thomas Neumann at TU Munich) in high-performance engines like HyPer and LeanStore:

> **If you want linear multi-core scaling, readers must write ZERO bytes to memory.**

In **Optimistic Lock Coupling (OLC)**, readers do not acquire any lock. They do not increment any reader counter. They execute with pure speculation:

### The 64-Bit Version Primitive
Every node in the tree stores a single 64-bit integer:

```text
 63                                                         1   0
┌────────────────────────────────────────────────────────────┬───┐
│                    Version Counter                         │ L │
│   (Even number when unlocked, incremented on write_unlock) │ K │
└────────────────────────────────────────────────────────────┴───┘
                                                               ▲
                                         0 = Unlocked, 1 = Locked
```

* **Bit 0 (`LOCKED`)**: 1 = exclusive write lock held, 0 = unlocked.
* **Bits 1..63 (`VERSION`)**: A monotonic version counter incremented on every unlock.

```cpp
class OptLock {
    std::atomic<uint64_t> version_{0};
    static constexpr uint64_t LOCKED_BIT = 0x1ULL;

public:
    // Read Path: ZERO writes to shared memory!
    inline uint64_t read_lock_optimistic() const {
        uint64_t v = version_.load(std::memory_order_acquire); // Compiles to ARM64 LDAR
        while (__builtin_expect((v & LOCKED_BIT) != 0, 0)) {
            asm volatile("yield" ::: "memory");
            v = version_.load(std::memory_order_acquire);
        }
        return v;
    }

    // Validate: Did any writer touch this node while I was reading?
    inline bool validate(uint64_t old_version) const {
        return version_.load(std::memory_order_acquire) == old_version;
    }

    // Write Path: Exclusive lock via CAS on bit 0
    inline void write_lock() {
        uint64_t expected = version_.load(std::memory_order_relaxed);
        while (true) {
            if ((expected & LOCKED_BIT) == 0) {
                if (version_.compare_exchange_weak(expected, expected | LOCKED_BIT,
                                                   std::memory_order_acquire,
                                                   std::memory_order_relaxed)) {
                    return;
                }
            } else {
                asm volatile("yield" ::: "memory");
                expected = version_.load(std::memory_order_relaxed);
            }
        }
    }

    // Unlock: Advance version by 2 and clear locked bit
    inline void write_unlock() {
        uint64_t current = version_.load(std::memory_order_relaxed);
        version_.store(current + 1, std::memory_order_release); // Compiles to ARM64 STLR
    }
};
```

### How an OLC Read Works: Hand-over-Hand Validation

Instead of holding locks from parent to child down the tree, readers validate each hop optimistically:

```text
               [ Parent Node ]  (Read Version: V_parent)
                      │
                      │ 1. Speculative read child pointer
                      ▼
               [ Child Node  ]  (Read Version: V_child)
                      │
                      │ 2. Validate Parent: version == V_parent ?
                      ├──────────────────────────┐
                     YES                         NO
                      │                          │
              3. Parent was intact!       Concurrently modified!
                 Traverse down child         Abort & Restart from Root
```

When traversing down a tree from parent to child:
1. The reader reads `parent->version` using ARM64's single-cycle `LDAR` (Load-Acquire).
2. It finds the child pointer and reads `child->version`.
3. It validates: `parent->validate(parent_version)`.
4. If valid, it moves down. If invalid (a writer split the parent while it was reading), it restarts the search from the root.

Because readers execute only `LDAR` and **never write a single bit**:
* The cache line stays in state **Shared (S)** across all 8 L1 caches.
* Zero invalidate messages travel across the bus.
* All 8 cores read at full hardware bandwidth simultaneously.

---

## 4. The Hidden Trap: Memory Reclamation (EBR)

There is a catch.

If readers do not acquire locks, what happens when a writer splits or deletes a node while a reader is midway through traversing it?

If the writer calls `delete node`, the reader will dereference freed memory and crash with a segmentation fault (Use-After-Free).

To fix this without a heavyweight garbage collector, we use **Epoch-Based Reclamation (EBR)**:

```text
                 ┌───────────────┐
                 │  Epoch [ 0 ]  │
                 └───────┬───────┘
                         │
                 Advance │ (Active Readers Pin Current Epoch)
                         ▼
  ┌───────────────┐              ┌───────────────┐
  │  Epoch [ 2 ]  │ <─────────── │  Epoch [ 1 ]  │
  └───────┬───────┘   Advance    └───────────────┘
          │
          │ Objects retired in Epoch 2 can be freed
          ▼ when ALL active threads have moved past Epoch 2!
     Safe Bulk Free
```

* A global epoch rotates through `0 → 1 → 2 → 0`.
* When a reader thread begins a query, it pins the current epoch:
  `local_epoch.store(global_epoch);`
* When a writer unlinks a node from the tree, it does not `free()` it. It pushes the pointer into an epoch retirement queue:
  `retired_buckets[current_epoch].push_back(node);`
* Once all threads have advanced past `epoch G`, objects retired in `epoch (G + 1) % 3` are mathematically guaranteed to have **zero active readers**. They can be deallocated immediately in bulk without locks.

### The Apple Silicon Rule: 128-Byte Alignment
On Apple Silicon, each thread's epoch tracking struct must be aligned to **128 bytes** (`alignas(128)`):

```cpp
struct alignas(128) ThreadEpochState {
    std::atomic<uint64_t> local_epoch{UNPINNED};
    std::atomic<bool> in_use{false};
    std::array<std::vector<RetiredNode>, 3> retired_buckets;
    size_t pending_count{0};
    char pad[...];
};
```

If you don't align it to 128 bytes, adjacent cores dirty each other's epoch variables, re-introducing false sharing and silently destroying your throughput.

---

## 5. The Benchmark: Real Numbers on Apple M2

I built a concurrent B+ tree using OLC and EBR and benchmarked it against `std::shared_mutex` across all 8 physical cores of the Apple M2.

### Workload 1: 100% Reads (10,000,000 Lookups)

| Threads | `std::shared_mutex` (RWLock) | OLC (Apple Silicon Speculative `LDAR`) | OLC Scaling | OLC Speedup |
| :---: | :---: | :---: | :---: | :---: |
| **1 Core** | 23.1 Mops/sec | 32.4 Mops/sec | 1.00x | **1.40x** |
| **2 Cores** | 12.5 Mops/sec *(collapses)* | 59.7 Mops/sec *(near-linear)* | 1.84x | **4.78x** |
| **4 Cores** | 6.6 Mops/sec *(collapses)* | 116.0 Mops/sec *(linear)* | 3.58x | **17.49x** |
| **8 Cores** | **6.0 Mops/sec** *(collapses)* | **154.5 Mops/sec** | **4.77x** | **25.78x Faster** |

Look at the difference:
* `std::shared_mutex` collapsed by **74%** as core count increased.
* OLC scaled smoothly up to **154.5 million lookups per second** (a **25.8x leap**).

---

### Workload 2: 90% Read / 10% Write (5,000,000 Ops)

In real workloads, writers are constantly modifying the tree. Under standard locking, exclusive writers block all readers, and queued readers starve writers:

| Threads | `std::shared_mutex` (RWLock) | OLC + EBR (Apple Silicon) | Speedup |
| :---: | :---: | :---: | :---: |
| **1 Core** | 22.5 Mops/sec | 31.4 Mops/sec | **1.40x** |
| **2 Cores** | 6.9 Mops/sec | 57.9 Mops/sec | **8.45x** |
| **4 Cores** | 3.4 Mops/sec | 111.4 Mops/sec | **32.58x** |
| **8 Cores** | **1.3 Mops/sec** | **109.0 Mops/sec** | **83.63x Faster (84x Leap!)** |

At 8 threads under mixed read/write contention, `std::shared_mutex` ground to a halt at **1.3 million ops/sec**. 

The OLC + EBR engine maintained over **109 million ops/sec**—an **84x speedup**—while safely retiring and reclaiming tens of thousands of nodes with zero data races and zero memory leaks.

---

## Takeaways for Systems Engineers

1. **Read-locks are writes in disguise**: Any lock that increments an atomic counter turns concurrent reads into a cache-line invalidation storm.
2. **Zero-write reading is the only path to linear scaling**: Readers must use speculative loads (`LDAR`) and version checks instead of writing lock state.
3. **EBR is essential for lock-free memory safety**: You don't need a heavy garbage collector to safely free memory in concurrent systems; a 3-epoch rotation does it with zero reader locks.
4. **Respect 128-byte cache lines**: Always isolate thread-local state with `alignas(128)` on Apple Silicon to prevent adjacent cores from invalidating each other's state.

---

## Reproducing the Benchmark

The complete self-contained C++ benchmark is hosted directly on this site:
* **Source Code**: [`/scripts/olc_ebr_benchmark.cpp`](/scripts/olc_ebr_benchmark.cpp)

You can run it on your own Mac with:

```sh
curl -sO https://mohit-k-s.github.io/scripts/olc_ebr_benchmark.cpp
clang++ -O3 -std=c++20 -mcpu=apple-m2 -pthread olc_ebr_benchmark.cpp -o olc_bench
./olc_bench
```
