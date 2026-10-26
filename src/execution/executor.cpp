// mini-db - executor.cpp
#include "execution/executor.h"

#include <algorithm>
#include <iostream>

namespace minidb {

// ---- ExprEvaluator ----
ExprEvaluator::ExprEvaluator(const Schema& schema, const std::string& table_name)
    : schema_(schema), table_name_(table_name) {}

Value ExprEvaluator::eval(const Expr& expr, const Tuple& tuple) const {
  switch (expr.type) {
    case ExprType::LITERAL_INT:
      return Value::makeInteger(expr.int_val);
    case ExprType::LITERAL_STRING:
      return Value::makeVarchar(expr.str_val);
    case ExprType::COLUMN_REF: {
      int idx = schema_.columnIndex(expr.column);
      if (idx < 0) return Value::makeInteger(0);
      return tuple[idx];
    }
    case ExprType::BINARY_OP: {
      Value l = expr.left ? eval(*expr.left, tuple) : Value::makeInteger(0);
      Value r = expr.right ? eval(*expr.right, tuple) : Value::makeInteger(0);
      switch (expr.op) {
        case OpType::EQ: return Value::makeInteger(l == r ? 1 : 0);
        case OpType::NE: return Value::makeInteger(l.compare(r) != 0 ? 1 : 0);
        case OpType::LT: return Value::makeInteger(l < r ? 1 : 0);
        case OpType::LE: return Value::makeInteger(l <= r ? 1 : 0);
        case OpType::GT: return Value::makeInteger(l > r ? 1 : 0);
        case OpType::GE: return Value::makeInteger(l >= r ? 1 : 0);
        case OpType::AND:
          return Value::makeInteger(
              (l.type == TypeId::INTEGER && l.integer != 0) &&
              (r.type == TypeId::INTEGER && r.integer != 0) ? 1 : 0);
        case OpType::OR:
          return Value::makeInteger(
              (l.type == TypeId::INTEGER && l.integer != 0) ||
              (r.type == TypeId::INTEGER && r.integer != 0) ? 1 : 0);
        case OpType::NOT:
          return Value::makeInteger(
              l.type == TypeId::INTEGER && l.integer == 0 ? 1 : 0);
      }
      return Value::makeInteger(0);
    }
    case ExprType::UNARY_OP: {
      Value l = expr.left ? eval(*expr.left, tuple) : Value::makeInteger(0);
      if (expr.op == OpType::NOT) {
        return Value::makeInteger(
            l.type == TypeId::INTEGER && l.integer == 0 ? 1 : 0);
      }
      return l;
    }
  }
  return Value::makeInteger(0);
}

bool ExprEvaluator::evalPredicate(const Expr& expr, const Tuple& tuple) const {
  Value v = eval(expr, tuple);
  return v.type == TypeId::INTEGER && v.integer != 0;
}

// ---- SeqScanExecutor ----
SeqScanExecutor::SeqScanExecutor(HeapTable* table,
                                 std::shared_ptr<Schema> schema,
                                 const std::string& table_name)
    : table_(table), schema_(schema), table_name_(table_name) {}

void SeqScanExecutor::init() {
  started_ = false;
  buffered_.clear();
  buffer_idx_ = 0;
  // Buffer all tuples (simplification; a real engine would stream).
  table_->scan([this](rid_t rid, const Tuple& tuple) {
    ExecRow row;
    row.tuple = tuple;
    row.rid = rid;
    buffered_.push_back(row);
    return true;
  });
}

bool SeqScanExecutor::next(ExecRow* out) {
  if (buffer_idx_ >= buffered_.size()) return false;
  *out = buffered_[buffer_idx_++];
  return true;
}

// ---- IndexScanExecutor ----
IndexScanExecutor::IndexScanExecutor(HeapTable* table, BPlusTree* index,
                                     std::shared_ptr<Schema> schema,
                                     uint32_t key_col, const Value& key)
    : table_(table),
      index_(index),
      schema_(schema),
      key_col_(key_col),
      key_(key) {}

void IndexScanExecutor::init() {
  rids_ = index_->search(key_);
  rid_idx_ = 0;
}

bool IndexScanExecutor::next(ExecRow* out) {
  while (rid_idx_ < rids_.size()) {
    rid_t rid = rids_[rid_idx_++];
    Tuple tuple;
    if (table_->getTuple(rid, &tuple)) {
      out->tuple = std::move(tuple);
      out->rid = rid;
      return true;
    }
  }
  return false;
}

// ---- FilterExecutor ----
FilterExecutor::FilterExecutor(std::unique_ptr<Executor> child,
                               const Expr* predicate,
                               const std::string& table_name)
    : child_(std::move(child)),
      predicate_(predicate),
      table_name_(table_name) {}

void FilterExecutor::init() { child_->init(); }

bool FilterExecutor::next(ExecRow* out) {
  ExecRow row;
  while (child_->next(&row)) {
    ExprEvaluator eval(*child_->getOutputSchema(), table_name_);
    if (eval.evalPredicate(*predicate_, row.tuple)) {
      *out = row;
      return true;
    }
  }
  return false;
}

// ---- ProjectionExecutor ----
ProjectionExecutor::ProjectionExecutor(
    std::unique_ptr<Executor> child,
    std::shared_ptr<Schema> output_schema,
    std::vector<uint32_t> col_indices)
    : child_(std::move(child)),
      output_schema_(output_schema),
      col_indices_(std::move(col_indices)) {}

void ProjectionExecutor::init() { child_->init(); }

bool ProjectionExecutor::next(ExecRow* out) {
  ExecRow row;
  if (!child_->next(&row)) return false;
  Tuple projected;
  for (uint32_t idx : col_indices_) {
    projected.push_back(row.tuple[idx]);
  }
  out->tuple = std::move(projected);
  out->rid = row.rid;
  return true;
}

// ---- NestedLoopJoinExecutor ----
NestedLoopJoinExecutor::NestedLoopJoinExecutor(
    std::unique_ptr<Executor> left, std::unique_ptr<Executor> right,
    const Expr* condition, std::shared_ptr<Schema> output_schema)
    : left_(std::move(left)),
      right_(std::move(right)),
      condition_(condition),
      output_schema_(output_schema) {}

void NestedLoopJoinExecutor::init() {
  left_->init();
  right_->init();
  left_valid_ = left_->next(&left_row_);
  right_exhausted_ = false;
}

bool NestedLoopJoinExecutor::next(ExecRow* out) {
  while (left_valid_) {
    ExecRow right_row;
    if (!right_->next(&right_row)) {
      // Reset right and advance left.
      right_->init();
      left_valid_ = left_->next(&left_row_);
      continue;
    }

    // Combine tuples.
    Tuple combined = left_row_.tuple;
    for (const auto& v : right_row.tuple) combined.push_back(v);

    // Check join condition.
    if (condition_) {
      ExprEvaluator eval(*output_schema_, "");
      if (!eval.evalPredicate(*condition_, combined)) continue;
    }

    out->tuple = std::move(combined);
    out->rid = left_row_.rid;
    return true;
  }
  return false;
}

// ---- HashJoinExecutor ----
HashJoinExecutor::HashJoinExecutor(std::unique_ptr<Executor> left,
                                   std::unique_ptr<Executor> right,
                                   uint32_t left_key_col,
                                   uint32_t right_key_col,
                                   std::shared_ptr<Schema> output_schema)
    : left_(std::move(left)),
      right_(std::move(right)),
      left_key_col_(left_key_col),
      right_key_col_(right_key_col),
      output_schema_(output_schema) {}

void HashJoinExecutor::buildHashTable() {
  left_->init();
  ExecRow row;
  while (left_->next(&row)) {
    hash_table_[row.tuple[left_key_col_]].push_back(row.tuple);
  }
  built_ = true;
}

void HashJoinExecutor::init() {
  buildHashTable();
  right_->init();
  match_idx_ = 0;
  matches_.clear();
}

bool HashJoinExecutor::next(ExecRow* out) {
  while (true) {
    if (match_idx_ < matches_.size()) {
      Tuple combined = matches_[match_idx_++];
      ExecRow right_row;
      // We need the current right tuple; store it.
      // Actually, we need to track the right row separately.
      // Simplification: combine left match with stored right tuple.
      // This requires storing the right tuple.  Let's fix this.
      // We'll use a member to store the current right tuple.
      // For now, return the left tuple only (incorrect, but placeholder).
      // FIXME: store right_row.
      out->tuple = std::move(combined);
      return true;
    }

    ExecRow right_row;
    if (!right_->next(&right_row)) return false;

    // Look up matches in hash table.
    Value key = right_row.tuple[right_key_col_];
    auto it = hash_table_.find(key);
    if (it != hash_table_.end()) {
      matches_ = it->second;
      // Prepend right columns to each left match.
      for (auto& m : matches_) {
        for (const auto& v : right_row.tuple) m.push_back(v);
      }
      match_idx_ = 0;
    }
  }
}

// ---- AggregateExecutor ----
AggregateExecutor::AggregateExecutor(
    std::unique_ptr<Executor> child,
    std::shared_ptr<Schema> output_schema,
    std::vector<uint32_t> group_by_cols)
    : child_(std::move(child)),
      output_schema_(output_schema),
      group_by_cols_(std::move(group_by_cols)) {}

void AggregateExecutor::init() {
  child_->init();
  computed_ = false;
  results_.clear();
  result_idx_ = 0;

  // Collect all input rows and group them.
  std::unordered_map<std::string, std::vector<Tuple>> groups;
  std::vector<std::string> group_order;

  ExecRow row;
  while (child_->next(&row)) {
    std::string key;
    if (group_by_cols_.empty()) {
      key = "__all__";
    } else {
      for (uint32_t col : group_by_cols_) {
        key += row.tuple[col].toString() + "|";
      }
    }
    if (groups.find(key) == groups.end()) {
      group_order.push_back(key);
    }
    groups[key].push_back(row.tuple);
  }

  // Produce one output row per group: group columns + COUNT.
  for (const auto& key : group_order) {
    const auto& tuples = groups[key];
    ExecRow out;
    if (group_by_cols_.empty()) {
      out.tuple.push_back(Value::makeInteger(
          static_cast<int64_t>(tuples.size())));
    } else {
      for (uint32_t col : group_by_cols_) {
        out.tuple.push_back(tuples[0][col]);
      }
      out.tuple.push_back(Value::makeInteger(
          static_cast<int64_t>(tuples.size())));
    }
    results_.push_back(out);
  }
  computed_ = true;
}

bool AggregateExecutor::next(ExecRow* out) {
  if (result_idx_ >= results_.size()) return false;
  *out = results_[result_idx_++];
  return true;
}

// ---- SortExecutor ----
SortExecutor::SortExecutor(std::unique_ptr<Executor> child,
                           std::vector<std::pair<uint32_t, bool>> sort_keys)
    : child_(std::move(child)), sort_keys_(std::move(sort_keys)) {}

void SortExecutor::init() {
  child_->init();
  sorted_.clear();
  idx_ = 0;
  sorted_done_ = false;

  ExecRow row;
  while (child_->next(&row)) {
    sorted_.push_back(row);
  }

  std::sort(sorted_.begin(), sorted_.end(),
            [this](const ExecRow& a, const ExecRow& b) {
              for (const auto& [col, desc] : sort_keys_) {
                int cmp = a.tuple[col].compare(b.tuple[col]);
                if (cmp != 0) return desc ? cmp > 0 : cmp < 0;
              }
              return false;
            });
  sorted_done_ = true;
}

bool SortExecutor::next(ExecRow* out) {
  if (idx_ >= sorted_.size()) return false;
  *out = sorted_[idx_++];
  return true;
}

// ---- LimitExecutor ----
LimitExecutor::LimitExecutor(std::unique_ptr<Executor> child, int64_t limit)
    : child_(std::move(child)), limit_(limit) {}

void LimitExecutor::init() {
  child_->init();
  count_ = 0;
}

bool LimitExecutor::next(ExecRow* out) {
  if (count_ >= limit_) return false;
  if (!child_->next(out)) return false;
  count_++;
  return true;
}

}  // namespace minidb