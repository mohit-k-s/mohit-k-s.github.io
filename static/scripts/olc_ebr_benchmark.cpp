#include <arm_neon.h>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>
#include <array>
#include <algorithm>
#include <shared_mutex>
#include <mutex>
#include <cstring>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <random>
#include <cassert>

#if defined(__aarch64__) || defined(__arm64__)
#define CPU_RELAX() asm volatile("yield" ::: "memory")
#else
#define CPU_RELAX() std::this_thread::yield()
#endif

// ============================================================================
// 1. OPTIMISTIC LOCK COUPLING (OLC) PRIMITIVE
// ============================================================================
class OptLock {
    std::atomic<uint64_t> version_{0};
    static constexpr uint64_t LOCKED_BIT = 0x1ULL;

public:
    OptLock() = default;
    OptLock(const OptLock&) = delete;
    OptLock& operator=(const OptLock&) = delete;

    inline uint64_t read_lock_optimistic() const {
        uint64_t v = version_.load(std::memory_order_acquire);
        while (__builtin_expect((v & LOCKED_BIT) != 0, 0)) {
            CPU_RELAX();
            v = version_.load(std::memory_order_acquire);
        }
        return v;
    }

    inline bool validate(uint64_t old_version) const {
        return version_.load(std::memory_order_acquire) == old_version;
    }

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
                CPU_RELAX();
                expected = version_.load(std::memory_order_relaxed);
            }
        }
    }

    inline void write_unlock() {
        uint64_t current = version_.load(std::memory_order_relaxed);
        version_.store(current + 1, std::memory_order_release);
    }

    inline void write_unlock_abort() {
        uint64_t current = version_.load(std::memory_order_relaxed);
        version_.store(current - 1, std::memory_order_release);
    }
};

// ============================================================================
// 2. EPOCH-BASED MEMORY RECLAMATION (EBR)
// ============================================================================
namespace ebr {
constexpr uint64_t UNPINNED = UINT64_MAX;
constexpr size_t NUM_EPOCHS = 3;
constexpr size_t BATCH_THRESHOLD = 64;
constexpr size_t MAX_THREADS = 64;

struct RetiredNode {
    void* ptr;
    void (*deleter)(void*);
};

struct alignas(128) ThreadEpochState {
    std::atomic<uint64_t> local_epoch{UNPINNED};
    std::atomic<bool> in_use{false};
    std::array<std::vector<RetiredNode>, NUM_EPOCHS> retired_buckets;
    size_t pending_count{0};
    char pad[128 - (sizeof(std::atomic<uint64_t>) + sizeof(std::atomic<bool>) +
                    sizeof(std::array<std::vector<RetiredNode>, NUM_EPOCHS>) + sizeof(size_t)) % 128];
};

class EpochManager {
    std::atomic<uint64_t> global_epoch_{0};
    std::array<ThreadEpochState, MAX_THREADS> threads_;

    EpochManager() {
        for (auto& t : threads_) {
            t.local_epoch.store(UNPINNED, std::memory_order_relaxed);
            t.in_use.store(false, std::memory_order_relaxed);
        }
    }

public:
    static EpochManager& instance() {
        static EpochManager mgr;
        return mgr;
    }

    int register_thread() {
        for (size_t i = 0; i < MAX_THREADS; ++i) {
            bool expected = false;
            if (threads_[i].in_use.compare_exchange_strong(expected, true,
                                                           std::memory_order_acquire,
                                                           std::memory_order_relaxed)) {
                return static_cast<int>(i);
            }
        }
        std::abort();
    }

    void enter(int tid) {
        uint64_t cur = global_epoch_.load(std::memory_order_relaxed);
        threads_[tid].local_epoch.store(cur, std::memory_order_release);
    }

    void exit(int tid) {
        threads_[tid].local_epoch.store(UNPINNED, std::memory_order_release);
    }

    template <typename T>
    void retire(int tid, T* ptr) {
        if (!ptr) return;
        auto del = [](void* p) { delete static_cast<T*>(p); };
        uint64_t ep = global_epoch_.load(std::memory_order_relaxed);
        ThreadEpochState& s = threads_[tid];
        s.retired_buckets[ep % NUM_EPOCHS].push_back({ptr, del});
        s.pending_count++;
        if (s.pending_count >= BATCH_THRESHOLD) {
            try_advance_and_reclaim(tid);
        }
    }

    void try_advance_and_reclaim(int tid) {
        uint64_t cur = global_epoch_.load(std::memory_order_relaxed);
        bool can_advance = true;
        for (size_t i = 0; i < MAX_THREADS; ++i) {
            if (threads_[i].in_use.load(std::memory_order_relaxed)) {
                uint64_t ep = threads_[i].local_epoch.load(std::memory_order_acquire);
                if (ep != UNPINNED && ep < cur) {
                    can_advance = false;
                    break;
                }
            }
        }
        if (can_advance) {
            global_epoch_.compare_exchange_strong(cur, cur + 1,
                                                 std::memory_order_release,
                                                 std::memory_order_relaxed);
        }
        uint64_t new_ep = global_epoch_.load(std::memory_order_relaxed);
        size_t safe_bucket = (new_ep + 1) % NUM_EPOCHS;
        ThreadEpochState& s = threads_[tid];
        auto& b = s.retired_buckets[safe_bucket];
        if (!b.empty()) {
            for (auto& item : b) {
                if (item.ptr && item.deleter) item.deleter(item.ptr);
            }
            s.pending_count -= b.size();
            b.clear();
        }
    }
};

inline int get_tid() {
    thread_local int tid = -1;
    if (__builtin_expect(tid == -1, 0)) {
        tid = EpochManager::instance().register_thread();
    }
    return tid;
}

class EpochGuard {
    int tid_;
public:
    EpochGuard() : tid_(get_tid()) { EpochManager::instance().enter(tid_); }
    ~EpochGuard() { EpochManager::instance().exit(tid_); }
    int tid() const { return tid_; }
};
} // namespace ebr

// ============================================================================
// 3. CONCURRENT B+ TREE WITH OLC
// ============================================================================
namespace olc_tree {
constexpr int B_TREE_ORDER = 16;

template <typename Key, typename Value>
struct BNode {
    OptLock lock;
    bool is_leaf;
    int num_keys;
    Key keys[B_TREE_ORDER];
    Value values[B_TREE_ORDER];
    BNode* children[B_TREE_ORDER + 1];

    BNode(bool leaf) : is_leaf(leaf), num_keys(0) {
        std::memset(keys, 0, sizeof(keys));
        std::memset(values, 0, sizeof(values));
        std::memset(children, 0, sizeof(children));
    }

    inline int find_child_slot(const Key& k) const {
        int low = 0, high = num_keys;
        while (low < high) {
            int mid = low + (high - low) / 2;
            if (keys[mid] <= k) low = mid + 1;
            else high = mid;
        }
        return low;
    }

    inline int find_key_slot(const Key& k) const {
        int low = 0, high = num_keys;
        while (low < high) {
            int mid = low + (high - low) / 2;
            if (keys[mid] < k) low = mid + 1;
            else high = mid;
        }
        return low;
    }
};

template <typename Key, typename Value>
class ConcurrentOlcTree {
    using Node = BNode<Key, Value>;
    Node* root_;
    OptLock root_lock_;
    std::mutex split_mutex_;

public:
    ConcurrentOlcTree() : root_(new Node(true)) {}
    ~ConcurrentOlcTree() { destroy(root_); }

    bool get(const Key& k, Value& out_val) {
        ebr::EpochGuard guard;
    restart:
        uint64_t v_root = root_lock_.read_lock_optimistic();
        Node* curr = root_;
        if (!root_lock_.validate(v_root)) goto restart;

        uint64_t v_curr = curr->lock.read_lock_optimistic();
        if (!root_lock_.validate(v_root)) goto restart;

        while (!curr->is_leaf) {
            int slot = curr->find_child_slot(k);
            Node* child = curr->children[slot];
            uint64_t v_child = child->lock.read_lock_optimistic();
            if (!curr->lock.validate(v_curr)) goto restart;
            curr = child;
            v_curr = v_child;
        }

        int slot = curr->find_key_slot(k);
        bool found = false;
        if (slot < curr->num_keys && curr->keys[slot] == k) {
            out_val = curr->values[slot];
            found = true;
        }
        if (!curr->lock.validate(v_curr)) goto restart;
        return found;
    }

    void insert(const Key& k, const Value& v) {
        ebr::EpochGuard guard;
    restart:
        uint64_t v_root = root_lock_.read_lock_optimistic();
        Node* curr = root_;
        if (!root_lock_.validate(v_root)) goto restart;

        uint64_t v_curr = curr->lock.read_lock_optimistic();
        if (!root_lock_.validate(v_root)) goto restart;

        while (!curr->is_leaf) {
            int slot = curr->find_child_slot(k);
            Node* child = curr->children[slot];
            uint64_t v_child = child->lock.read_lock_optimistic();
            if (!curr->lock.validate(v_curr)) goto restart;
            curr = child;
            v_curr = v_child;
        }

        curr->lock.write_lock();
        int slot = curr->find_key_slot(k);
        if (slot < curr->num_keys && curr->keys[slot] == k) {
            curr->values[slot] = v;
            curr->lock.write_unlock();
            return;
        }

        if (curr->num_keys < B_TREE_ORDER - 1) {
            for (int i = curr->num_keys; i > slot; --i) {
                curr->keys[i] = curr->keys[i - 1];
                curr->values[i] = curr->values[i - 1];
            }
            curr->keys[slot] = k;
            curr->values[slot] = v;
            curr->num_keys++;
            curr->lock.write_unlock();
            return;
        }

        curr->lock.write_unlock_abort();
        insert_with_split(k, v);
    }

private:
    void insert_with_split(const Key& k, const Value& v) {
        std::lock_guard<std::mutex> split_lock(split_mutex_);
        if (root_->num_keys == B_TREE_ORDER - 1) {
            root_lock_.write_lock();
            Node* new_root = new Node(false);
            new_root->children[0] = root_;
            split_child(new_root, 0, root_);
            root_ = new_root;
            root_lock_.write_unlock();
        }
        insert_non_full(root_, k, v);
    }

    void insert_non_full(Node* node, const Key& k, const Value& v) {
        node->lock.write_lock();
        if (node->is_leaf) {
            int slot = node->find_key_slot(k);
            if (slot < node->num_keys && node->keys[slot] == k) {
                node->values[slot] = v;
                node->lock.write_unlock();
                return;
            }
            for (int i = node->num_keys; i > slot; --i) {
                node->keys[i] = node->keys[i - 1];
                node->values[i] = node->values[i - 1];
            }
            node->keys[slot] = k;
            node->values[slot] = v;
            node->num_keys++;
            node->lock.write_unlock();
            return;
        }

        int slot = node->find_child_slot(k);
        Node* child = node->children[slot];
        if (child->num_keys == B_TREE_ORDER - 1) {
            split_child(node, slot, child);
            if (k >= node->keys[slot]) child = node->children[slot + 1];
        }
        node->lock.write_unlock();
        insert_non_full(child, k, v);
    }

    void split_child(Node* parent, int index, Node* child) {
        child->lock.write_lock();
        Node* sibling = new Node(child->is_leaf);
        int mid = (B_TREE_ORDER - 1) / 2;

        if (child->is_leaf) {
            sibling->num_keys = child->num_keys - mid;
            for (int j = 0; j < sibling->num_keys; ++j) {
                sibling->keys[j] = child->keys[mid + j];
                sibling->values[j] = child->values[mid + j];
            }
            child->num_keys = mid;
            for (int j = parent->num_keys; j > index; --j) {
                parent->keys[j] = parent->keys[j - 1];
                parent->children[j + 1] = parent->children[j];
            }
            parent->keys[index] = sibling->keys[0];
            parent->children[index + 1] = sibling;
            parent->num_keys++;
        } else {
            sibling->num_keys = child->num_keys - mid - 1;
            for (int j = 0; j < sibling->num_keys; ++j) sibling->keys[j] = child->keys[mid + 1 + j];
            for (int j = 0; j <= sibling->num_keys; ++j) sibling->children[j] = child->children[mid + 1 + j];
            Key promoted = child->keys[mid];
            child->num_keys = mid;
            for (int j = parent->num_keys; j > index; --j) {
                parent->keys[j] = parent->keys[j - 1];
                parent->children[j + 1] = parent->children[j];
            }
            parent->keys[index] = promoted;
            parent->children[index + 1] = sibling;
            parent->num_keys++;
        }
        child->lock.write_unlock();
    }

    void destroy(Node* n) {
        if (!n) return;
        if (!n->is_leaf) {
            for (int i = 0; i <= n->num_keys; ++i) destroy(n->children[i]);
        }
        delete n;
    }
};

template <typename Key, typename Value>
class CoarseLockedTree {
    using Node = BNode<Key, Value>;
    Node* root_;
    mutable std::shared_mutex rw_lock_;

public:
    CoarseLockedTree() : root_(new Node(true)) {}
    ~CoarseLockedTree() { destroy(root_); }

    bool get(const Key& k, Value& out_val) const {
        std::shared_lock<std::shared_mutex> lock(rw_lock_);
        Node* curr = root_;
        while (!curr->is_leaf) {
            int slot = curr->find_child_slot(k);
            curr = curr->children[slot];
        }
        int slot = curr->find_key_slot(k);
        if (slot < curr->num_keys && curr->keys[slot] == k) {
            out_val = curr->values[slot];
            return true;
        }
        return false;
    }

    void insert(const Key& k, const Value& v) {
        std::unique_lock<std::shared_mutex> lock(rw_lock_);
        if (root_->num_keys == B_TREE_ORDER - 1) {
            Node* new_root = new Node(false);
            new_root->children[0] = root_;
            split_child(new_root, 0, root_);
            root_ = new_root;
        }
        insert_non_full(root_, k, v);
    }

private:
    void insert_non_full(Node* node, const Key& k, const Value& v) {
        if (node->is_leaf) {
            int slot = node->find_key_slot(k);
            if (slot < node->num_keys && node->keys[slot] == k) {
                node->values[slot] = v;
                return;
            }
            for (int i = node->num_keys; i > slot; --i) {
                node->keys[i] = node->keys[i - 1];
                node->values[i] = node->values[i - 1];
            }
            node->keys[slot] = k;
            node->values[slot] = v;
            node->num_keys++;
            return;
        }
        int slot = node->find_child_slot(k);
        Node* child = node->children[slot];
        if (child->num_keys == B_TREE_ORDER - 1) {
            split_child(node, slot, child);
            if (k >= node->keys[slot]) child = node->children[slot + 1];
        }
        insert_non_full(child, k, v);
    }

    void split_child(Node* parent, int index, Node* child) {
        Node* sibling = new Node(child->is_leaf);
        int mid = (B_TREE_ORDER - 1) / 2;
        if (child->is_leaf) {
            sibling->num_keys = child->num_keys - mid;
            for (int j = 0; j < sibling->num_keys; ++j) {
                sibling->keys[j] = child->keys[mid + j];
                sibling->values[j] = child->values[mid + j];
            }
            child->num_keys = mid;
            for (int j = parent->num_keys; j > index; --j) {
                parent->keys[j] = parent->keys[j - 1];
                parent->children[j + 1] = parent->children[j];
            }
            parent->keys[index] = sibling->keys[0];
            parent->children[index + 1] = sibling;
            parent->num_keys++;
        } else {
            sibling->num_keys = child->num_keys - mid - 1;
            for (int j = 0; j < sibling->num_keys; ++j) sibling->keys[j] = child->keys[mid + 1 + j];
            for (int j = 0; j <= sibling->num_keys; ++j) sibling->children[j] = child->children[mid + 1 + j];
            Key promoted = child->keys[mid];
            child->num_keys = mid;
            for (int j = parent->num_keys; j > index; --j) {
                parent->keys[j] = parent->keys[j - 1];
                parent->children[j + 1] = parent->children[j];
            }
            parent->keys[index] = promoted;
            parent->children[index + 1] = sibling;
            parent->num_keys++;
        }
    }

    void destroy(Node* n) {
        if (!n) return;
        if (!n->is_leaf) {
            for (int i = 0; i <= n->num_keys; ++i) destroy(n->children[i]);
        }
        delete n;
    }
};
} // namespace olc_tree

// ============================================================================
// MAIN BENCHMARK
// ============================================================================
class NanoTimer {
    std::chrono::high_resolution_clock::time_point start_time;
public:
    NanoTimer() { reset(); }
    void reset() { start_time = std::chrono::high_resolution_clock::now(); }
    double elapsed_ns() const {
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::nano>(end - start_time).count();
    }
};

int main() {
    std::cout << "\n========================================================================================\n";
    std::cout << "         OPTIMISTIC LOCK COUPLING (OLC) & EBR BENCHMARK (APPLE SILICON)\n";
    std::cout << "========================================================================================\n";
    std::cout << "System: Apple M2 | 8 Cores | 128B Cache Lines | ARM64 LDAR/STLR\n\n";

    constexpr uint64_t TOTAL_READS = 10000000;
    constexpr uint64_t DATASET_SIZE = 10000;

    std::mt19937_64 rng(1337);
    std::vector<uint64_t> search_keys(TOTAL_READS);
    for (size_t i = 0; i < TOTAL_READS; ++i) {
        search_keys[i] = (rng() % DATASET_SIZE) + 1;
    }

    std::cout << ">>> 100% READ WORKLOAD (10,000,000 Lookups)\n";
    std::cout << "----------------------------------------------------------------------------------------\n";
    std::cout << std::setw(10) << "Threads"
              << std::setw(22) << "std::shared_mutex"
              << std::setw(22) << "OLC (Apple Silicon)"
              << std::setw(16) << "OLC Speedup" << "\n";
    std::cout << "----------------------------------------------------------------------------------------\n";

    for (int num_threads : {1, 2, 4, 8}) {
        olc_tree::CoarseLockedTree<uint64_t, uint64_t> rw_tree;
        olc_tree::ConcurrentOlcTree<uint64_t, uint64_t> olc_tree;
        for (uint64_t i = 1; i <= DATASET_SIZE; ++i) {
            rw_tree.insert(i, i * 7);
            olc_tree.insert(i, i * 7);
        }

        uint64_t ops_per_thread = TOTAL_READS / num_threads;

        // 1. RWLock
        double ns_rw = 0;
        {
            NanoTimer t;
            std::vector<std::thread> ths;
            for (int tid = 0; tid < num_threads; ++tid) {
                ths.emplace_back([&, tid]() {
                    uint64_t start_idx = tid * ops_per_thread;
                    uint64_t sum = 0, out_val = 0;
                    for (uint64_t i = 0; i < ops_per_thread; ++i) {
                        if (rw_tree.get(search_keys[start_idx + i], out_val)) sum += out_val;
                    }
                    asm volatile("" : : "r"(sum) : "memory");
                });
            }
            for (auto& th : ths) th.join();
            ns_rw = t.elapsed_ns();
        }

        // 2. OLC
        double ns_olc = 0;
        {
            NanoTimer t;
            std::vector<std::thread> ths;
            for (int tid = 0; tid < num_threads; ++tid) {
                ths.emplace_back([&, tid]() {
                    uint64_t start_idx = tid * ops_per_thread;
                    uint64_t sum = 0, out_val = 0;
                    for (uint64_t i = 0; i < ops_per_thread; ++i) {
                        if (olc_tree.get(search_keys[start_idx + i], out_val)) sum += out_val;
                    }
                    asm volatile("" : : "r"(sum) : "memory");
                });
            }
            for (auto& th : ths) th.join();
            ns_olc = t.elapsed_ns();
        }

        double total_ops = static_cast<double>(num_threads * ops_per_thread);
        double mops_rw = (total_ops / (ns_rw / 1e9)) / 1e6;
        double mops_olc = (total_ops / (ns_olc / 1e9)) / 1e6;
        double speedup = mops_olc / mops_rw;

        std::cout << std::setw(8) << num_threads << " T"
                  << std::setw(18) << std::fixed << std::setprecision(1) << mops_rw << " Mops/s"
                  << std::setw(18) << mops_olc << " Mops/s"
                  << std::setw(14) << std::setprecision(2) << speedup << "x\n";
    }

    std::cout << "\n>>> 90% READ / 10% WRITE MIXED WORKLOAD (5,000,000 Ops)\n";
    std::cout << "----------------------------------------------------------------------------------------\n";
    std::cout << std::setw(10) << "Threads"
              << std::setw(22) << "std::shared_mutex"
              << std::setw(22) << "OLC (Apple Silicon)"
              << std::setw(16) << "OLC Speedup" << "\n";
    std::cout << "----------------------------------------------------------------------------------------\n";

    constexpr uint64_t MIXED_OPS = 5000000;
    std::vector<uint64_t> mixed_keys(MIXED_OPS);
    std::vector<bool> is_write(MIXED_OPS);
    for (size_t i = 0; i < MIXED_OPS; ++i) {
        mixed_keys[i] = (rng() % DATASET_SIZE) + 1;
        is_write[i] = (rng() % 100) < 10;
    }

    for (int num_threads : {1, 2, 4, 8}) {
        olc_tree::CoarseLockedTree<uint64_t, uint64_t> rw_tree;
        olc_tree::ConcurrentOlcTree<uint64_t, uint64_t> olc_tree;
        for (uint64_t i = 1; i <= DATASET_SIZE; ++i) {
            rw_tree.insert(i, i * 7);
            olc_tree.insert(i, i * 7);
        }

        uint64_t ops_per_thread = MIXED_OPS / num_threads;

        double ns_rw = 0;
        {
            NanoTimer t;
            std::vector<std::thread> ths;
            for (int tid = 0; tid < num_threads; ++tid) {
                ths.emplace_back([&, tid]() {
                    uint64_t start = tid * ops_per_thread;
                    uint64_t sum = 0, out_val = 0;
                    for (uint64_t i = 0; i < ops_per_thread; ++i) {
                        uint64_t k = mixed_keys[start + i];
                        if (is_write[start + i]) rw_tree.insert(k, k * 11);
                        else if (rw_tree.get(k, out_val)) sum += out_val;
                    }
                    asm volatile("" : : "r"(sum) : "memory");
                });
            }
            for (auto& th : ths) th.join();
            ns_rw = t.elapsed_ns();
        }

        double ns_olc = 0;
        {
            NanoTimer t;
            std::vector<std::thread> ths;
            for (int tid = 0; tid < num_threads; ++tid) {
                ths.emplace_back([&, tid]() {
                    uint64_t start = tid * ops_per_thread;
                    uint64_t sum = 0, out_val = 0;
                    for (uint64_t i = 0; i < ops_per_thread; ++i) {
                        uint64_t k = mixed_keys[start + i];
                        if (is_write[start + i]) olc_tree.insert(k, k * 11);
                        else if (olc_tree.get(k, out_val)) sum += out_val;
                    }
                    asm volatile("" : : "r"(sum) : "memory");
                });
            }
            for (auto& th : ths) th.join();
            ns_olc = t.elapsed_ns();
        }

        double total_ops = static_cast<double>(num_threads * ops_per_thread);
        double mops_rw = (total_ops / (ns_rw / 1e9)) / 1e6;
        double mops_olc = (total_ops / (ns_olc / 1e9)) / 1e6;
        double speedup = mops_olc / mops_rw;

        std::cout << std::setw(8) << num_threads << " T"
                  << std::setw(18) << std::fixed << std::setprecision(1) << mops_rw << " Mops/s"
                  << std::setw(18) << mops_olc << " Mops/s"
                  << std::setw(14) << std::setprecision(2) << speedup << "x\n";
    }

    std::cout << "========================================================================================\n\n";
    return 0;
}
