// mini-db - page.h
// A fixed-size disk page.
//
// Every page on disk is PAGE_SIZE bytes.  The first 8 bytes of each page
// hold a header:
//   - page_id        (4 bytes)
//   - pin_count      (2 bytes)   -- used only while in the buffer pool
//   - is_dirty       (1 byte)
//   - page_type      (1 byte)
// The remaining bytes (PAGE_SIZE - 8) are the raw data area.
//
// Higher-level structures (heap pages, B+ tree nodes) overlay their own
// layout on top of the data area.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include "common/config.h"
#include "common/types.h"

namespace minidb {

enum class PageType : uint8_t {
  INVALID = 0,
  HEADER = 1,
  HEAP = 2,
  INDEX = 3,
};

// In-memory representation of a page frame.
struct alignas(64) Page {
  // Raw bytes stored on disk.  data_[0..7] is the page header.
  char data_[PAGE_SIZE]{};

  // Metadata that lives only in memory (not persisted).
  std::atomic<page_id_t> page_id_{INVALID_PAGE_ID};
  std::atomic<uint32_t> pin_count_{0};
  std::atomic<bool> is_dirty_{false};
  PageType page_type_{PageType::INVALID};
  // LSN for write-ahead logging (stored in the data header area too).
  std::atomic<lsn_t> lsn_{0};

  Page() = default;

  // ---- accessors ----
  page_id_t getPageId() const { return page_id_.load(); }
  void setPageId(page_id_t id) { page_id_ = id; }

  uint32_t getPinCount() const { return pin_count_.load(); }
  bool isPinned() const { return getPinCount() > 0; }
  void incrementPinCount() { pin_count_++; }
  void decrementPinCount() {
    if (pin_count_ > 0) pin_count_--;
  }

  bool isDirty() const { return is_dirty_.load(); }
  void setDirty(bool d) { is_dirty_ = d; }

  lsn_t getLSN() const { return lsn_.load(); }
  void setLSN(lsn_t l) { lsn_ = l; }

  // Raw data pointer (the whole buffer, including header).
  char* getData() { return data_; }
  const char* getData() const { return data_; }

  // The usable payload area after the 8-byte header.
  char* getPayload() { return data_ + 8; }
  const char* getPayload() const { return data_ + 8; }
  static constexpr uint32_t PAYLOAD_SIZE = PAGE_SIZE - 8;

  // Reset to a clean state (called when a frame is reused).
  void reset() {
    std::memset(data_, 0, PAGE_SIZE);
    page_id_ = INVALID_PAGE_ID;
    pin_count_ = 0;
    is_dirty_ = false;
    page_type_ = PageType::INVALID;
    lsn_ = 0;
  }
};

}  // namespace minidb