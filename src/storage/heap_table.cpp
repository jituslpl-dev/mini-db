// mini-db - heap_table.cpp
#include "storage/heap_table.h"

#include <cstring>

namespace minidb {

HeapTable::HeapTable(BufferPoolManager* bpm,
                     std::shared_ptr<Schema> schema,
                     page_id_t first_page_id)
    : bpm_(bpm),
      schema_(std::move(schema)),
      first_page_id_(first_page_id),
      last_page_id_(first_page_id) {}

HeapPageHeader HeapTable::readHeader(Page* page) {
  HeapPageHeader hdr{};
  char* p = page->getPayload();
  std::memcpy(&hdr.num_tuples, p, 4);
  std::memcpy(&hdr.next_page_id, p + 4, 4);
  return hdr;
}

void HeapTable::writeHeader(Page* page, const HeapPageHeader& hdr) {
  char* p = page->getPayload();
  std::memcpy(p, &hdr.num_tuples, 4);
  std::memcpy(p + 4, &hdr.next_page_id, 4);
}

// Slot layout: 2 bytes offset (from start of data area), 2 bytes length.
// A length of 0xFFFF means the slot is deleted/tombstoned.
static constexpr uint16_t TOMBSTONE = 0xFFFF;

static void writeSlot(Page* page, uint32_t slot_idx, uint16_t offset,
                      uint16_t length) {
  char* p = page->getPayload() + HEAP_DATA_START + slot_idx * HEAP_SLOT_SIZE;
  std::memcpy(p, &offset, 2);
  std::memcpy(p + 2, &length, 2);
}

static void readSlot(Page* page, uint32_t slot_idx, uint16_t* offset,
                     uint16_t* length) {
  char* p = page->getPayload() + HEAP_DATA_START + slot_idx * HEAP_SLOT_SIZE;
  std::memcpy(offset, p, 2);
  std::memcpy(length, p + 2, 2);
}

rid_t HeapTable::insertTuple(const Tuple& tuple) {
  std::lock_guard<std::mutex> guard(mutex_);
  std::string data = schema_->serializeTuple(tuple);
  uint32_t data_len = static_cast<uint32_t>(data.size());

  // Walk pages to find one with space.
  page_id_t cur = first_page_id_;
  while (true) {
    Page* page = bpm_->fetchPage(cur);
    auto hdr = readHeader(page);

    // Calculate free space: space between end of slot directory and the
    // start of the data area (which grows backward from end of payload).
    uint32_t slot_dir_end =
        HEAP_DATA_START + hdr.num_tuples * HEAP_SLOT_SIZE;
    uint16_t min_offset = Page::PAYLOAD_SIZE;
    for (uint32_t i = 0; i < hdr.num_tuples; ++i) {
      uint16_t off, len;
      readSlot(page, i, &off, &len);
      if (len != TOMBSTONE && off < min_offset) min_offset = off;
    }
    uint32_t data_area_used =
        (hdr.num_tuples == 0) ? 0 : (Page::PAYLOAD_SIZE - min_offset);
    uint32_t free_space =
        Page::PAYLOAD_SIZE - slot_dir_end - data_area_used;

    if (free_space >= data_len + HEAP_SLOT_SIZE) {
      // Insert here.
      uint16_t offset = static_cast<uint16_t>(Page::PAYLOAD_SIZE -
                                              data_area_used - data_len);
      std::memcpy(page->getPayload() + offset, data.data(), data_len);
      uint32_t slot = hdr.num_tuples;
      writeSlot(page, slot, offset, static_cast<uint16_t>(data_len));
      hdr.num_tuples++;
      writeHeader(page, hdr);
      page->setDirty(true);
      bpm_->unpinPage(cur, true);
      if (cur > last_page_id_) last_page_id_ = cur;
      return makeRid(cur, slot);
    }

    // No space: go to next page.
    page_id_t next = hdr.next_page_id;
    bpm_->unpinPage(cur, false);
    if (next == 0) {
      // Allocate a new page.
      page_id_t new_pid;
      Page* new_page = bpm_->newPage(&new_pid);
      HeapPageHeader new_hdr{0, 0};
      writeHeader(new_page, new_hdr);
      new_page->setDirty(true);

      // Link from old last page.
      Page* old_last = bpm_->fetchPage(cur);
      auto old_hdr = readHeader(old_last);
      old_hdr.next_page_id = new_pid;
      writeHeader(old_last, old_hdr);
      old_last->setDirty(true);
      bpm_->unpinPage(cur, true);

      // Insert into new page.
      char* payload = new_page->getPayload();
      uint16_t offset =
          static_cast<uint16_t>(Page::PAYLOAD_SIZE - data_len);
      std::memcpy(payload + offset, data.data(), data_len);
      writeSlot(new_page, 0, offset, static_cast<uint16_t>(data_len));
      HeapPageHeader nhdr{1, 0};
      writeHeader(new_page, nhdr);
      bpm_->unpinPage(new_pid, true);
      last_page_id_ = new_pid;
      return makeRid(new_pid, 0);
    }
    cur = next;
  }
}

bool HeapTable::getTuple(rid_t rid, Tuple* out) {
  page_id_t pid = ridToPage(rid);
  uint32_t slot = ridToSlot(rid);
  Page* page = bpm_->fetchPage(pid);
  auto hdr = readHeader(page);
  bool found = false;
  if (slot < hdr.num_tuples) {
    uint16_t off, len;
    readSlot(page, slot, &off, &len);
    if (len != TOMBSTONE) {
      std::string raw(page->getPayload() + off, len);
      *out = schema_->deserializeTuple(raw);
      found = true;
    }
  }
  bpm_->unpinPage(pid, false);
  return found;
}

bool HeapTable::updateTuple(rid_t rid, const Tuple& tuple) {
  std::lock_guard<std::mutex> guard(mutex_);
  page_id_t pid = ridToPage(rid);
  uint32_t slot = ridToSlot(rid);
  Page* page = bpm_->fetchPage(pid);
  auto hdr = readHeader(page);
  bool ok = false;
  if (slot < hdr.num_tuples) {
    uint16_t off, len;
    readSlot(page, slot, &off, &len);
    if (len != TOMBSTONE) {
      std::string data = schema_->serializeTuple(tuple);
      if (data.size() <= len) {
        // Fits in old slot.
        std::memcpy(page->getPayload() + off, data.data(), data.size());
        // Update length.
        writeSlot(page, slot, off, static_cast<uint16_t>(data.size()));
      } else {
        // Doesn't fit: tombstone old slot and append.
        writeSlot(page, slot, off, TOMBSTONE);
        bpm_->unpinPage(pid, true);
        // Re-insert (will find or create a page).
        insertTuple(tuple);
        bpm_->flushPage(pid);
        return true;
      }
      ok = true;
    }
  }
  bpm_->unpinPage(pid, ok);
  return ok;
}

bool HeapTable::deleteTuple(rid_t rid) {
  std::lock_guard<std::mutex> guard(mutex_);
  page_id_t pid = ridToPage(rid);
  uint32_t slot = ridToSlot(rid);
  Page* page = bpm_->fetchPage(pid);
  auto hdr = readHeader(page);
  bool ok = false;
  if (slot < hdr.num_tuples) {
    uint16_t off, len;
    readSlot(page, slot, &off, &len);
    if (len != TOMBSTONE) {
      writeSlot(page, slot, off, TOMBSTONE);
      ok = true;
    }
  }
  bpm_->unpinPage(pid, ok);
  return ok;
}

void HeapTable::scan(
    const std::function<bool(rid_t, const Tuple&)>& fn) {
  page_id_t cur = first_page_id_;
  while (cur != 0) {
    Page* page = bpm_->fetchPage(cur);
    auto hdr = readHeader(page);
    bool keep_going = true;
    for (uint32_t i = 0; i < hdr.num_tuples && keep_going; ++i) {
      uint16_t off, len;
      readSlot(page, i, &off, &len);
      if (len == TOMBSTONE) continue;
      std::string raw(page->getPayload() + off, len);
      Tuple tuple = schema_->deserializeTuple(raw);
      if (!fn(makeRid(cur, i), tuple)) {
        keep_going = false;
      }
    }
    page_id_t next = hdr.next_page_id;
    bpm_->unpinPage(cur, false);
    if (!keep_going) return;
    cur = next;
  }
}

}  // namespace minidb