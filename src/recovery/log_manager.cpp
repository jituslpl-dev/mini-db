// mini-db - log_manager.cpp
#include "recovery/log_manager.h"

#include <cstring>
#include <iostream>

namespace minidb {

namespace {

void appendU32(std::string& out, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    out.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
}

uint32_t readU32(const char* p) {
  return static_cast<uint32_t>(static_cast<uint8_t>(p[0])) |
         (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(p[3])) << 24);
}

void appendU64(std::string& out, uint64_t v) {
  for (int i = 0; i < 8; ++i)
    out.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
}

uint64_t readU64(const char* p) {
  uint64_t u = 0;
  for (int i = 0; i < 8; ++i)
    u |= static_cast<uint64_t>(static_cast<uint8_t>(p[i])) << (i * 8);
  return u;
}

}  // namespace

std::string LogRecord::serialize() const {
  std::string out;
  // Payload: lsn(4) + txn_id(4) + type(1) + table_id(4) + rid(8)
  //          + old_data_len(4) + old_data + new_data_len(4) + new_data
  std::string payload;
  appendU32(payload, lsn);
  appendU32(payload, txn_id);
  payload.push_back(static_cast<char>(type));
  appendU32(payload, table_id);
  appendU64(payload, rid);
  appendU32(payload, static_cast<uint32_t>(old_data.size()));
  payload.append(old_data);
  appendU32(payload, static_cast<uint32_t>(new_data.size()));
  payload.append(new_data);

  // Total record = 4-byte length prefix + payload.
  appendU32(out, static_cast<uint32_t>(payload.size()));
  out.append(payload);
  return out;
}

bool LogRecord::deserialize(const std::string& buf, size_t* offset,
                            LogRecord* out) {
  if (*offset + 4 > buf.size()) return false;
  const char* p = buf.data() + *offset;
  uint32_t payload_len = readU32(p);
  *offset += 4;

  if (*offset + payload_len > buf.size()) return false;
  const char* payload = buf.data() + *offset;
  size_t off = 0;

  out->lsn = readU32(payload + off); off += 4;
  out->txn_id = readU32(payload + off); off += 4;
  out->type = static_cast<LogRecordType>(payload[off]); off += 1;
  out->table_id = readU32(payload + off); off += 4;
  out->rid = readU64(payload + off); off += 8;

  uint32_t old_len = readU32(payload + off); off += 4;
  out->old_data.assign(payload + off, old_len); off += old_len;

  uint32_t new_len = readU32(payload + off); off += 4;
  out->new_data.assign(payload + off, new_len); off += new_len;

  *offset += payload_len;
  return true;
}

std::string LogRecord::toString() const {
  std::string type_str;
  switch (type) {
    case LogRecordType::BEGIN: type_str = "BEGIN"; break;
    case LogRecordType::COMMIT: type_str = "COMMIT"; break;
    case LogRecordType::ABORT: type_str = "ABORT"; break;
    case LogRecordType::INSERT: type_str = "INSERT"; break;
    case LogRecordType::DELETE_TUPLE: type_str = "DELETE"; break;
    case LogRecordType::UPDATE: type_str = "UPDATE"; break;
    case LogRecordType::CHECKPOINT: type_str = "CHECKPOINT"; break;
    case LogRecordType::NEW_PAGE: type_str = "NEW_PAGE"; break;
  }
  return "LSN=" + std::to_string(lsn) + " TXN=" + std::to_string(txn_id) +
         " " + type_str;
}

// ---- LogManager ----
LogManager::LogManager(const std::string& log_file)
    : log_file_(log_file) {
  // Open in append mode; create if not exists.
  log_stream_.open(log_file_,
                   std::ios::in | std::ios::out | std::ios::binary |
                       std::ios::app);
  if (!log_stream_.is_open()) {
    // Create.
    log_stream_.open(log_file_, std::ios::out | std::ios::binary | std::ios::trunc);
    log_stream_.close();
    log_stream_.open(log_file_,
                     std::ios::in | std::ios::out | std::ios::binary |
                         std::ios::app);
  }

  // Determine next LSN by reading existing records.
  auto records = readAllRecords();
  for (const auto& r : records) {
    if (r.lsn >= next_lsn_.load()) next_lsn_.store(r.lsn + 1);
    if (r.lsn > flushed_lsn_.load()) flushed_lsn_.store(r.lsn);
  }
}

LogManager::~LogManager() {
  try {
    flush();
    if (log_stream_.is_open()) log_stream_.close();
  } catch (...) {}
}

lsn_t LogManager::appendRecord(const LogRecord& rec) {
  std::lock_guard<std::mutex> guard(mutex_);
  LogRecord r = rec;
  r.lsn = next_lsn_.fetch_add(1);
  buffer_ += r.serialize();

  // If buffer is large, flush.
  if (buffer_.size() >= WAL_BUFFER_SIZE) {
    flushBuffer();
  }
  return r.lsn;
}

void LogManager::flush(lsn_t flush_lsn) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (flush_lsn == 0 || flush_lsn >= next_lsn_.load()) {
    flushBuffer();
  }
}

void LogManager::flushBuffer() {
  if (buffer_.empty()) return;
  log_stream_.clear();
  log_stream_.seekp(0, std::ios::end);
  log_stream_.write(buffer_.data(), buffer_.size());
  log_stream_.flush();
  buffer_.clear();
  flushed_lsn_.store(next_lsn_.load() - 1);
}

std::vector<LogRecord> LogManager::readAllRecords() {
  std::lock_guard<std::mutex> guard(mutex_);
  // Flush buffer first so file is up to date.
  flushBuffer();

  std::ifstream in(log_file_, std::ios::binary);
  std::vector<LogRecord> records;
  std::string content((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());

  size_t offset = 0;
  while (offset < content.size()) {
    LogRecord rec;
    if (!LogRecord::deserialize(content, &offset, &rec)) break;
    records.push_back(rec);
  }
  return records;
}

void LogManager::truncate() {
  std::lock_guard<std::mutex> guard(mutex_);
  flushBuffer();
  log_stream_.close();
  log_stream_.open(log_file_, std::ios::out | std::ios::binary |
                                   std::ios::trunc);
  log_stream_.close();
  log_stream_.open(log_file_, std::ios::in | std::ios::out |
                                   std::ios::binary | std::ios::app);
  next_lsn_.store(1);
  flushed_lsn_.store(0);
}

}  // namespace minidb