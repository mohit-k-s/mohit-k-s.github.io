#include <arm_neon.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <random>
#include <iostream>
#include <iomanip>

class NanoTimer {
    std::chrono::high_resolution_clock::time_point start_time;
public:
    NanoTimer() { reset(); }
    void reset() { start_time = std::chrono::high_resolution_clock::now(); }
    double elapsed_ns() const {
        auto end_time = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::nano>(end_time - start_time).count();
    }
};

// 1. Standard Branching Binary Search
__attribute__((noinline))
int binary_search_branching(const uint32_t* arr, int n, uint32_t target) {
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

// 2. Branchless Binary Search on Sorted Array (Shar's / Clang CMOV)
__attribute__((noinline))
int binary_search_branchless(const uint32_t* arr, int n, uint32_t target) {
    const uint32_t* base = arr;
    int len = n;
    while (len > 1) {
        int half = len / 2;
        // Compiles to CSEL on ARM64
        base = (base[half] < target) ? base + half : base;
        len -= half;
    }
    return (*base < target) ? (base - arr) + 1 : (base - arr);
}

// 3. Eytzinger Heap Layout Constructor
void build_eytzinger(const std::vector<uint32_t>& sorted, std::vector<uint32_t>& eytzinger, int k = 1, int* in_idx = nullptr) {
    static int idx = 0;
    if (in_idx == nullptr) {
        idx = 0;
        in_idx = &idx;
    }
    if (k <= static_cast<int>(sorted.size())) {
        build_eytzinger(sorted, eytzinger, 2 * k, in_idx);
        eytzinger[k] = sorted[(*in_idx)++];
        build_eytzinger(sorted, eytzinger, 2 * k + 1, in_idx);
    }
}

// 4. Eytzinger Branchless Search
__attribute__((noinline))
int search_eytzinger_branchless(const uint32_t* arr, int n, uint32_t target) {
    int k = 1;
    while (k <= n) {
        // Generates branchless CSEL on Apple Silicon
        k = 2 * k + (target >= arr[k] ? 1 : 0);
    }
    return k >> (__builtin_ffs(~k));
}

// 5. Eytzinger Branchless with Software Prefetching
__attribute__((noinline))
int search_eytzinger_prefetch(const uint32_t* arr, int n, uint32_t target) {
    int k = 1;
    while (k <= n) {
        // Prefetch child nodes ahead in the heap (e.g. 4 levels down: 2^4 = 16*k)
        __builtin_prefetch(&arr[k * 16], 0, 0);
        k = 2 * k + (target >= arr[k] ? 1 : 0);
    }
    return k >> (__builtin_ffs(~k));
}

int main() {
    std::cout << "========================================================================================\n";
    std::cout << "               BINARY SEARCH ON APPLE M2: SCALING & OPTIMIZATION\n";
    std::cout << "========================================================================================\n\n";

    std::vector<int> sizes = {1024, 16384, 65536, 1048576, 8388608};
    constexpr int NUM_QUERIES = 2000000;

    std::mt19937 rng(42);
    std::vector<uint32_t> queries(NUM_QUERIES);

    std::cout << std::setw(12) << "Array Size"
              << std::setw(10) << "RAM Size"
              << std::setw(16) << "std::lower_bd"
              << std::setw(16) << "Branchless Sort"
              << std::setw(16) << "Eytzinger"
              << std::setw(16) << "Eytzinger+Pref"
              << std::setw(12) << "Best Gain" << "\n";
    std::cout << "----------------------------------------------------------------------------------------\n";

    for (int n : sizes) {
        std::vector<uint32_t> sorted(n);
        for (int i = 0; i < n; ++i) sorted[i] = i * 4;

        std::vector<uint32_t> eytzinger(n + 1, 0);
        build_eytzinger(sorted, eytzinger);

        for (int i = 0; i < NUM_QUERIES; ++i) queries[i] = rng() % (n * 4);

        // 1. std::lower_bound
        double ns_std = 0;
        {
            uint64_t sum = 0;
            NanoTimer t;
            for (int i = 0; i < NUM_QUERIES; ++i) {
                auto it = std::lower_bound(sorted.begin(), sorted.end(), queries[i]);
                sum += (it - sorted.begin());
            }
            double ns = t.elapsed_ns();
            asm volatile("" : : "r"(sum) : "memory");
            ns_std = ns / NUM_QUERIES;
        }

        // 2. Branchless Sorted
        double ns_sort_bl = 0;
        {
            uint64_t sum = 0;
            const uint32_t* arr = sorted.data();
            NanoTimer t;
            for (int i = 0; i < NUM_QUERIES; ++i) {
                sum += binary_search_branchless(arr, n, queries[i]);
            }
            double ns = t.elapsed_ns();
            asm volatile("" : : "r"(sum) : "memory");
            ns_sort_bl = ns / NUM_QUERIES;
        }

        // 3. Eytzinger Branchless
        double ns_eyt = 0;
        {
            uint64_t sum = 0;
            const uint32_t* arr = eytzinger.data();
            NanoTimer t;
            for (int i = 0; i < NUM_QUERIES; ++i) {
                sum += search_eytzinger_branchless(arr, n, queries[i]);
            }
            double ns = t.elapsed_ns();
            asm volatile("" : : "r"(sum) : "memory");
            ns_eyt = ns / NUM_QUERIES;
        }

        // 4. Eytzinger + Prefetch
        double ns_pref = 0;
        {
            uint64_t sum = 0;
            const uint32_t* arr = eytzinger.data();
            NanoTimer t;
            for (int i = 0; i < NUM_QUERIES; ++i) {
                sum += search_eytzinger_prefetch(arr, n, queries[i]);
            }
            double ns = t.elapsed_ns();
            asm volatile("" : : "r"(sum) : "memory");
            ns_pref = ns / NUM_QUERIES;
        }

        double best = std::min({ns_sort_bl, ns_eyt, ns_pref});
        double gain = (ns_std / best);

        std::string mem_str;
        if (n * 4 < 1024 * 1024) mem_str = std::to_string(n * 4 / 1024) + " KB";
        else mem_str = std::to_string(n * 4 / (1024 * 1024)) + " MB";

        std::cout << std::setw(12) << n
                  << std::setw(10) << mem_str
                  << std::setw(14) << std::fixed << std::setprecision(2) << ns_std << " ns"
                  << std::setw(14) << ns_sort_bl << " ns"
                  << std::setw(14) << ns_eyt << " ns"
                  << std::setw(14) << ns_pref << " ns"
                  << std::setw(10) << std::setprecision(1) << gain << "x\n";
    }

    std::cout << "========================================================================================\n";
    return 0;
}
