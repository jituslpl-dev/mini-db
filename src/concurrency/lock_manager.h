// mini-db - lock_manager.h
//
// LockManager implements a hierarchical lock system compatible with
// Strict Two-Phase Locking (Strict 2PL):
//
//   - Locks are acquired on resources identified by a (table_oid, rid)
//     pair.  Table-level locks use rid = 0.
//   - Lock modes: SHARED (S), EXCLUSIVE (X), INTENTION_SHARED (IS),
//     INTENTION_EXCLUSIVE (IX), SHARED_INTENTION_EXCLUSIVE (SIX).
//   - Compatibility matrix follows the standard ANSI SQL isolation rules.
//   - Lock upgrades are supported (e.g. S -> X).
//   - Deadlock detection uses a wait-for graph; a background thread (or
//     synchronous detection) runs cycle detection and aborts a victim.
//
// Strict 2PL: all locks are held until the transaction commits or aborts
// (no early release in the growing phase only).
//
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/config.h"
#include "common/types.h"

namespace minidb {

// Lock modes.
enum class LockMode {
  SHARED,             // S
  EXCLUSIVE,          // X
  INTENTION_SHARED,   // IS
  INTENTION_EXCLUSIVE,// IX
  SHARED_INTENTION_EXCLUSIVE  // SIX
};

// A resource identifier: (table_id, rid).  rid = 0 means table-level.
struct ResourceId {
  uint32_t table_id;
  rid_t rid;

  bool operator==(const ResourceId& o) const {
    return table_id == o.table_id && rid == o.rid;
  }

  std::string toString() const {
    return "R(" + std::to_string(table_id) + "," + std::to_string(rid) + ")";
  }
};

struct ResourceIdHash {
  std::size_t operator()(const ResourceId& r) const noexcept {
    return std::hash<uint64_t>{}(
        (static_cast<uint64_t>(r.table_id) << 32) | (r.rid & 0xFFFFFFFF));
  }
};

// A single lock request from a transaction.
struct LockRequest {
  txn_id_t txn_id;
  LockMode mode;
  bool granted{false};
  bool upgrading{false};
};

// The queue of lock requests for a single resource.
class LockRequestQueue {
 public:
  // Try to grant a lock request.  Returns true if granted immediately.
  bool tryGrant(txn_id_t txn_id, LockMode mode);

  // Check compatibility of `mode` with all currently granted locks
  // (excluding txn_id itself, for upgrades).
  bool compatibleWith(LockMode mode, txn_id_t txn_id) const;

  // Remove a transaction from the queue (on abort/commit).
  void remove(txn_id_t txn_id);

  // After removing a txn, re-grant as many waiting requests as possible.
  void regrant();

  std::vector<LockRequest> requests;
  bool is_upgrading{false};
};

class LockManager {
 public:
  LockManager();

  // Acquire a lock.  Blocks until granted or the transaction is chosen
  // as a deadlock victim (returns false).
  bool lock(txn_id_t txn_id, const ResourceId& resource, LockMode mode);

  // Release all locks held by a transaction (on commit or abort).
  void unlockAll(txn_id_t txn_id);

  // Run deadlock detection.  Returns true if a deadlock was found and
  // a victim was selected and notified.
  bool detectDeadlock();

  // Enable / disable background deadlock detection.
  void startDeadlockDetector();
  void stopDeadlockDetector();

  // For testing: get the wait-for graph.
  std::unordered_map<txn_id_t, std::unordered_set<txn_id_t>> getWaitForGraph();

  // Compatibility check.
  static bool compatible(LockMode a, LockMode b);

 private:
  void addEdge(txn_id_t t1, txn_id_t t2);
  void removeEdge(txn_id_t t1, txn_id_t t2);
  bool hasCycle(txn_id_t* victim);
  bool dfsCycle(txn_id_t node, std::unordered_set<txn_id_t>& visited,
                std::unordered_set<txn_id_t>& rec_stack,
                std::vector<txn_id_t>& path, txn_id_t* victim);

  std::mutex latch_;
  std::unordered_map<ResourceId, std::unique_ptr<LockRequestQueue>,
                     ResourceIdHash>
      lock_table_;

  // Wait-for graph: txn -> set of txns it's waiting for.
  std::unordered_map<txn_id_t, std::unordered_set<txn_id_t>> waits_for_;

  // Per-transaction condition variables for blocking.  Stored as
  // unique_ptr because std::condition_variable is not movable.
  std::unordered_map<txn_id_t, std::unique_ptr<std::condition_variable>> cv_map_;

  // Deadlock victim selection.
  std::atomic<txn_id_t> victim_txn_{INVALID_TXN_ID};

  // Background deadlock detector.
  std::thread detector_thread_;
  std::atomic<bool> stop_detector_{false};
  void detectorLoop();
};

}  // namespace minidb