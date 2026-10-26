// mini-db - planner.cpp
#include "execution/planner.h"

#include <sstream>

#include "concurrency/transaction.h"
#include "recovery/log_manager.h"

namespace minidb {

Planner::Planner(Catalog* catalog, LogManager* log_manager,
                 TransactionManager* txn_manager)
    : catalog_(catalog),
      log_manager_(log_manager),
      txn_manager_(txn_manager) {}

ExecutionResult Planner::execute(const SQLStatement& stmt) {
  switch (stmt.kind) {
    case SQLStatement::Kind::CREATE_TABLE:
      return executeCreateTable(std::get<CreateTableStmt>(stmt.data));
    case SQLStatement::Kind::DROP_TABLE:
      return executeDropTable(std::get<DropTableStmt>(stmt.data));
    case SQLStatement::Kind::INSERT:
      return executeInsert(std::get<InsertStmt>(stmt.data));
    case SQLStatement::Kind::DELETE:
      return executeDelete(std::get<DeleteStmt>(stmt.data));
    case SQLStatement::Kind::UPDATE:
      return executeUpdate(std::get<UpdateStmt>(stmt.data));
    case SQLStatement::Kind::SELECT:
      return executeSelect(std::get<SelectStmt>(stmt.data));
    case SQLStatement::Kind::EXPLAIN: {
      auto result = executeSelect(std::get<SelectStmt>(stmt.data));
      result.plan_string = stmt.toString();
      return result;
    }
    case SQLStatement::Kind::UNKNOWN:
      return {.message = "Unknown statement", .success = false};
  }
  return {};
}

ExecutionResult Planner::executeCreateTable(const CreateTableStmt& stmt) {
  // Convert ColumnDef (parser AST) to Column (schema).
  std::vector<Column> columns;
  for (const auto& col : stmt.columns) {
    columns.emplace_back(col.name, col.type, col.nullable);
  }
  uint32_t id = catalog_->createTable(stmt.table_name, columns);
  if (id == 0) {
    return {.message = "Table already exists: " + stmt.table_name,
            .success = false};
  }
  return {.message = "CREATE TABLE " + stmt.table_name + " (id=" +
                       std::to_string(id) + ")",
          .success = true};
}

ExecutionResult Planner::executeDropTable(const DropTableStmt& stmt) {
  if (catalog_->dropTable(stmt.table_name)) {
    return {.message = "DROP TABLE " + stmt.table_name, .success = true};
  }
  return {.message = "Table not found: " + stmt.table_name,
          .success = false};
}

ExecutionResult Planner::executeInsert(const InsertStmt& stmt) {
  auto table_info = catalog_->getTable(stmt.table_name);
  if (!table_info) {
    return {.message = "Table not found: " + stmt.table_name,
            .success = false};
  }

  uint32_t count = 0;
  for (const auto& row : stmt.rows) {
    // Type-check values against schema.
    Tuple tuple;
    const auto& cols = table_info->schema->columns();
    if (row.size() != cols.size()) {
      return {.message = "Column count mismatch", .success = false};
    }
    for (size_t i = 0; i < row.size(); ++i) {
      tuple.push_back(row[i]);
    }

    rid_t rid = table_info->table->insertTuple(tuple);
    count++;

    // WAL: log the insert.
    if (log_manager_) {
      LogRecord rec;
      rec.txn_id = 0;
      rec.type = LogRecordType::INSERT;
      rec.table_id = table_info->table_id;
      rec.rid = rid;
      rec.new_data = table_info->schema->serializeTuple(tuple);
      log_manager_->appendRecord(rec);
    }

    // Update indexes.
    auto indexes = catalog_->getIndexes(table_info->table_id);
    for (auto& idx : indexes) {
      idx->tree->insert(tuple[idx->column_index], rid);
    }
  }

  return {.message = "INSERT " + std::to_string(count) + " row(s)",
          .success = true};
}

ExecutionResult Planner::executeDelete(const DeleteStmt& stmt) {
  auto table_info = catalog_->getTable(stmt.table_name);
  if (!table_info) {
    return {.message = "Table not found: " + stmt.table_name,
            .success = false};
  }

  // Scan and collect matching rids.
  std::vector<rid_t> to_delete;
  table_info->table->scan([&](rid_t rid, const Tuple& tuple) {
    if (stmt.where) {
      ExprEvaluator eval(*table_info->schema, stmt.table_name);
      if (!eval.evalPredicate(*stmt.where, tuple)) return true;
    }
    to_delete.push_back(rid);
    return true;
  });

  // Delete and update indexes.
  auto indexes = catalog_->getIndexes(table_info->table_id);
  for (rid_t rid : to_delete) {
    Tuple tuple;
    table_info->table->getTuple(rid, &tuple);
    for (auto& idx : indexes) {
      idx->tree->remove(tuple[idx->column_index], rid);
    }
    table_info->table->deleteTuple(rid);

    if (log_manager_) {
      LogRecord rec;
      rec.txn_id = 0;
      rec.type = LogRecordType::DELETE_TUPLE;
      rec.table_id = table_info->table_id;
      rec.rid = rid;
      rec.old_data = table_info->schema->serializeTuple(tuple);
      log_manager_->appendRecord(rec);
    }
  }

  return {.message = "DELETE " + std::to_string(to_delete.size()) + " row(s)",
          .success = true};
}

ExecutionResult Planner::executeUpdate(const UpdateStmt& stmt) {
  auto table_info = catalog_->getTable(stmt.table_name);
  if (!table_info) {
    return {.message = "Table not found: " + stmt.table_name,
            .success = false};
  }

  uint32_t count = 0;
  table_info->table->scan([&](rid_t rid, const Tuple& tuple) {
    if (stmt.where) {
      ExprEvaluator eval(*table_info->schema, stmt.table_name);
      if (!eval.evalPredicate(*stmt.where, tuple)) return true;
    }

    // Apply assignments.
    Tuple new_tuple = tuple;
    for (const auto& [col, expr] : stmt.assignments) {
      int idx = table_info->schema->columnIndex(col);
      if (idx < 0) return true;
      ExprEvaluator eval(*table_info->schema, stmt.table_name);
      new_tuple[idx] = eval.eval(*expr, tuple);
    }

    // Update indexes if key columns changed.
    auto indexes = catalog_->getIndexes(table_info->table_id);
    for (auto& idx : indexes) {
      idx->tree->remove(tuple[idx->column_index], rid);
    }
    table_info->table->updateTuple(rid, new_tuple);
    for (auto& idx : indexes) {
      idx->tree->insert(new_tuple[idx->column_index], rid);
    }

    if (log_manager_) {
      LogRecord rec;
      rec.txn_id = 0;
      rec.type = LogRecordType::UPDATE;
      rec.table_id = table_info->table_id;
      rec.rid = rid;
      rec.old_data = table_info->schema->serializeTuple(tuple);
      rec.new_data = table_info->schema->serializeTuple(new_tuple);
      log_manager_->appendRecord(rec);
    }

    count++;
    return true;
  });

  return {.message = "UPDATE " + std::to_string(count) + " row(s)",
          .success = true};
}

std::shared_ptr<Schema> Planner::buildJoinSchema(const Schema& left,
                                                   const Schema& right) {
  std::vector<Column> cols;
  for (const auto& c : left.columns()) cols.push_back(c);
  for (const auto& c : right.columns()) cols.push_back(c);
  return std::make_shared<Schema>(cols);
}

std::unique_ptr<Executor> Planner::buildSelectPlan(const SelectStmt& stmt,
                                                     std::string* plan_str) {
  auto table_info = catalog_->getTable(stmt.from_table);
  if (!table_info) return nullptr;

  // Start with a sequential scan.
  std::unique_ptr<Executor> plan =
      std::make_unique<SeqScanExecutor>(table_info->table.get(),
                                        table_info->schema,
                                        stmt.from_table);
  std::shared_ptr<Schema> current_schema = table_info->schema;

  if (plan_str) *plan_str += "SeqScan(" + stmt.from_table + ")\n";

  // Index selection: check if WHERE has an equality on an indexed column.
  // (Simplified: only for a simple col = literal predicate.)
  auto indexes = catalog_->getIndexes(table_info->table_id);
  if (stmt.where && !indexes.empty()) {
    // Check for simple equality predicate.
    const Expr* w = stmt.where.get();
    if (w->type == ExprType::BINARY_OP && w->op == OpType::EQ) {
      // Left is column, right is literal.
      if (w->left && w->left->type == ExprType::COLUMN_REF &&
          w->right && w->right->type == ExprType::LITERAL_INT) {
        int col_idx = table_info->schema->columnIndex(w->left->column);
        for (auto& idx : indexes) {
          if (idx->column_index == static_cast<uint32_t>(col_idx)) {
            // Use index scan.
            plan = std::make_unique<IndexScanExecutor>(
                table_info->table.get(), idx->tree.get(), table_info->schema,
                idx->column_index, Value::makeInteger(w->right->int_val));
            if (plan_str) *plan_str += "  -> IndexScan(" + idx->name + ")\n";
            break;
          }
        }
      }
    }
  }

  // Join.
  if (!stmt.join_table.empty()) {
    auto right_info = catalog_->getTable(stmt.join_table);
    if (right_info) {
      auto right_scan = std::make_unique<SeqScanExecutor>(
          right_info->table.get(), right_info->schema, stmt.join_table);
      auto join_schema = buildJoinSchema(*current_schema, *right_info->schema);
      // Use HashJoin if the condition is a simple equality on a column
      // from each side; otherwise use NestedLoopJoin.
      plan = std::make_unique<NestedLoopJoinExecutor>(
          std::move(plan), std::move(right_scan), stmt.join_condition.get(),
          join_schema);
      current_schema = join_schema;
      if (plan_str) *plan_str += "  -> NestedLoopJoin\n";
    }
  }

  // WHERE filter (predicate pushdown: applies right after scan/join).
  if (stmt.where) {
    plan = std::make_unique<FilterExecutor>(std::move(plan),
                                             stmt.where.get(),
                                             stmt.from_table);
    if (plan_str) *plan_str += "  -> Filter(WHERE)\n";
  }

  // GROUP BY / Aggregate.
  if (!stmt.group_by.empty()) {
    std::vector<Column> agg_cols;
    std::vector<uint32_t> group_cols;
    for (const auto& col_name : stmt.group_by) {
      int idx = current_schema->columnIndex(col_name);
      if (idx >= 0) {
        group_cols.push_back(idx);
        agg_cols.push_back(current_schema->column(idx));
      }
    }
    agg_cols.push_back({"count", TypeId::INTEGER});
    auto agg_schema = std::make_shared<Schema>(agg_cols);
    plan = std::make_unique<AggregateExecutor>(std::move(plan), agg_schema,
                                                group_cols);
    current_schema = agg_schema;
    if (plan_str) *plan_str += "  -> Aggregate(GROUP BY)\n";
  }

  // ORDER BY.
  if (!stmt.order_by.empty()) {
    std::vector<std::pair<uint32_t, bool>> sort_keys;
    for (const auto& item : stmt.order_by) {
      int idx = current_schema->columnIndex(item.column);
      if (idx >= 0) {
        sort_keys.emplace_back(idx, item.descending);
      }
    }
    plan = std::make_unique<SortExecutor>(std::move(plan), sort_keys);
    if (plan_str) *plan_str += "  -> Sort(ORDER BY)\n";
  }

  // Projection (column selection).
  if (!stmt.select_all && !stmt.columns.empty()) {
    std::vector<Column> proj_cols;
    std::vector<uint32_t> col_indices;
    for (const auto& [tbl, col] : stmt.columns) {
      int idx = current_schema->columnIndex(col);
      if (idx >= 0) {
        col_indices.push_back(idx);
        proj_cols.push_back(current_schema->column(idx));
      }
    }
    auto proj_schema = std::make_shared<Schema>(proj_cols);
    plan = std::make_unique<ProjectionExecutor>(std::move(plan), proj_schema,
                                                 col_indices);
    current_schema = proj_schema;
    if (plan_str) *plan_str += "  -> Projection\n";
  }

  // LIMIT.
  if (stmt.limit >= 0) {
    plan = std::make_unique<LimitExecutor>(std::move(plan), stmt.limit);
    if (plan_str) *plan_str += "  -> Limit(" + std::to_string(stmt.limit) + ")\n";
  }

  return plan;
}

ExecutionResult Planner::executeSelect(const SelectStmt& stmt) {
  std::string plan_str;
  auto plan = buildSelectPlan(stmt, &plan_str);
  if (!plan) {
    return {.message = "Table not found: " + stmt.from_table,
            .success = false};
  }

  plan->init();

  ExecutionResult result;
  result.success = true;

  // Column names.
  auto schema = plan->getOutputSchema();
  if (schema) {
    for (const auto& col : schema->columns()) {
      result.column_names.push_back(col.name);
    }
  }

  // Rows.
  ExecRow row;
  while (plan->next(&row)) {
    result.rows.push_back(row.tuple);
  }

  result.plan_string = plan_str;
  result.message = std::to_string(result.rows.size()) + " row(s)";
  return result;
}

}  // namespace minidb