// test_btree.cpp - B+ Tree correctness tests
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <set>

#include "index/b_plus_tree.h"
#include "storage/buffer_pool.h"
#include "storage/heap_table.h"

using namespace minidb;

void testBasicInsertSearch() {
  std::string db_file = "test_btree1.db";
  std::filesystem::remove(db_file);

  BufferPoolManager bpm(db_file, 100, 2);
  page_id_t root_pid;
  bpm.newPage(&root_pid);
  bpm.unpinPage(root_pid, true);

  BPlusTree tree(&bpm, root_pid);

  // Insert 1000 keys.
  for (int i = 0; i < 1000; ++i) {
    tree.insert(Value::makeInteger(i), makeRid(i + 1, 0));
  }

  // Verify count.
  assert(tree.count() == 1000);

  // Search for each key.
  for (int i = 0; i < 1000; ++i) {
    auto rids = tree.search(Value::makeInteger(i));
    assert(rids.size() == 1);
    assert(ridToSlot(rids[0]) == 0);
  }

  // Search for non-existent key.
  auto rids = tree.search(Value::makeInteger(9999));
  assert(rids.empty());

  // Validate sorted order.
  assert(tree.validate());

  bpm.shutdown();
  std::filesystem::remove(db_file);
  std::cout << "[PASS] testBasicInsertSearch\n";
}

void testRangeScan() {
  std::string db_file = "test_btree2.db";
  std::filesystem::remove(db_file);

  BufferPoolManager bpm(db_file, 100, 2);
  page_id_t root_pid;
  bpm.newPage(&root_pid);
  bpm.unpinPage(root_pid, true);

  BPlusTree tree(&bpm, root_pid);

  for (int i = 0; i < 100; ++i) {
    tree.insert(Value::makeInteger(i), makeRid(i + 1, 0));
  }

  // Range [10, 20).
  auto rids = tree.rangeScan(Value::makeInteger(10), Value::makeInteger(19));
  assert(rids.size() == 10);

  // Range [0, 99].
  rids = tree.rangeScan(Value::makeInteger(0), Value::makeInteger(99));
  assert(rids.size() == 100);

  bpm.shutdown();
  std::filesystem::remove(db_file);
  std::cout << "[PASS] testRangeScan\n";
}

void testDelete() {
  std::string db_file = "test_btree3.db";
  std::filesystem::remove(db_file);

  BufferPoolManager bpm(db_file, 100, 2);
  page_id_t root_pid;
  bpm.newPage(&root_pid);
  bpm.unpinPage(root_pid, true);

  BPlusTree tree(&bpm, root_pid);

  for (int i = 0; i < 100; ++i) {
    tree.insert(Value::makeInteger(i), makeRid(i + 1, 0));
  }

  // Delete even keys.
  for (int i = 0; i < 100; i += 2) {
    tree.remove(Value::makeInteger(i), makeRid(i + 1, 0));
  }

  // Verify odd keys are still there.
  for (int i = 1; i < 100; i += 2) {
    auto rids = tree.search(Value::makeInteger(i));
    assert(rids.size() == 1);
  }

  // Verify even keys are gone.
  for (int i = 0; i < 100; i += 2) {
    auto rids = tree.search(Value::makeInteger(i));
    assert(rids.empty());
  }

  bpm.shutdown();
  std::filesystem::remove(db_file);
  std::cout << "[PASS] testDelete\n";
}

void testDuplicateKeys() {
  std::string db_file = "test_btree4.db";
  std::filesystem::remove(db_file);

  BufferPoolManager bpm(db_file, 100, 2);
  page_id_t root_pid;
  bpm.newPage(&root_pid);
  bpm.unpinPage(root_pid, true);

  BPlusTree tree(&bpm, root_pid);

  // Insert multiple rids for the same key.
  for (int i = 0; i < 5; ++i) {
    tree.insert(Value::makeInteger(42), makeRid(i + 1, 0));
  }

  auto rids = tree.search(Value::makeInteger(42));
  assert(rids.size() == 5);

  bpm.shutdown();
  std::filesystem::remove(db_file);
  std::cout << "[PASS] testDuplicateKeys\n";
}

int main() {
  testBasicInsertSearch();
  testRangeScan();
  testDelete();
  testDuplicateKeys();
  std::cout << "All B+ tree tests passed!\n";
  return 0;
}
