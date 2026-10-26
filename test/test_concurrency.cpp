// test_concurrency.cpp - Tests for lock manager, transactions, deadlocks
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include "concurrency/lock_manager.h"
#include "concurrency/transaction.h"

using namespace minidb;

void testBasicLocking() {
  LockManager lm;
  TransactionManager tm(&lm);

  txn_id_t t1 = tm.begin();
  ResourceId r1{1, 0};

  // Acquire shared lock.
  assert(tm.lock(t1, r1, LockMode::SHARED));
  // Should be able to acquire IX (upgrade to SIX).
  // Actually, S and IX conflict.  Let's test basic S/X.
  tm.commit(t1);

  // Exclusive lock.
  txn_id_t t2 = tm.begin();
  assert(tm.lock(t2, r1, LockMode::EXCLUSIVE));
  tm.commit(t2);

  std::cout << "[PASS] testBasicLocking\n";
}

void testSharedLocksCompatible() {
  LockManager lm;
  TransactionManager tm(&lm);

  txn_id_t t1 = tm.begin();
  txn_id_t t2 = tm.begin();
  ResourceId r1{1, 0};

  // Two shared locks should be compatible.
  assert(tm.lock(t1, r1, LockMode::SHARED));
  assert(tm.lock(t2, r1, LockMode::SHARED));

  tm.commit(t1);
  tm.commit(t2);
  std::cout << "[PASS] testSharedLocksCompatible\n";
}

void testExclusiveBlocks() {
  LockManager lm;
  TransactionManager tm(&lm);

  txn_id_t t1 = tm.begin();
  txn_id_t t2 = tm.begin();
  ResourceId r1{1, 0};

  // t1 gets X lock.
  assert(tm.lock(t1, r1, LockMode::EXCLUSIVE));

  // t2 tries to get S lock; should block.
  // We run it in a separate thread.
  std::atomic<bool> got_lock{false};
  std::thread t([&]() {
    bool ok = tm.lock(t2, r1, LockMode::SHARED);
    got_lock = ok;
    if (ok) tm.commit(t2);
  });

  // Wait a bit.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  assert(!got_lock);  // t2 should still be blocked.

  // t1 commits, releasing the lock.
  tm.commit(t1);
  t.join();
  assert(got_lock);  // t2 should now have the lock.

  std::cout << "[PASS] testExclusiveBlocks\n";
}

void testDeadlockDetection() {
  LockManager lm;
  TransactionManager tm(&lm);
  lm.startDeadlockDetector();

  txn_id_t t1 = tm.begin();
  txn_id_t t2 = tm.begin();
  ResourceId r1{1, 0};
  ResourceId r2{2, 0};

  // t1 locks r1.
  assert(tm.lock(t1, r1, LockMode::EXCLUSIVE));
  // t2 locks r2.
  assert(tm.lock(t2, r2, LockMode::EXCLUSIVE));

  // Now t1 tries to lock r2 (held by t2) -> waits.
  // And t2 tries to lock r1 (held by t1) -> waits.
  // This is a deadlock.

  std::atomic<bool> t1_aborted{false};
  std::atomic<bool> t2_aborted{false};

  std::thread th1([&]() {
    bool ok = tm.lock(t1, r2, LockMode::EXCLUSIVE);
    if (!ok) {
      t1_aborted = true;
    } else {
      tm.commit(t1);
    }
  });

  std::thread th2([&]() {
    bool ok = tm.lock(t2, r1, LockMode::EXCLUSIVE);
    if (!ok) {
      t2_aborted = true;
    } else {
      tm.commit(t2);
    }
  });

  th1.join();
  th2.join();

  // At least one should have been aborted.
  assert(t1_aborted || t2_aborted);

  lm.stopDeadlockDetector();
  std::cout << "[PASS] testDeadlockDetection\n";
}

void testLockUpgrade() {
  LockManager lm;
  TransactionManager tm(&lm);

  txn_id_t t1 = tm.begin();
  ResourceId r1{1, 0};

  // Acquire S, then upgrade to X.
  assert(tm.lock(t1, r1, LockMode::SHARED));
  assert(tm.lock(t1, r1, LockMode::EXCLUSIVE));

  tm.commit(t1);
  std::cout << "[PASS] testLockUpgrade\n";
}

int main() {
  testBasicLocking();
  testSharedLocksCompatible();
  testExclusiveBlocks();
  testDeadlockDetection();
  testLockUpgrade();
  std::cout << "All concurrency tests passed!\n";
  return 0;
}