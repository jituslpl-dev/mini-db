// mini-db - disk_manager.h
// DiskManager is responsible for reading and writing fixed-size pages
// to a database file on disk.  It also tracks the next free page id for
// allocation and maintains a simple free-list for deallocated pages.
//
//   +--------+--------+--------+--------+ ...
//   | page 0 | page 1 | page 2 | page 3 |   (each PAGE_SIZE bytes)
//   +--------+--------+--------+--------+
//
// Page 0 is reserved as a header page that stores the next-page-id counter
// and the free list, so that allocation state persists across restarts.
#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <queue>
#include <string>

#include "common/config.h"
#include "common/types.h"
#include "storage/page.h"

namespace minidb {

class DiskManager {
 public:
  explicit DiskManager(const std::string& db_file);
  ~DiskManager();

  // Write a page to disk at the offset determined by its page_id.
  void writePage(Page* page);

  // Read a page from disk into the supplied Page object.
  void readPage(page_id_t page_id, Page* page);

  // Allocate a new page id.  Reuses freed pages if available, otherwise
  // extends the file.
  page_id_t allocatePage();

  // Mark a page as free so its id can be reused later.
  void deallocatePage(page_id_t page_id);

  // Flush the header page (page 0) so allocation metadata persists.
  void flushMeta();

  // Number of pages currently allocated (for diagnostics).
  uint32_t getNumPages() const { return next_page_id_; }

  // Shut down cleanly (flush meta).
  void shutdown();

 private:
  void readMeta();
  void ensureFileExists();

  std::string db_file_;
  std::fstream db_stream_;
  mutable std::mutex io_mutex_;

  page_id_t next_page_id_{1};  // page 0 reserved for metadata
  std::queue<page_id_t> free_list_;
};

}  // namespace minidb