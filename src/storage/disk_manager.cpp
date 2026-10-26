// mini-db - disk_manager.cpp
#include "storage/disk_manager.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <vector>

namespace minidb {

// ---- On-disk metadata layout (page 0) ----
//   bytes 0..3   : next_page_id
//   bytes 4..7   : free_list_size
//   bytes 8..    : free_list entries (each 4 bytes)
struct MetaHeader {
  uint32_t next_page_id;
  uint32_t free_list_size;
};

DiskManager::DiskManager(const std::string& db_file) : db_file_(db_file) {
  ensureFileExists();
  readMeta();
}

DiskManager::~DiskManager() {
  try {
    flushMeta();
    if (db_stream_.is_open()) db_stream_.close();
  } catch (...) { /* swallow in destructor */ }
}

void DiskManager::ensureFileExists() {
  db_stream_.open(db_file_, std::ios::in | std::ios::out | std::ios::binary);
  if (!db_stream_.is_open()) {
    // Create the file.
    db_stream_.open(db_file_, std::ios::in | std::ios::out | std::ios::binary |
                                  std::ios::trunc);
    db_stream_.close();
    db_stream_.open(db_file_, std::ios::in | std::ios::out | std::ios::binary);
  }
}

void DiskManager::readMeta() {
  // Read page 0 if the file already has data.
  db_stream_.seekg(0, std::ios::end);
  auto size = db_stream_.tellg();
  if (size < static_cast<std::streamoff>(PAGE_SIZE)) {
    // Fresh database: write an empty metadata page.
    next_page_id_ = 1;
    flushMeta();
    return;
  }

  char buf[PAGE_SIZE];
  db_stream_.seekg(0, std::ios::beg);
  db_stream_.read(buf, PAGE_SIZE);
  if (!db_stream_) {
    next_page_id_ = 1;
    return;
  }

  MetaHeader hdr;
  std::memcpy(&hdr, buf, sizeof(MetaHeader));
  next_page_id_ = hdr.next_page_id;
  free_list_ = {};
  for (uint32_t i = 0; i < hdr.free_list_size; ++i) {
    page_id_t pid;
    std::memcpy(&pid, buf + sizeof(MetaHeader) + i * sizeof(page_id_t),
                sizeof(page_id_t));
    free_list_.push(pid);
  }
}

void DiskManager::flushMeta() {
  std::lock_guard<std::mutex> guard(io_mutex_);
  char buf[PAGE_SIZE] = {0};
  MetaHeader hdr;
  hdr.next_page_id = next_page_id_;
  hdr.free_list_size = static_cast<uint32_t>(free_list_.size());
  std::memcpy(buf, &hdr, sizeof(MetaHeader));

  // Copy free list.
  auto temp = free_list_;
  uint32_t offset = sizeof(MetaHeader);
  while (!temp.empty()) {
    page_id_t pid = temp.front();
    temp.pop();
    std::memcpy(buf + offset, &pid, sizeof(page_id_t));
    offset += sizeof(page_id_t);
  }

  db_stream_.clear();
  db_stream_.seekp(0, std::ios::beg);
  db_stream_.write(buf, PAGE_SIZE);
  db_stream_.flush();
}

void DiskManager::writePage(Page* page) {
  std::lock_guard<std::mutex> guard(io_mutex_);
  page_id_t pid = page->getPageId();
  std::streamoff offset = static_cast<std::streamoff>(pid) * PAGE_SIZE;
  db_stream_.clear();
  db_stream_.seekp(offset, std::ios::beg);
  db_stream_.write(page->getData(), PAGE_SIZE);
  // We do NOT flush every write for performance; the buffer pool
  // caller or shutdown() handles flushing.
}

void DiskManager::readPage(page_id_t page_id, Page* page) {
  std::lock_guard<std::mutex> guard(io_mutex_);
  std::streamoff offset = static_cast<std::streamoff>(page_id) * PAGE_SIZE;
  db_stream_.clear();
  db_stream_.seekg(offset, std::ios::beg);
  db_stream_.read(page->getData(), PAGE_SIZE);
  if (!db_stream_) {
    // Page beyond EOF: zero it out (new page).
    std::memset(page->getData(), 0, PAGE_SIZE);
    db_stream_.clear();
  }
  page->setPageId(page_id);
}

page_id_t DiskManager::allocatePage() {
  std::lock_guard<std::mutex> guard(io_mutex_);
  if (!free_list_.empty()) {
    page_id_t pid = free_list_.front();
    free_list_.pop();
    return pid;
  }
  return next_page_id_++;
}

void DiskManager::deallocatePage(page_id_t page_id) {
  std::lock_guard<std::mutex> guard(io_mutex_);
  free_list_.push(page_id);
}

void DiskManager::shutdown() {
  flushMeta();
  if (db_stream_.is_open()) db_stream_.close();
}

}  // namespace minidb