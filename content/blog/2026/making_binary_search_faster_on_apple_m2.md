+++
title = "Making Binary Search Faster on Apple M2"
date = "2026-11-09"
description = "Binary search is O(log N) and standard library code like std::lower_bound is heavily optimized. Can we make it nearly 3x faster on Apple Silicon? Here is what happens when you dismantle branch mispredictions and cache lines."
category = "tech"
tags = [
    "performance",
    "arm64",
    "systems",
    "c++",
    "apple-silicon",
]
+++

Binary search is one of the first algorithms every programmer learns. It is O(log N), elegant, and standard libraries like C++'s `std::lower_bound` have had decades to optimize it.

If you have a sorted array and need to find an element, you call `std::lower_bound` and move on.

A few days ago, I started wondering: on modern superscalar CPUs—specifically Apple Silicon (M2)—is `std::lower_bound` actually running as fast as the hardware allows?

Or is it leaving performance on the table?

To find out, I wrote a microbenchmark suite and tested several search implementations across array sizes ranging from 1,024 elements (4 KB, fits in L1 cache) to 8,388,608 elements (32 MB, spills into DRAM).

It turns out you can beat `std::lower_bound` by **up to 2.9x**.

Here is how, and why the hardware makes it possible.

---

## The Hardware Reality: What Happens on Apple M2

To understand why traditional binary search struggles, you have to look at the CPU executing it.

My test machine is an M2 MacBook Pro:
* **8 CPU cores** (4 Avalanche performance cores + 4 Blizzard efficiency cores)
* **64 KB L1 Data Cache** per performance core
* **4 MB L2 Cache**
* **128-byte cache line size** (`hw.cachelinesize = 128`)

Apple's performance cores are beasts. They have an **8-wide instruction decode engine** and a massive reorder buffer (~600+ instructions). In an ideal scenario, the CPU executes several instructions per clock cycle.

But binary search exposes two major weaknesses in modern CPU architectures:

1. **The Branch Misprediction Penalty**:
   When you search for random keys, the comparison `arr[mid] < target` is true 50% of the time and false 50% of the time. It is mathematically the worst-case scenario for branch predictors. Every misprediction flushes the execution pipeline, costing 15 to 20 clock cycles.
2. **The Cache-Locality Penalty**:
   In a traditional sorted array, the first step checks index N/2. The next step jumps to N/4 or 3N/4 (megabytes away). The next step jumps hundreds of kilobytes away. Every step jumps to an unpredictable cache line.

---

## Step 1: The Baseline (`std::lower_bound`)

Here is the standard binary search approach:

```cpp
template <typename T>
int binary_search_branching(const T* arr, int n, T target) {
    int low = 0, high = n - 1;
    while (low <= high) {
        int mid = low + (high - low) / 2;
        if (arr[mid] < target) {
            low = mid + 1;
        } else if (arr[mid] > target) {
            high = mid - 1;
        } else {
            return mid;
        }
    }
    return low;
}
```

On a 65,536-element array (256 KB), `std::lower_bound` takes **32.26 ns** per lookup.

For an algorithm that only does ~16 iterations (log₂(65536) = 16), 32 nanoseconds means every iteration is costing around 2 nanoseconds (~7 to 8 CPU clock cycles). The CPU is spending most of its time stalling on mispredicted branches.

---

## Step 2: Branchless Binary Search on Sorted Arrays

Can we eliminate the `if-else` branches entirely?

Yes. Instead of branching on the comparison, we use conditional pointer arithmetic (often called Shar's algorithm or branchless binary search):

```cpp
int binary_search_branchless(const uint32_t* arr, int n, uint32_t target) {
    const uint32_t* base = arr;
    int len = n;
    while (len > 1) {
        int half = len / 2;
        // On ARM64, this compiles to CSEL (Conditional Select)
        base = (base[half] < target) ? base + half : base;
        len -= half;
    }
    return (*base < target) ? (base - arr) + 1 : (base - arr);
}
```

Notice what changed: there are no branches inside the `while` loop.

When compiled with `clang++ -O3 -mcpu=apple-m2`, Clang emits ARM64's `CSEL` (Conditional Select) instruction. `CSEL` evaluates in **1 clock cycle** regardless of whether the condition was true or false. Zero pipeline flushes.

### The Result
* At N = 1,024 (4 KB): drops from **19.33 ns → 7.71 ns (2.5x faster)**.
* At N = 65,536 (256 KB): drops from **32.26 ns → 18.49 ns (1.7x faster)**.

By simply removing the branch, we doubled the speed. But when the array grows larger than L2 cache, another bottleneck takes over: memory access patterns.

---

## Step 3: The Eytzinger (Breadth-First Heap) Layout

In a sorted array, the memory addresses accessed during binary search are scattered:

```
Sorted:   [ 10,  20,  30,  40,  50,  60,  70 ]
Index:       0    1    2    3    4    5    6
Step 1:                     ^ (mid)
Step 2:          ^ (or)               ^
```

Each step jumps across huge memory strides.

In the 1970s, Michael Eytzinger proposed a layout for binary trees that is identical to a binary heap:
* The root is stored at **index 1**.
* The left child of node k is stored at **2k**.
* The right child of node k is stored at **2k + 1**.

```
Sorted Array:      [ 10,  20,  30,  40,  50,  60,  70 ]
Eytzinger Layout:  [ 40,  20,  60,  10,  30,  50,  70 ]
Index:                1    2    3    4    5    6    7
```

### Why is this brilliant for CPU caches?

Look at where the most frequently accessed nodes live:
* Index 1 (the root) is visited on **every single search**.
* Indices 2 and 3 (level 1) are visited on **every single search**.
* Indices 4, 5, 6, 7 (level 2) are visited on **every single search**.

Indices 1 through 15 take 15 × 4 = 60 bytes.

On Apple Silicon, a single cache line is **128 bytes**.

That means the entire top 4 levels of the tree fit inside **one single cache line**! Once fetched, it stays permanently hot in the L1 Data Cache. The first 4 steps of any search will never incur a cache miss.

### The Search Implementation

The traversal loop is astonishingly simple:

```cpp
int search_eytzinger_branchless(const uint32_t* arr, int n, uint32_t target) {
    int k = 1;
    while (k <= n) {
        // Pure conditional arithmetic: compiles to 1-cycle CSEL on ARM64
        k = 2 * k + (target >= arr[k] ? 1 : 0);
    }
    // Restore the index
    return k >> (__builtin_ffs(~k));
}
```

Every single iteration is two operations:
1. Compare target with `arr[k]`
2. Update `k = 2 * k + condition`

Zero branches. Zero branch mispredictions. Perfect cache locality at the top of the tree.

---

## The Benchmark: Across Cache Boundaries

I ran 2,000,000 random lookups across five array sizes on the Apple M2:

| Elements | Memory Size | Cache Level | `std::lower_bound` | Branchless Sorted | Eytzinger Heap | Speedup |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **1,024** | 4 KB | L1 Cache (64 KB) | 19.33 ns | 7.71 ns | **7.19 ns** | **2.7x faster** |
| **16,384** | 64 KB | L1 Boundary | 17.37 ns | **9.66 ns** | 11.20 ns | **1.8x faster** |
| **65,536** | 256 KB | L2 Cache (4 MB) | 32.26 ns | 18.49 ns | **14.57 ns** | **2.2x faster** |
| **1,048,576** | 4 MB | L2 Boundary | 58.97 ns | 38.84 ns | **27.69 ns** | **2.1x faster** |
| **8,388,608** | 32 MB | DRAM (Main Memory) | 240.60 ns | 158.91 ns | **84.27 ns** | **2.9x faster** |

---

## Why Eytzinger Dominates at 32 MB

Look at the 32 MB result:
* `std::lower_bound`: **240.60 ns**
* Eytzinger: **84.27 ns (nearly 3x faster)**

When an array is 32 MB, standard binary search incurs cold DRAM cache misses right from the beginning because the root and first few subdivisions are megabytes apart.

With Eytzinger, the first 6 to 8 levels of the search are served directly out of L1 and L2 cache. The search only has to hit DRAM for the very last leaf accesses. Combined with zero branch mispredictions, latency drops from 240 ns down to 84 ns.

---

## What About Software Prefetching?

In academic literature (e.g. Paul-Virak Khuong and Pat Morin's 2017 paper *"Array Layouts for Comparison-Based Searching"*), they recommend adding software prefetch instructions to Eytzinger search:

```cpp
__builtin_prefetch(&arr[k * 16]); // Prefetch 4 levels ahead
```

I tested this on the M2:

```
At 65,536 elements:
Without prefetch:  14.57 ns
With prefetch:     67.62 ns  (4.6x SLOWER!)
```

Why did software prefetching make it worse?

Apple Silicon's Avalanche cores have an aggressive hardware prefetcher. When you inject explicit `PRFM` (prefetch) instructions into a tight 1-cycle loop, you clog the instruction decode ports and instruction queue. For in-memory data structures on Apple Silicon, **letting the hardware prefetcher do its job beats manual software prefetching**.

---

## Summary of Takeaways

1. **`std::lower_bound` is branch-bound**: Random binary searches on sorted arrays trigger ~50% branch mispredictions, stalling wide out-of-order CPUs.
2. **`CSEL` is your friend**: On ARM64, branchless conditional arithmetic executes in 1 cycle without pipeline flushes.
3. **Data layout is an optimization**: Sorted order is human-friendly; Eytzinger breadth-first layout is cache-friendly.
4. **Hardware boundaries dictate wins**:
   * For arrays fitting in L1 cache (N ≤ 1,024), branchless sorted search is fastest (7.1 ns).
   * For arrays exceeding L2 cache (N ≥ 1,000,000), Eytzinger layout wins by almost 3x because the tree root and top layers never leave CPU cache.

## Reproducing the Benchmark

The full C++ benchmark source code and runner script are available directly on this site:
* **Benchmark Source**: [`/scripts/binary_search_benchmark.cpp`](/scripts/binary_search_benchmark.cpp)
* **Runner Script**: [`/scripts/run_binary_search_benchmark.sh`](/scripts/run_binary_search_benchmark.sh)

You can reproduce the exact numbers on your Mac with:

```sh
curl -sO https://mohit-k-s.github.io/scripts/binary_search_benchmark.cpp
clang++ -O3 -std=c++20 -mcpu=apple-m2 binary_search_benchmark.cpp -o bs_bench
./bs_bench
```
