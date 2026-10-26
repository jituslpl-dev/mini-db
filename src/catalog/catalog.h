// mini-db - catalog.h
//
// The Catalog manages metadata about tables, schemas, and indexes.
// It provides a mapping from table names to table ids, schemas, and
// HeapTable instances.  It also tracks indexes (B+ trees).
//
// The catalog persists its metadata in a dedicated catalog page on disk
// (a simple JSON-like serialization in the first data page).
//
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/schema.h"
#include "common/types.h"
#include "index/b_plus_tree.h"
#include "storage/buffer_pool.h"
#include "storage/heap_table.h"

namespace minidb {

struct TableInfo {
  uint32_t table_id;
  std::string name;
  std::shared_ptr<Schema> schema;
  std::unique_ptr<HeapTable> table;
  page_id_t first_page_id;
};

struct IndexInfo {
  uint32_t index_id;
  std::string name;
  uint32_t table_id;
  uint32_t column_index;  // which column the index is on
  std::unique_ptr<BPlusTree> tree;
  page_id_t root_page_id;
};

class Catalog {
 public:
  Catalog(BufferPoolManager* bpm);

  // Create a new table.  Returns its table_id.
  uint32_t createTable(const std::string& name,
                       const std::vector<Column>& columns);

  // Drop a table by name.
  bool dropTable(const std::string& name);

  // Get table by name.
  std::shared_ptr<TableInfo> getTable(const std::string& name) const;

  // Get table by id.
  std::shared_ptr<TableInfo> getTableById(uint32_t table_id) const;

  // Get schema by table id.
  std::shared_ptr<Schema> getSchemaById(uint32_t table_id) const;

  // Create an index on a column.
  uint32_t createIndex(const std::string& index_name, uint32_t table_id,
                       uint32_t column_index);

  // Get all indexes for a table.
  std::vector<std::shared_ptr<IndexInfo>> getIndexes(uint32_t table_id) const;

  // Get index by name.
  std::shared_ptr<IndexInfo> getIndex(const std::string& name) const;

  // List all table names.
  std::vector<std::string> listTables() const;

  // Persist catalog metadata to disk.
  void persist();

  // Load catalog metadata from disk.
  void load();

 private:
  BufferPoolManager* bpm_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<TableInfo>> tables_by_name_;
  std::unordered_map<uint32_t, std::shared_ptr<TableInfo>> tables_by_id_;
  std::unordered_map<std::string, std::shared_ptr<IndexInfo>> indexes_by_name_;
  std::unordered_map<uint32_t, std::vector<std::shared_ptr<IndexInfo>>>
      indexes_by_table_;
  uint32_t next_table_id_{1};
  uint32_t next_index_id_{1};
};

}  // namespace minidb