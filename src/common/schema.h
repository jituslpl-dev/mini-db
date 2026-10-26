// mini-db - schema.h
// Column and Schema definitions.
//
// A Schema describes the layout of a tuple: the name and type of each
// column.  Schemas are immutable once created and are owned by the
// Catalog.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/types.h"

namespace minidb {

struct Column {
  std::string name;
  TypeId type;
  bool nullable{true};

  Column(std::string n, TypeId t, bool nullable_ = true)
      : name(std::move(n)), type(t), nullable(nullable_) {}
};

class Schema {
 public:
  Schema() = default;
  explicit Schema(std::vector<Column> cols) : columns_(std::move(cols)) {
    for (size_t i = 0; i < columns_.size(); ++i) {
      name_to_index_[columns_[i].name] = i;
    }
  }

  const std::vector<Column>& columns() const { return columns_; }
  uint32_t columnCount() const {
    return static_cast<uint32_t>(columns_.size());
  }
  const Column& column(uint32_t i) const { return columns_[i]; }

  // Returns the index of a column by name, or -1 if not found.
  int columnIndex(const std::string& name) const {
    auto it = name_to_index_.find(name);
    if (it == name_to_index_.end()) return -1;
    return static_cast<int>(it->second);
  }

  // Serialize / deserialize a tuple to/from a compact binary form.
  // Format: for each column, 1 byte type flag, then the data
  //   INTEGER: 8 bytes little-endian int64
  //   VARCHAR: 4 bytes length + length bytes
  std::string serializeTuple(const Tuple& tuple) const;
  Tuple deserializeTuple(const std::string& data) const;

 private:
  std::vector<Column> columns_;
  std::unordered_map<std::string, size_t> name_to_index_;
};

}  // namespace minidb