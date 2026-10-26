// mini-db - log_manager.h
//
// Write-Ahead Logging (WAL) system.
//
// The log is an append-only file of log records.  Each record has a
// monotonically increasing Log Sequence Number (LSN).  The log manager
// buffers records in memory and flushes them to disk:
//   - On commit of any transaction (force-at-commit).
//   - When the buffer is full.
//
// WAL Rule: a data page may not be written to disk until all log records
// describing modifications to that page have been flushed.
//
// Log record types:
//   BEGIN    - transaction started
//   COMMIT   - transaction committed
//   ABORT    - transaction aborted
//   INSERT   - tuple inserted (for REDO)
//   DELETE   - tuple deleted (for UNDO)
//   UPDATE   - tuple updated (for UNDO/REDO)
//   CHECKPOINT - fuzzy checkpoint marker
//
// Record format (variable-length, length-prefixed):
//   [4 bytes total_len][4 bytes lsn][4 bytes txn_id][1 byte type]
//   [type-specific payload]
//
#pragma once

#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "common/config.h"
#include "common/types.h"

namespace minidb {

enum class LogRecordType : uint8_t {
  BEGIN = 1,
  COMMIT = 2,
  ABORT = 3,
  INSERT = 4,
  DELETE_TUPLE = 5,
  UPDATE = 6,
  CHECKPOINT = 7,
  NEW_PAGE = 8,
};

struct LogRecord {
  lsn_t lsn{0};
  txn_id_t txn_id{0};
  LogRecordType type{LogRecordType::BEGIN};

  // For INSERT/DELETE/UPDATE:
  uint32_t table_id{0};
  rid_t rid{0};
  std::string old_data;  // for UNDO (DELETE/UPDATE)
  std::string new_data;  // for REDO (INSERT/UPDATE)

  // Serialize to a byte string (including a 4-byte length prefix).
  std::string serialize() const;

  // Deserialize from a byte buffer; returns bytes consumed, 0 on error.
  static bool deserialize(const std::string& buf, size_t* offset,
                          LogRecord* out);

  std::string toString() const;
};

class LogManager {
 public:
  explicit LogManager(const std::string& log_file);
  ~LogManager();

  // Append a log record.  Returns its LSN.
  lsn_t appendRecord(const LogRecord& rec);

  // Flush all buffered log records up to (and including) the given LSN.
  // If flush_lsn is 0, flush everything.
  void flush(lsn_t flush_lsn = 0);

  // Get the current LSN (last assigned + 1 conceptually).
  lsn_t getNextLSN() const { return next_lsn_.load(); }

  // Get the last flushed LSN.
  lsn_t getFlushedLSN() const { return flushed_lsn_.load(); }

  // Read all records from the log file (for recovery).
  std::vector<LogRecord> readAllRecords();

  // Truncate the log (after checkpoint).
  void truncate();

 private:
  void flushBuffer();

  std::string log_file_;
  std::ofstream log_stream_;
  std::mutex mutex_;
  std::string buffer_;
  std::atomic<lsn_t> next_lsn_{1};
  std::atomic<lsn_t> flushed_lsn_{0};
};

}  // namespace minidb