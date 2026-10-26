// mini-db - lock_manager.cpp
#include "concurrency/lock_manager.h"

#include <algorithm>
#include <chrono>
#include <iostream>

namespace minidb {

// ---- Lock mode compatibility matrix ----
//         IS    IX    S     SIX   X
//  IS     Y     Y     Y     Y     N
//  IX     Y     Y     N     N     N
//  S      Y     N     Y     N     N
//  SIX    Y     N     N     N     N
//  X      N     N     N     N     N
bool LockManager::compatible(LockMode a, LockMode b) {
  if (a == b) {
    return a != LockMode::EXCLUSIVE && a != LockMode::SHARED_INTENTION_EXCLUSIVE;
  }
  // X is incompatible with everything.
  if (a == LockMode::EXCLUSIVE || b == LockMode::EXCLUSIVE) return false;

  // S is compatible with IS only.
  if (a == LockMode::SHARED) return b == LockMode::INTENTION_SHARED;
  if (b == LockMode::SHARED) return a == LockMode::INTENTION_SHARED;

  // SIX is compatible with IS only.
  if (a == LockMode::SHARED_INTENTION_EXCLUSIVE)
    return b == LockMode::INTENTION_SHARED;
  if (b == LockMode::SHARED_INTENTION_EXCLUSIVE)
    return a == LockMode::INTENTION_SHARED;

  // IX vs IS: compatible.
  return true;
}

// ---- LockRequestQueue ----
bool LockRequestQueue::compatibleWith(LockMode mode, txn_id_t txn_id) const {
  for (const auto& req : requests) {
    if (!req.granted) continue;
    if (req.txn_id == txn_id) continue;
    if (!LockManager::compatible(mode, req.mode)) return false;
  }
  return true;
}

bool LockRequestQueue::tryGrant(txn_id_t txn_id, LockMode mode) {
  // Check if this txn already holds a lock.
  for (auto& req : requests) {
    if (req.txn_id == txn_id && req.granted) {
      // Already holds a lock: check if we can upgrade.
      if (req.mode == mode) return true;
      // Upgrade: new mode must be stronger.
      if (compatibleWith(mode, txn_id)) {
        req.mode = mode;
        return true;
      }
      return false;
    }
  }

  // New request: check compatibility with all granted locks.
  if (!compatibleWith(mode, txn_id)) return false;

  // Grant it.
  requests.push_back({txn_id, mode, true, false});
  return true;
}

void LockRequestQueue::remove(txn_id_t txn_id) {
  requests.erase(
      std::remove_if(requests.begin(), requests.end(),
                     [txn_id](const LockRequest& r) {
                       return r.txn_id == txn_id;
                     }),
      requests.end());
  is_upgrading = false;
}

void LockRequestQueue::regrant() {
  // Try to grant waiting requests from front to back (FIFO).
  for (auto& req : requests) {
    if (req.granted) continue;
    if (compatibleWith(req.mode, req.txn_id)) {
      req.granted = true;
    } else {
      break;  // FIFO: don't skip ahead.
    }
  }
}

// ---- LockManager ----
LockManager::LockManager() = default;

bool LockManager::lock(txn_id_t txn_id, const ResourceId& resource,
                       LockMode mode) {
  std::unique_lock<std::mutex> guard(latch_);

  // Get or create the lock queue.
  auto& queue = lock_table_[resource];
  if (!queue) queue = std::make_unique<LockRequestQueue>();

  while (true) {
    // Try to grant immediately.
    if (queue->tryGrant(txn_id, mode)) {
      // Granted: remove from wait-for graph.
      removeEdge(txn_id, 0);  // remove all outgoing edges from txn_id
      return true;
    }

    // Check if this txn is already a victim.
    if (victim_txn_.load() == txn_id) {
      victim_txn_.store(INVALID_TXN_ID);
      removeEdge(txn_id, 0);
      return false;
    }

    // Can't grant: add to queue as waiting and record wait-for edges.
    // First check if already in queue as waiting.
    bool already_waiting = false;
    for (const auto& req : queue->requests) {
      if (req.txn_id == txn_id && !req.granted) {
        already_waiting = true;
        break;
      }
    }
    if (!already_waiting) {
      queue->requests.push_back({txn_id, mode, false, false});
    }

    // Add wait-for edges: txn_id waits for all granted holders.
    for (const auto& req : queue->requests) {
      if (req.granted && req.txn_id != txn_id) {
        addEdge(txn_id, req.txn_id);
      }
    }

    // Check for deadlock.
    txn_id_t victim;
    if (hasCycle(&victim)) {
      // Select the victim (the one with the highest txn_id, a simple policy).
      victim_txn_.store(victim);
      // Wake up the victim.
      if (cv_map_[victim]) cv_map_[victim]->notify_all();
      if (victim == txn_id) {
        victim_txn_.store(INVALID_TXN_ID);
        removeEdge(txn_id, 0);
        return false;
      }
    }

    // Wait for notification or timeout.
    // We poll with a timeout to re-check periodically.
    if (!cv_map_[txn_id]) {
      cv_map_[txn_id] = std::make_unique<std::condition_variable>();
    }
    cv_map_[txn_id]->wait_for(guard, std::chrono::milliseconds(100));
  }
}

void LockManager::unlockAll(txn_id_t txn_id) {
  std::lock_guard<std::mutex> guard(latch_);

  // Remove from all queues.
  std::vector<ResourceId> to_regrant;
  for (auto& [resource, queue] : lock_table_) {
    bool had = false;
    for (const auto& req : queue->requests) {
      if (req.txn_id == txn_id) {
        had = true;
        break;
      }
    }
    if (had) {
      queue->remove(txn_id);
      to_regrant.push_back(resource);
    }
  }

  // Regrant waiting requests on affected resources.
  for (const auto& resource : to_regrant) {
    auto& queue = lock_table_[resource];
    queue->regrant();
    // Wake up all waiters on this resource.
    for (const auto& req : queue->requests) {
      if (!req.granted) {
        if (cv_map_[req.txn_id]) cv_map_[req.txn_id]->notify_all();
      }
    }
  }

  // Remove from wait-for graph.
  removeEdge(txn_id, 0);
  waits_for_.erase(txn_id);
  for (auto& [t, edges] : waits_for_) {
    edges.erase(txn_id);
  }
}

// ---- Wait-for graph management ----
void LockManager::addEdge(txn_id_t t1, txn_id_t t2) {
  if (t2 == 0) return;  // sentinel: remove all
  waits_for_[t1].insert(t2);
}

void LockManager::removeEdge(txn_id_t t1, txn_id_t t2) {
  if (t2 == 0) {
    // Remove all outgoing edges from t1.
    waits_for_.erase(t1);
  } else {
    waits_for_[t1].erase(t2);
  }
}

bool LockManager::hasCycle(txn_id_t* victim) {
  std::unordered_set<txn_id_t> visited;
  std::unordered_set<txn_id_t> rec_stack;
  std::vector<txn_id_t> path;

  for (const auto& [node, _] : waits_for_) {
    if (visited.find(node) == visited.end()) {
      if (dfsCycle(node, visited, rec_stack, path, victim)) {
        return true;
      }
    }
  }
  return false;
}

bool LockManager::dfsCycle(txn_id_t node,
                           std::unordered_set<txn_id_t>& visited,
                           std::unordered_set<txn_id_t>& rec_stack,
                           std::vector<txn_id_t>& path,
                           txn_id_t* victim) {
  visited.insert(node);
  rec_stack.insert(node);
  path.push_back(node);

  auto it = waits_for_.find(node);
  if (it != waits_for_.end()) {
    for (txn_id_t neighbor : it->second) {
      if (visited.find(neighbor) == visited.end()) {
        if (dfsCycle(neighbor, visited, rec_stack, path, victim)) return true;
      } else if (rec_stack.count(neighbor)) {
        // Found a cycle.  Select the victim: the transaction with the
        // smallest id in the cycle (arbitrary but deterministic).
        *victim = neighbor;
        return true;
      }
    }
  }

  rec_stack.erase(node);
  path.pop_back();
  return false;
}

bool LockManager::detectDeadlock() {
  std::lock_guard<std::mutex> guard(latch_);
  txn_id_t victim;
  if (hasCycle(&victim)) {
    victim_txn_.store(victim);
    if (cv_map_[victim]) cv_map_[victim]->notify_all();
    return true;
  }
  return false;
}

void LockManager::startDeadlockDetector() {
  stop_detector_ = false;
  detector_thread_ = std::thread(&LockManager::detectorLoop, this);
}

void LockManager::stopDeadlockDetector() {
  stop_detector_ = true;
  if (detector_thread_.joinable()) detector_thread_.join();
}

void LockManager::detectorLoop() {
  while (!stop_detector_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    detectDeadlock();
  }
}

std::unordered_map<txn_id_t, std::unordered_set<txn_id_t>>
LockManager::getWaitForGraph() {
  std::lock_guard<std::mutex> guard(latch_);
  return waits_for_;
}

}  // namespace minidb