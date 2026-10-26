// mini-db - cli.cpp
// Interactive SQL shell for the mini-db RDBMS.
//
// Usage:
//   ./minidb_cli [db_file]
//
// If no db_file is given, defaults to "minidb.db".
//
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "catalog/catalog.h"
#include "concurrency/lock_manager.h"
#include "concurrency/transaction.h"
#include "execution/planner.h"
#include "parser/sql_parser.h"
#include "recovery/log_manager.h"
#include "recovery/recovery_manager.h"
#include "storage/buffer_pool.h"

using namespace minidb;

// Print query results in a formatted table.
static void printResult(const ExecutionResult& result) {
  if (!result.success) {
    std::cout << "ERROR: " << result.message << "\n";
    return;
  }

  // Print column header.
  if (!result.column_names.empty()) {
    for (size_t i = 0; i < result.column_names.size(); ++i) {
      if (i > 0) std::cout << " | ";
      std::cout << result.column_names[i];
    }
    std::cout << "\n";
    // Separator line.
    for (size_t i = 0; i < result.column_names.size(); ++i) {
      if (i > 0) std::cout << "-+-";
      std::cout << std::string(result.column_names[i].size(), '-');
    }
    std::cout << "\n";
  }

  // Print rows.
  for (const auto& row : result.rows) {
    for (size_t i = 0; i < row.size(); ++i) {
      if (i > 0) std::cout << " | ";
      std::cout << row[i].toString();
    }
    std::cout << "\n";
  }

  // Print message.
  if (!result.column_names.empty()) {
    std::cout << "(" << result.message << ")\n";
  } else {
    std::cout << result.message << "\n";
  }
}

// Print the query execution plan for EXPLAIN.
static void printPlan(const ExecutionResult& result) {
  std::cout << "QUERY PLAN\n";
  std::cout << "----------\n";
  std::cout << result.plan_string << "\n";
}

int main(int argc, char* argv[]) {
  std::string db_file = (argc > 1) ? argv[1] : "minidb.db";
  std::string log_file = db_file + ".log";

  std::cout << "mini-db RDBMS v1.0\n";
  std::cout << "Database: " << db_file << "\n";
  std::cout << "Type 'help' for commands, 'exit' to quit.\n\n";

  // Initialize engine components.
  auto buffer_pool = std::make_unique<BufferPoolManager>(db_file, 512, 2);
  auto catalog = std::make_unique<Catalog>(buffer_pool.get());
  auto lock_manager = std::make_unique<LockManager>();
  auto txn_manager = std::make_unique<TransactionManager>(lock_manager.get());
  auto log_manager = std::make_unique<LogManager>(log_file);

  // Run recovery on startup.
  RecoveryManager recovery(log_manager.get(), catalog.get(), buffer_pool.get());
  recovery.recover();

  lock_manager->startDeadlockDetector();

  Planner planner(catalog.get(), log_manager.get(), txn_manager.get());

  // Interactive loop.
  std::string line;
  std::string buffer;
  while (true) {
    // Prompt.
    if (buffer.empty()) {
      std::cout << "db> ";
    } else {
      std::cout << "... ";
    }
    std::cout.flush();

    if (!std::getline(std::cin, line)) {
      break;
    }

    // Trim leading/trailing whitespace.
    while (!line.empty() && std::isspace(line.front())) line.erase(0, 1);
    while (!line.empty() && std::isspace(line.back())) line.pop_back();

    if (line.empty()) continue;

    // Special commands.
    if (buffer.empty()) {
      if (line == "exit" || line == "quit") {
        break;
      }
      if (line == "help") {
        std::cout << "Commands:\n";
        std::cout << "  CREATE TABLE name (col TYPE, ...);\n";
        std::cout << "  DROP TABLE name;\n";
        std::cout << "  INSERT INTO name VALUES (...);\n";
        std::cout << "  DELETE FROM name WHERE ...;\n";
        std::cout << "  UPDATE name SET col = val WHERE ...;\n";
        std::cout << "  SELECT ... FROM name [JOIN ...] [WHERE ...] "
                     "[GROUP BY ...] [ORDER BY ...] [LIMIT n];\n";
        std::cout << "  EXPLAIN SELECT ...;\n";
        std::cout << "  help - show this message\n";
        std::cout << "  exit - quit\n";
        continue;
      }
      if (line == "tables") {
        auto names = catalog->listTables();
        std::cout << "Tables:\n";
        for (const auto& n : names) std::cout << "  " << n << "\n";
        continue;
      }
    }

    // Accumulate input until we see a semicolon.
    buffer += line + " ";
    if (buffer.find(';') == std::string::npos) {
      continue;
    }

    // Execute the statement.
    SQLParser parser(buffer);
    auto stmt = parser.parse();

    if (!stmt) {
      std::cout << "PARSE ERROR: " << parser.getError() << "\n";
      buffer.clear();
      continue;
    }

    if (stmt->kind == SQLStatement::Kind::EXPLAIN) {
      // For EXPLAIN, show the plan.
      const auto& select = std::get<SelectStmt>(stmt->data);
      auto plan = planner.buildSelectPlan(select);
      if (plan) {
        ExecutionResult result;
        result.success = true;
        result.plan_string = stmt->toString() + "\n\n";
        // Rebuild with plan string.
        std::string plan_str;
        planner.buildSelectPlan(select, &plan_str);
        result.plan_string += plan_str;
        printPlan(result);
      } else {
        std::cout << "ERROR: Could not build plan\n";
      }
      buffer.clear();
      continue;
    }

    ExecutionResult result = planner.execute(*stmt);
    printResult(result);

    // Flush log on DML.
    if (stmt->kind == SQLStatement::Kind::INSERT ||
        stmt->kind == SQLStatement::Kind::DELETE ||
        stmt->kind == SQLStatement::Kind::UPDATE) {
      log_manager->flush();
    }

    buffer.clear();
  }

  // Shutdown.
  std::cout << "Shutting down...\n";
  lock_manager->stopDeadlockDetector();
  log_manager->flush();
  catalog->persist();
  buffer_pool->shutdown();

  return 0;
}