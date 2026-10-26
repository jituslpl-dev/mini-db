// mini-db - types.cpp
#include "common/types.h"

#include <sstream>

namespace minidb {

int Value::compare(const Value& other) const {
  if (type != other.type) {
    // Arbitrarily order integers before varchars when types differ.
    return type == TypeId::INTEGER ? -1 : 1;
  }
  if (type == TypeId::INTEGER) {
    if (integer < other.integer) return -1;
    if (integer > other.integer) return 1;
    return 0;
  }
  return varchar.compare(other.varchar);
}

std::string Value::toString() const {
  if (type == TypeId::INTEGER) return std::to_string(integer);
  return varchar;
}

std::size_t ValueHash::operator()(const Value& v) const noexcept {
  if (v.type == TypeId::INTEGER) {
    return std::hash<int64_t>{}(v.integer);
  }
  return std::hash<std::string>{}(v.varchar);
}

}  // namespace minidb