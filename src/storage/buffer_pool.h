// mini-db - buffer_pool.h
//
// BufferPoolManager
//
// Manages a pool of in-memory page frames.  When a page is requested the
// manager first checks the page table (page_id -> frame_id).  If the page
// is not in memory it is fetched from disk, evicting another page if
// necessary using the LRU-K replacer.  Dirty pages are written back to
// disk on eviction or on explicit flush.
//
//   +-------------+        +----------+        +-----------+
//   | DiskManager | <----> | Buffer   | <----> | Replacer  |
//   |   (file)    |        | Pool     |        | (LRU-K)   |
//   +-------------+        +----------+        +-----------+
//
// All operations are thread-safe.
//
#pragma once

#include <condition_variable>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "common/config.h"
#include "common/types.h"
#include "storage/disk_manager.h"
#include "storage/lru_k_replacer.h"
#include "storage/page.h"

namespace minidb {

class BufferPoolManager {
 public:
  BufferPoolManager(const std::string& db_file, size_t pool_size = DEFAULT_POOL_SIZE,
                    size_t k = LRU_K);
  ~BufferPoolManager();

  // Fetch a page by id.  The returned page has pin_count incremented.
  // Returns nullptr if the page cannot be fetched (e.g. all frames pinned).
  Page* fetchPage(page_id_t page_id);

  // Create a new page (allocates a page id on disk).  The returned page
  // is pinned and marked dirty.
  Page* newPage(page_id_t* page_id);

  // Unpin a page.  If is_dirty is true the page is marked dirty.
  // If pin_count reaches 0 the frame becomes evictable.
  bool unpinPage(page_id_t page_id, bool is_dirty);

  // Flush a single page to disk (if dirty).
  bool flushPage(page_id_t page_id);

  // Flush all dirty pages to disk.
  void flushAllPages();

  // Delete a page: flush if dirty, free the frame, and deallocate the
  // page id on disk.
  bool deletePage(page_id_t page_id);

  // Shut down: flush all pages, stop background flusher.
  void shutdown();

  size_t getPoolSize() const { return pool_size_; }

 private:
  // Find a free frame, evicting if necessary.  Returns nullptr if no
  // frame is available.
  Page* getVictimFrame();

  // Background async flush thread.
  void flusherLoop();

  size_t pool_size_;
  std::vector<Page> pages_;  // the frame array

  std::unordered_map<page_id_t, frame_id_t> page_table_;
  std::list<frame_id_t> free_list_;

  std::unique_ptr<DiskManager> disk_manager_;
  std::unique_ptr<LRUKReplacer> replacer_;

  mutable std::mutex latch_;
  std::thread flusher_thread_;
  std::condition_variable flusher_cv_;
  bool stop_flusher_{false};
};

}  // namespace minidb