// test_recovery.cpp - WAL and recovery tests
#include <cassert>
#include <filesystem>
#include <iostream>

#include "catalog/catalog.h"
#include "execution/planner.h"
#include "parser/sql_parser.h"
#include "recovery/log_manager.h"
#include "recovery/recovery_manager.h"
#include "storage/buffer_pool.h"

using namespace minidb;

static ExecutionResult execSQL(Planner& planner, const std::string& sql) {
  SQLParser parser(sql);
  auto stmt = parser.parse();
  return planner.execute(*stmt);
}

void testWALBasic() {
  std::string db_file = "test_rec1.db";
  std::string log_file = db_file + ".log";
  std::filesystem::remove(db_file);
  std::filesystem::remove(log_file);

  {
    BufferPoolManager bpm(db_file, 100, 2);
    Catalog catalog(&bpm);
    LogManager lm(log_file);

    // Log some records.
    LogRecord r1;
    r1.txn_id = 1;
    r1.type = LogRecordType::BEGIN;
    lsn_t lsn1 = lm.appendRecord(r1);
    assert(lsn1 == 1);

    LogRecord r2;
    r2.txn_id = 1;
    r2.type = LogRecordType::INSERT;
    r2.table_id = 1;
    r2.rid = 42;
    r2.new_data = "test_data";
    lsn_t lsn2 = lm.appendRecord(r2);
    assert(lsn2 == 2);

    LogRecord r3;
    r3.txn_id = 1;
    r3.type = LogRecordType::COMMIT;
    lm.appendRecord(r3);

    lm.flush();

    // Read back.
    auto records = lm.readAllRecords();
    assert(records.size() == 3);
    assert(records[0].type == LogRecordType::BEGIN);
    assert(records[1].type == LogRecordType::INSERT);
    assert(records[1].rid == 42);
    assert(records[2].type == LogRecordType::COMMIT);

    bpm.shutdown();
  }

  std::filesystem::remove(db_file);
  std::filesystem::remove(log_file);
  std::cout << "[PASS] testWALBasic\n";
}

void testCrashRecovery() {
  std::string db_file = "test_rec2.db";
  std::string log_file = db_file + ".log";
  std::filesystem::remove(db_file);
  std::filesystem::remove(log_file);

  // Phase 1: insert data, simulate crash (don't flush buffer pool).
  {
    BufferPoolManager bpm(db_file, 100, 2);
    Catalog catalog(&bpm);
    LogManager lm(log_file);
    Planner planner(&catalog, &lm);

    execSQL(planner, "CREATE TABLE t (id INTEGER, name VARCHAR);");
    execSQL(planner, "INSERT INTO t VALUES (1, 'Alice');");
    execSQL(planner, "INSERT INTO t VALUES (2, 'Bob');");

    // Flush log but NOT buffer pool (simulate crash).
    lm.flush();
    // Don't call bpm.shutdown() to simulate crash.
  }

  // Phase 2: recover.
  {
    BufferPoolManager bpm(db_file, 100, 2);
    Catalog catalog(&bpm);
    LogManager lm(log_file);

    RecoveryManager rm(&lm, &catalog, &bpm);
    auto stats = rm.recover();
    // Should have found records.
    assert(stats.redone >= 0);

    bpm.shutdown();
  }

  std::filesystem::remove(db_file);
  std::filesystem::remove(log_file);
  std::cout << "[PASS] testCrashRecovery\n";
}

void testLogSerialization() {
  // Test serialize/deserialize round-trip.
  LogRecord rec;
  rec.lsn = 42;
  rec.txn_id = 7;
  rec.type = LogRecordType::UPDATE;
  rec.table_id = 3;
  rec.rid = 99;
  rec.old_data = "old";
  rec.new_data = "new_data_here";

  std::string serialized = rec.serialize();

  size_t offset = 0;
  LogRecord recovered;
  assert(LogRecord::deserialize(serialized, &offset, &recovered));
  assert(recovered.lsn == 42);
  assert(recovered.txn_id == 7);
  assert(recovered.type == LogRecordType::UPDATE);
  assert(recovered.table_id == 3);
  assert(recovered.rid == 99);
  assert(recovered.old_data == "old");
  assert(recovered.new_data == "new_data_here");

  std::cout << "[PASS] testLogSerialization\n";
}

int main() {
  testLogSerialization();
  testWALBasic();
  testCrashRecovery();
  std::cout << "All recovery tests passed!\n";
  return 0;
}