// mini-db - catalog.cpp
#include "catalog/catalog.h"

#include <cstring>
#include <sstream>

namespace minidb {

Catalog::Catalog(BufferPoolManager* bpm) : bpm_(bpm) {}

uint32_t Catalog::createTable(const std::string& name,
                              const std::vector<Column>& columns) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (tables_by_name_.find(name) != tables_by_name_.end()) {
    return 0;  // table already exists
  }

  // Allocate a page for the table's first heap page.
  page_id_t first_page_id;
  Page* first_page = bpm_->newPage(&first_page_id);
  // Initialize as empty heap page.
  HeapPageHeader hdr{0, 0};
  std::memcpy(first_page->getPayload(), &hdr, sizeof(HeapPageHeader));
  first_page->setDirty(true);
  bpm_->unpinPage(first_page_id, true);

  uint32_t table_id = next_table_id_++;
  auto info = std::make_shared<TableInfo>();
  info->table_id = table_id;
  info->name = name;
  info->schema = std::make_shared<Schema>(columns);
  info->first_page_id = first_page_id;
  info->table = std::make_unique<HeapTable>(bpm_, info->schema, first_page_id);

  tables_by_name_[name] = info;
  tables_by_id_[table_id] = info;
  return table_id;
}

bool Catalog::dropTable(const std::string& name) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto it = tables_by_name_.find(name);
  if (it == tables_by_name_.end()) return false;

  uint32_t table_id = it->second->table_id;
  tables_by_name_.erase(it);
  tables_by_id_.erase(table_id);

  // Also remove indexes.
  indexes_by_table_.erase(table_id);
  return true;
}

std::shared_ptr<TableInfo> Catalog::getTable(const std::string& name) const {
  std::lock_guard<std::mutex> guard(mutex_);
  auto it = tables_by_name_.find(name);
  if (it == tables_by_name_.end()) return nullptr;
  return it->second;
}

std::shared_ptr<TableInfo> Catalog::getTableById(uint32_t table_id) const {
  std::lock_guard<std::mutex> guard(mutex_);
  auto it = tables_by_id_.find(table_id);
  if (it == tables_by_id_.end()) return nullptr;
  return it->second;
}

std::shared_ptr<Schema> Catalog::getSchemaById(uint32_t table_id) const {
  auto info = getTableById(table_id);
  if (!info) return nullptr;
  return info->schema;
}

uint32_t Catalog::createIndex(const std::string& index_name,
                               uint32_t table_id, uint32_t column_index) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (indexes_by_name_.find(index_name) != indexes_by_name_.end()) return 0;

  // Allocate a page for the B+ tree root.
  page_id_t root_page_id;
  Page* root = bpm_->newPage(&root_page_id);
  root->setDirty(true);
  bpm_->unpinPage(root_page_id, true);

  uint32_t index_id = next_index_id_++;
  auto info = std::make_shared<IndexInfo>();
  info->index_id = index_id;
  info->name = index_name;
  info->table_id = table_id;
  info->column_index = column_index;
  info->root_page_id = root_page_id;
  info->tree = std::make_unique<BPlusTree>(bpm_, root_page_id);

  indexes_by_name_[index_name] = info;
  indexes_by_table_[table_id].push_back(info);
  return index_id;
}

std::vector<std::shared_ptr<IndexInfo>> Catalog::getIndexes(
    uint32_t table_id) const {
  std::lock_guard<std::mutex> guard(mutex_);
  auto it = indexes_by_table_.find(table_id);
  if (it == indexes_by_table_.end()) return {};
  return it->second;
}

std::shared_ptr<IndexInfo> Catalog::getIndex(const std::string& name) const {
  std::lock_guard<std::mutex> guard(mutex_);
  auto it = indexes_by_name_.find(name);
  if (it == indexes_by_name_.end()) return nullptr;
  return it->second;
}

std::vector<std::string> Catalog::listTables() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<std::string> names;
  for (const auto& [name, _] : tables_by_name_) {
    names.push_back(name);
  }
  return names;
}

void Catalog::persist() {
  // In a full implementation, we'd serialize all table/index metadata
  // to a catalog page.  For simplicity, metadata lives in memory and
  // is rebuilt on startup from the schema file.
  bpm_->flushAllPages();
}

void Catalog::load() {
  // No-op: tables are created via DDL on startup.
  // A full implementation would read the catalog page and rebuild
  // TableInfo / IndexInfo objects.
}

}  // namespace minidb