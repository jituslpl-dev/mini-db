// mini-db - executor.h
//
// Volcano model iterator-based execution engine.
//
// Every operator implements the same interface:
//   - init()   : called once before iteration begins
//   - next()   : returns the next tuple, or false if exhausted
//   - getOutputSchema() : the schema of tuples this operator produces
//
// Operators are composed into a tree; each operator pulls tuples from its
// children (hence "pull-based" / Volcano model).
//
//   SeqScan  ->  Filter  ->  Project  ->  Sort  ->  Limit
//                    |
//                    +-> HashJoin <- IndexScan
//
// Supported operators:
//   - SeqScan:         sequential scan of a heap table
//   - IndexScan:       scan via B+ tree index (point or range)
//   - Filter (Selection): applies a WHERE predicate
//   - Project:         selects/reorders columns
//   - NestedLoopJoin:  simple nested-loop join
//   - HashJoin:        hash-based equi-join
//   - Aggregate:        GROUP BY with COUNT/SUM
//   - Sort:             in-memory sort
//   - Limit:            LIMIT n
//
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "catalog/catalog.h"
#include "common/schema.h"
#include "common/types.h"
#include "concurrency/transaction.h"
#include "index/b_plus_tree.h"
#include "parser/sql_parser.h"
#include "storage/buffer_pool.h"
#include "storage/heap_table.h"

namespace minidb {

// An executed row carries both the tuple and the rid (for updates/deletes).
struct ExecRow {
  Tuple tuple;
  rid_t rid{0};
};

// Base class for all operators.
class Executor {
 public:
  virtual ~Executor() = default;
  virtual void init() = 0;
  virtual bool next(ExecRow* out) = 0;
  virtual std::shared_ptr<Schema> getOutputSchema() const = 0;
};

// ---- Expression evaluator ----
// Evaluates an Expr against a tuple, given a schema and optional table
// name for resolving column references.
class ExprEvaluator {
 public:
  ExprEvaluator(const Schema& schema, const std::string& table_name = "");

  Value eval(const Expr& expr, const Tuple& tuple) const;
  bool evalPredicate(const Expr& expr, const Tuple& tuple) const;

 private:
  const Schema& schema_;
  std::string table_name_;
};

// ---- Sequential Scan ----
class SeqScanExecutor : public Executor {
 public:
  SeqScanExecutor(HeapTable* table, std::shared_ptr<Schema> schema,
                  const std::string& table_name);

  void init() override;
  bool next(ExecRow* out) override;
  std::shared_ptr<Schema> getOutputSchema() const override { return schema_; }

 private:
  HeapTable* table_;
  std::shared_ptr<Schema> schema_;
  std::string table_name_;
  bool started_{false};
  std::vector<ExecRow> buffered_;
  size_t buffer_idx_{0};
};

// ---- Index Scan ----
class IndexScanExecutor : public Executor {
 public:
  IndexScanExecutor(HeapTable* table, BPlusTree* index,
                    std::shared_ptr<Schema> schema, uint32_t key_col,
                    const Value& key);

  void init() override;
  bool next(ExecRow* out) override;
  std::shared_ptr<Schema> getOutputSchema() const override { return schema_; }

 private:
  HeapTable* table_;
  BPlusTree* index_;
  std::shared_ptr<Schema> schema_;
  uint32_t key_col_;
  Value key_;
  std::vector<rid_t> rids_;
  size_t rid_idx_{0};
};

// ---- Filter (Selection) ----
class FilterExecutor : public Executor {
 public:
  FilterExecutor(std::unique_ptr<Executor> child, const Expr* predicate,
                 const std::string& table_name);

  void init() override;
  bool next(ExecRow* out) override;
  std::shared_ptr<Schema> getOutputSchema() const override {
    return child_->getOutputSchema();
  }

 private:
  std::unique_ptr<Executor> child_;
  const Expr* predicate_;
  std::string table_name_;
};

// ---- Projection ----
class ProjectionExecutor : public Executor {
 public:
  ProjectionExecutor(std::unique_ptr<Executor> child,
                     std::shared_ptr<Schema> output_schema,
                     std::vector<uint32_t> col_indices);

  void init() override;
  bool next(ExecRow* out) override;
  std::shared_ptr<Schema> getOutputSchema() const override {
    return output_schema_;
  }

 private:
  std::unique_ptr<Executor> child_;
  std::shared_ptr<Schema> output_schema_;
  std::vector<uint32_t> col_indices_;
};

// ---- Nested Loop Join ----
class NestedLoopJoinExecutor : public Executor {
 public:
  NestedLoopJoinExecutor(std::unique_ptr<Executor> left,
                         std::unique_ptr<Executor> right,
                         const Expr* condition,
                         std::shared_ptr<Schema> output_schema);

  void init() override;
  bool next(ExecRow* out) override;
  std::shared_ptr<Schema> getOutputSchema() const override {
    return output_schema_;
  }

 private:
  std::unique_ptr<Executor> left_;
  std::unique_ptr<Executor> right_;
  const Expr* condition_;
  std::shared_ptr<Schema> output_schema_;
  ExecRow left_row_;
  bool left_valid_{false};
  bool right_exhausted_{false};
};

// ---- Hash Join ----
class HashJoinExecutor : public Executor {
 public:
  HashJoinExecutor(std::unique_ptr<Executor> left,
                   std::unique_ptr<Executor> right,
                   uint32_t left_key_col, uint32_t right_key_col,
                   std::shared_ptr<Schema> output_schema);

  void init() override;
  bool next(ExecRow* out) override;
  std::shared_ptr<Schema> getOutputSchema() const override {
    return output_schema_;
  }

 private:
  void buildHashTable();

  std::unique_ptr<Executor> left_;
  std::unique_ptr<Executor> right_;
  uint32_t left_key_col_;
  uint32_t right_key_col_;
  std::shared_ptr<Schema> output_schema_;

  // Hash table: key = left column value, value = list of left tuples.
  std::unordered_map<Value, std::vector<Tuple>, ValueHash> hash_table_;
  bool built_{false};

  // Current matching list for the current right tuple.
  std::vector<Tuple> matches_;
  size_t match_idx_{0};
};

// ---- Aggregate (GROUP BY + COUNT/SUM) ----
class AggregateExecutor : public Executor {
 public:
  AggregateExecutor(std::unique_ptr<Executor> child,
                     std::shared_ptr<Schema> output_schema,
                     std::vector<uint32_t> group_by_cols);

  void init() override;
  bool next(ExecRow* out) override;
  std::shared_ptr<Schema> getOutputSchema() const override {
    return output_schema_;
  }

 private:
  std::unique_ptr<Executor> child_;
  std::shared_ptr<Schema> output_schema_;
  std::vector<uint32_t> group_by_cols_;
  std::vector<ExecRow> results_;
  size_t result_idx_{0};
  bool computed_{false};
};

// ---- Sort ----
class SortExecutor : public Executor {
 public:
  SortExecutor(std::unique_ptr<Executor> child,
               std::vector<std::pair<uint32_t, bool>> sort_keys);

  void init() override;
  bool next(ExecRow* out) override;
  std::shared_ptr<Schema> getOutputSchema() const override {
    return child_->getOutputSchema();
  }

 private:
  std::unique_ptr<Executor> child_;
  std::vector<std::pair<uint32_t, bool>> sort_keys_;
  std::vector<ExecRow> sorted_;
  size_t idx_{0};
  bool sorted_done_{false};
};

// ---- Limit ----
class LimitExecutor : public Executor {
 public:
  LimitExecutor(std::unique_ptr<Executor> child, int64_t limit);

  void init() override;
  bool next(ExecRow* out) override;
  std::shared_ptr<Schema> getOutputSchema() const override {
    return child_->getOutputSchema();
  }

 private:
  std::unique_ptr<Executor> child_;
  int64_t limit_;
  int64_t count_{0};
};

}  // namespace minidb