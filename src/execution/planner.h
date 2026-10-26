// mini-db - planner.h
//
// The Planner converts a parsed SQLStatement (AST) into a tree of
// Executor operators (physical plan).  It applies simple optimizations:
//
//   - Predicate pushdown: pushes WHERE filters as close to the scan as
//     possible (in our simple model, directly on top of the scan).
//   - Projection pushdown: if only a subset of columns is needed,
//     projects early.
//   - Index selection: if a WHERE clause has an equality predicate on an
//     indexed column, uses an IndexScan instead of SeqScan.
//
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "execution/executor.h"
#include "parser/sql_parser.h"
#include "recovery/log_manager.h"

namespace minidb {

struct ExecutionResult {
  std::vector<std::string> column_names;
  std::vector<std::vector<Value>> rows;
  std::string message;
  bool success{false};

  // For EXPLAIN: the plan string.
  std::string plan_string;
};

class Planner {
 public:
  Planner(Catalog* catalog, LogManager* log_manager = nullptr,
          TransactionManager* txn_manager = nullptr);

  // Execute a statement and return results.
  ExecutionResult execute(const SQLStatement& stmt);

  // Build a physical plan for a SELECT statement (for EXPLAIN).
  std::unique_ptr<Executor> buildSelectPlan(const SelectStmt& stmt,
                                             std::string* plan_str = nullptr);

 private:
  // DDL/DML executors.
  ExecutionResult executeCreateTable(const CreateTableStmt& stmt);
  ExecutionResult executeDropTable(const DropTableStmt& stmt);
  ExecutionResult executeInsert(const InsertStmt& stmt);
  ExecutionResult executeDelete(const DeleteStmt& stmt);
  ExecutionResult executeUpdate(const UpdateStmt& stmt);
  ExecutionResult executeSelect(const SelectStmt& stmt);

  // Build a combined schema for join (left columns + right columns).
  std::shared_ptr<Schema> buildJoinSchema(const Schema& left,
                                           const Schema& right);

  Catalog* catalog_;
  LogManager* log_manager_;
  TransactionManager* txn_manager_;
};

}  // namespace minidb