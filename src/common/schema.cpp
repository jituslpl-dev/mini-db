// mini-db - schema.cpp
#include "common/schema.h"

#include <cstring>
#include <stdexcept>

namespace minidb {

namespace {

void writeU32(std::string& out, uint32_t v) {
  out.push_back(static_cast<char>(v & 0xFF));
  out.push_back(static_cast<char>((v >> 8) & 0xFF));
  out.push_back(static_cast<char>((v >> 16) & 0xFF));
  out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

uint32_t readU32(const char* p) {
  return static_cast<uint32_t>(static_cast<uint8_t>(p[0])) |
         (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(p[3])) << 24);
}

void writeI64(std::string& out, int64_t v) {
  uint64_t u = static_cast<uint64_t>(v);
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<char>((u >> (i * 8)) & 0xFF));
  }
}

int64_t readI64(const char* p) {
  uint64_t u = 0;
  for (int i = 0; i < 8; ++i) {
    u |= static_cast<uint64_t>(static_cast<uint8_t>(p[i])) << (i * 8);
  }
  return static_cast<int64_t>(u);
}

}  // namespace

std::string Schema::serializeTuple(const Tuple& tuple) const {
  std::string out;
  for (size_t i = 0; i < columns_.size(); ++i) {
    const auto& col = columns_[i];
    const auto& val = tuple[i];
    out.push_back(static_cast<char>(val.type));
    if (val.type == TypeId::INTEGER) {
      writeI64(out, val.integer);
    } else {
      uint32_t len = static_cast<uint32_t>(val.varchar.size());
      writeU32(out, len);
      out.append(val.varchar);
    }
  }
  return out;
}

Tuple Schema::deserializeTuple(const std::string& data) const {
  Tuple tuple;
  tuple.reserve(columns_.size());
  size_t offset = 0;
  const char* p = data.data();
  for (size_t i = 0; i < columns_.size(); ++i) {
    TypeId type = static_cast<TypeId>(p[offset]);
    offset += 1;
    if (type == TypeId::INTEGER) {
      int64_t v = readI64(p + offset);
      offset += 8;
      tuple.push_back(Value::makeInteger(v));
    } else {
      uint32_t len = readU32(p + offset);
      offset += 4;
      std::string s(p + offset, len);
      offset += len;
      tuple.push_back(Value::makeVarchar(std::move(s)));
    }
  }
  return tuple;
}

}  // namespace minidb