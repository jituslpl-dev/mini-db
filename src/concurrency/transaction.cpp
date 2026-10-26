// mini-db - transaction.cpp
#include "concurrency/transaction.h"

#include <iostream>

namespace minidb {

TransactionManager::TransactionManager(LockManager* lock_manager)
    : lock_manager_(lock_manager) {}

txn_id_t TransactionManager::begin() {
  txn_id_t id = next_txn_id_.fetch_add(1);
  auto txn = std::make_shared<Transaction>(id);
  std::lock_guard<std::mutex> guard(mutex_);
  txn_map_[id] = txn;
  return id;
}

bool TransactionManager::commit(txn_id_t txn_id) {
  std::shared_ptr<Transaction> txn;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = txn_map_.find(txn_id);
    if (it == txn_map_.end()) return false;
    txn = it->second;
  }

  if (txn->getState() != TransactionState::GROWING) return false;

  txn->setState(TransactionState::SHRINKING);
  // In Strict 2PL, we release all locks at commit time.
  lock_manager_->unlockAll(txn_id);
  txn->setState(TransactionState::COMMITTED);
  return true;
}

bool TransactionManager::abort(txn_id_t txn_id) {
  std::shared_ptr<Transaction> txn;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = txn_map_.find(txn_id);
    if (it == txn_map_.end()) return false;
    txn = it->second;
  }

  txn->setState(TransactionState::SHRINKING);

  // Rollback: iterate write set in reverse order and undo each operation.
  // The actual undo is performed by the recovery/storage layer; here we
  // just mark the transaction as aborted and release locks.
  auto write_set = txn->getWriteSet();
  // Note: actual undo logic is in the recovery module which reads the
  // write set.  For in-place rollback, the execution engine handles it.

  lock_manager_->unlockAll(txn_id);
  txn->setState(TransactionState::ABORTED);
  return true;
}

std::shared_ptr<Transaction> TransactionManager::getTransaction(
    txn_id_t txn_id) const {
  std::lock_guard<std::mutex> guard(mutex_);
  auto it = txn_map_.find(txn_id);
  if (it == txn_map_.end()) return nullptr;
  return it->second;
}

bool TransactionManager::lock(txn_id_t txn_id, const ResourceId& resource,
                               LockMode mode) {
  auto txn = getTransaction(txn_id);
  if (!txn || txn->getState() != TransactionState::GROWING) return false;

  bool ok = lock_manager_->lock(txn_id, resource, mode);
  if (ok) {
    txn->addLock(resource);
  } else {
    // Lock acquisition failed (deadlock victim): abort the transaction.
    abort(txn_id);
  }
  return ok;
}

}  // namespace minidb