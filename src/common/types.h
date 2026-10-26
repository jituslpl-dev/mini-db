// mini-db - types.h
// Common type aliases used across the engine.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace minidb {

using page_id_t = uint32_t;
using frame_id_t = uint32_t;
using txn_id_t = uint32_t;
using lsn_t = uint32_t;
using rid_t = uint64_t;   // row id

// Isolation levels for transactions.
enum class IsolationLevel {
  READ_UNCOMMITTED,
  READ_COMMITTED,
  REPEATABLE_READ,
  SERIALIZABLE
};

// A value stored in a tuple cell.  We keep this deliberately simple:
// everything is either an INTEGER (int64) or a TEXT (string).
enum class TypeId : uint8_t { INTEGER, VARCHAR };

struct Value {
  TypeId type;
  int64_t integer = 0;
  std::string varchar;

  static Value makeInteger(int64_t v) {
    Value x;
    x.type = TypeId::INTEGER;
    x.integer = v;
    return x;
  }
  static Value makeVarchar(std::string v) {
    Value x;
    x.type = TypeId::VARCHAR;
    x.varchar = std::move(v);
    return x;
  }

  // Comparison helpers.
  int compare(const Value& other) const;
  bool operator==(const Value& other) const { return compare(other) == 0; }
  bool operator<(const Value& other) const { return compare(other) < 0; }
  bool operator>(const Value& other) const { return compare(other) > 0; }
  bool operator<=(const Value& other) const { return compare(other) <= 0; }
  bool operator>=(const Value& other) const { return compare(other) >= 0; }

  std::string toString() const;
};

// A tuple is a vector of values.
using Tuple = std::vector<Value>;

// Hash of a Value for hash-join / hash-table purposes.
struct ValueHash {
  std::size_t operator()(const Value& v) const noexcept;
};

}  // namespace minidb