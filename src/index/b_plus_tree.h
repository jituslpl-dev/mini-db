// mini-db - b_plus_tree.h
//
// Concurrent B+ Tree with latch crabbing (lock coupling).
//
// Design:
//   - Keys are Value objects; values are rid_t (row ids).
//   - The tree maps a key to a set of rids (duplicate keys are allowed;
//     each (key, rid) pair is stored as a separate entry — keys are
//     internally augmented with the rid so every entry is unique).
//   - Internal nodes use "pointer-first" layout: [child_0, key_0, child_1,
//     key_1, ..., child_n], where key_i separates child_i and child_{i+1}.
//   - Leaf nodes store sorted entries and are linked via next_leaf pointers
//     for efficient range scans.
//
// Concurrency — Latch Crabbing:
//   - Search:  shared latches; release parent before descending.
//   - Insert/Delete: exclusive latches on the leaf; if the parent is "safe"
//     (won't split/merge), release ancestors.  Otherwise, hold the parent's
//     latch until the child is confirmed safe.  This is the classic
//     "lock-coupling" / crabbing protocol from Lehman & Yao (1981).
//
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "common/config.h"
#include "common/types.h"
#include "storage/buffer_pool.h"
#include "storage/page.h"

namespace minidb {

// A composite key: (value, rid).  Comparisons compare value first, then rid.
struct IndexKey {
  Value value;
  rid_t rid;

  IndexKey() = default;
  IndexKey(Value v, rid_t r) : value(std::move(v)), rid(r) {}

  bool operator<(const IndexKey& o) const {
    int c = value.compare(o.value);
    if (c != 0) return c < 0;
    return rid < o.rid;
  }
  bool operator==(const IndexKey& o) const {
    return value == o.value && rid == o.rid;
  }
};

// B+ tree node header, stored at the beginning of a page's payload.
struct BPlusNodeHeader {
  uint32_t is_leaf;       // 1 = leaf, 0 = internal
  uint32_t num_keys;
  page_id_t parent;       // 0 if root
  page_id_t next_leaf;    // only for leaves; linked list for range scan
  page_id_t page_id;      // this node's page id
};

class BPlusTree {
 public:
  BPlusTree(BufferPoolManager* bpm, page_id_t root_page_id);

  // Insert a (key, rid) entry.
  void insert(const Value& key, rid_t rid);

  // Remove a specific (key, rid) entry.
  void remove(const Value& key, rid_t rid);

  // Remove all entries with the given key; returns count removed.
  uint32_t removeAll(const Value& key);

  // Point lookup: find all rids with exactly `key`.
  std::vector<rid_t> search(const Value& key) const;

  // Range scan: all rids with key in [low, high].
  std::vector<rid_t> rangeScan(const Value& low, const Value& high) const;

  // Update rid for a key (remove old, insert new).
  void update(const Value& key, rid_t old_rid, rid_t new_rid);

  page_id_t getRootPageId() const { return root_page_id_; }

  // Validate tree invariants (for testing).
  bool validate() const;

  // Count total entries (for testing).
  uint64_t count() const;

  // Max keys per node (computed from page size).
  static constexpr uint32_t INTERNAL_MAX = 64;
  static constexpr uint32_t LEAF_MAX = 64;

  // Serialize/deserialize a key to/from a byte buffer.
  static std::string serializeKey(const IndexKey& key);
  static IndexKey deserializeKey(const char* data, uint32_t len);

 private:
  // ---- node helpers ----
  static BPlusNodeHeader readHeader(Page* page);
  static void writeHeader(Page* page, const BPlusNodeHeader& hdr);

  // ---- insertion helpers ----
  void insertIntoLeaf(Page* leaf, const IndexKey& key);
  void insertIntoParent(page_id_t left_pid, const IndexKey& key,
                        page_id_t right_pid);
  void splitLeaf(Page* leaf, Page* new_leaf, IndexKey* up_key);
  void splitInternal(Page* node, Page* new_node, IndexKey* up_key);

  // ---- deletion helpers ----
  void removeFromLeaf(Page* leaf, const IndexKey& key);
  void coalesceOrRedistribute(Page* node);
  void coalesceNodes(Page* node, Page* sibling, Page* parent,
                     uint32_t node_idx);
  void redistribute(Page* node, Page* sibling, Page* parent,
                    uint32_t node_idx, bool from_left);

  // ---- search helpers ----
  Page* findLeaf(const Value& key) const;

  BufferPoolManager* bpm_;
  page_id_t root_page_id_;
  mutable std::shared_mutex root_latch_;  // protects root_page_id_ changes
};

}  // namespace minidb