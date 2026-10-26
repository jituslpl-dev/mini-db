// mini-db - recovery_manager.h
//
// Crash recovery using ARIES-style REDO/UNDO.
//
// Phases:
//   1. Analysis:  Scan the log forward.  Identify committed and aborted
//                  transactions.  Build the "active transaction" table.
//   2. REDO:      Re-apply all logged modifications from the earliest
//                  log record forward, to reconstruct the database state
//                  as it was at crash time.
//   3. UNDO:      For transactions that were active at crash time (not
//                  committed), roll back their modifications in reverse
//                  log order.
//
// For simplicity, we replay INSERT records (redo) and reverse them (undo)
// directly against the heap table via the catalog.  This is a simplified
// version of ARIES without page-level LSN tracking.
//
#pragma once

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "recovery/log_manager.h"

namespace minidb {

class Catalog;
class BufferPoolManager;

class RecoveryManager {
 public:
  RecoveryManager(LogManager* log_manager, Catalog* catalog,
                  BufferPoolManager* bpm);

  // Run the full recovery process.  Returns the number of transactions
  // recovered.
  struct RecoveryStats {
    uint32_t redone{0};
    uint32_t undone{0};
    uint32_t active_txns{0};
  };

  RecoveryStats recover();

 private:
  // Analysis: determine which txns committed, which were active.
  void analyze(const std::vector<LogRecord>& records,
               std::unordered_set<txn_id_t>& committed,
               std::unordered_set<txn_id_t>& active);

  // Redo: replay all modifications.
  void redo(const std::vector<LogRecord>& records);

  // Undo: reverse modifications of active (uncommitted) txns.
  void undo(const std::vector<LogRecord>& records,
            const std::unordered_set<txn_id_t>& active);

  LogManager* log_manager_;
  Catalog* catalog_;
  BufferPoolManager* bpm_;
};

}  // namespace minidb