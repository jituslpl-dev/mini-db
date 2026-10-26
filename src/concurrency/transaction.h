// mini-db - transaction.h
//
// Transaction state and the TransactionManager.
//
// Transaction lifecycle:
//
//   BEGIN  -->  GROWING  (acquire locks)
//                    |
//               SHRINKING  (only release on abort/commit in Strict 2PL,
//                            so GROWING->SHRINKING happens at commit/abort)
//                    |
//           COMMITTED / ABORTED
//
// Each transaction tracks the locks it holds and its write set (for
// rollback).  The TransactionManager creates, commits, and aborts
// transactions, coordinating with the LockManager.
//
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/types.h"
#include "concurrency/lock_manager.h"

namespace minidb {

enum class TransactionState {
  GROWING,
  SHRINKING,
  COMMITTED,
  ABORTED
};

// A write record for undo.  Stores enough info to reverse an operation.
enum class WriteType { INSERT, DELETE_TUPLE, UPDATE };
struct WriteRecord {
  WriteType type;
  uint32_t table_id;
  rid_t rid;
  Tuple old_tuple;  // for UPDATE/DELETE undo
};

class Transaction {
 public:
  explicit Transaction(txn_id_t id, IsolationLevel /*level*/ = IsolationLevel::SERIALIZABLE)
      : txn_id_(id) {}

  txn_id_t getTxnId() const { return txn_id_; }
  TransactionState getState() const { return state_; }
  void setState(TransactionState s) { state_ = s; }

  bool isAborted() const { return state_ == TransactionState::ABORTED; }
  bool isCommitted() const { return state_ == TransactionState::COMMITTED; }

  // Record a write for potential undo.
  void appendWriteRecord(const WriteRecord& rec) {
    std::lock_guard<std::mutex> guard(mutex_);
    write_set_.push_back(rec);
  }

  // Get the write set (for rollback).
  std::vector<WriteRecord> getWriteSet() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return write_set_;
  }

  // Track locked resources.
  void addLock(const ResourceId& res) {
    std::lock_guard<std::mutex> guard(mutex_);
    locked_resources_.insert(res);
  }

  std::unordered_set<ResourceId, ResourceIdHash> getLockedResources() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return locked_resources_;
  }

 private:
  txn_id_t txn_id_;
  std::atomic<TransactionState> state_{TransactionState::GROWING};
  mutable std::mutex mutex_;
  std::vector<WriteRecord> write_set_;
  std::unordered_set<ResourceId, ResourceIdHash> locked_resources_;
};

class TransactionManager {
 public:
  explicit TransactionManager(LockManager* lock_manager);

  // Begin a new transaction; returns its id.
  txn_id_t begin();

  // Commit a transaction: release all locks.
  bool commit(txn_id_t txn_id);

  // Abort a transaction: rollback writes (via write set), release locks.
  bool abort(txn_id_t txn_id);

  // Get a transaction by id.
  std::shared_ptr<Transaction> getTransaction(txn_id_t txn_id) const;

  // Acquire a lock on behalf of a transaction.
  bool lock(txn_id_t txn_id, const ResourceId& resource, LockMode mode);

 private:
  std::atomic<txn_id_t> next_txn_id_{1};
  LockManager* lock_manager_;
  mutable std::mutex mutex_;
  std::unordered_map<txn_id_t, std::shared_ptr<Transaction>> txn_map_;
};

}  // namespace minidb