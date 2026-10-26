// mini-db - heap_table.h
//
// HeapTable stores tuples in a linked list of heap pages.
//
// Heap page layout (within the page payload):
//   bytes 0..3    : num_tuples (uint32)
//   bytes 4..7    : next_page_id (uint32, 0 = no next page)
//   bytes 8..     : slot directory, each slot:
//                     2 bytes offset, 2 bytes length
//                  followed by tuple data growing from the end.
//
// A HeapTable tracks the first page id and the last page id (for fast
// appends).  All access goes through the BufferPoolManager.
//
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/schema.h"
#include "common/types.h"
#include "storage/buffer_pool.h"
#include "storage/page.h"

namespace minidb {

// A RowId (rid) encodes the page id in the upper 32 bits and the slot
// number in the lower 32 bits.
inline rid_t makeRid(page_id_t page, uint32_t slot) {
  return (static_cast<uint64_t>(page) << 32) | slot;
}
inline page_id_t ridToPage(rid_t rid) { return static_cast<page_id_t>(rid >> 32); }
inline uint32_t ridToSlot(rid_t rid) { return static_cast<uint32_t>(rid & 0xFFFFFFFF); }

struct HeapPageHeader {
  uint32_t num_tuples;
  uint32_t next_page_id;
};

static constexpr uint32_t HEAP_SLOT_SIZE = 4;  // 2 bytes offset + 2 bytes length
static constexpr uint32_t HEAP_DATA_START = sizeof(HeapPageHeader);

class HeapTable {
 public:
  HeapTable(BufferPoolManager* bpm, std::shared_ptr<Schema> schema,
            page_id_t first_page_id);

  // Insert a tuple.  Returns the rid.
  rid_t insertTuple(const Tuple& tuple);

  // Get a tuple by rid.  Returns true on success.
  bool getTuple(rid_t rid, Tuple* out);

  // Update the tuple at the given rid.
  bool updateTuple(rid_t rid, const Tuple& tuple);

  // Delete the tuple at the given rid (marks slot as deleted).
  bool deleteTuple(rid_t rid);

  // Iterate all live tuples.  Calls fn(rid, tuple) for each.
  // Returns false if fn returns false (stop iteration).
  void scan(const std::function<bool(rid_t, const Tuple&)>& fn);

  page_id_t getFirstPageId() const { return first_page_id_; }

 private:
  // Read/Write the heap page header.
  static HeapPageHeader readHeader(Page* page);
  static void writeHeader(Page* page, const HeapPageHeader& hdr);

  BufferPoolManager* bpm_;
  std::shared_ptr<Schema> schema_;
  page_id_t first_page_id_;
  page_id_t last_page_id_;
  mutable std::mutex mutex_;
};

}  // namespace minidb