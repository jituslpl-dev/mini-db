// mini-db - buffer_pool.cpp
#include "storage/buffer_pool.h"

#include <chrono>
#include <iostream>

namespace minidb {

BufferPoolManager::BufferPoolManager(const std::string& db_file,
                                     size_t pool_size, size_t k)
    : pool_size_(pool_size), pages_(pool_size) {
  disk_manager_ = std::make_unique<DiskManager>(db_file);
  replacer_ = std::make_unique<LRUKReplacer>(pool_size, k);

  // All frames start free.
  for (frame_id_t i = 0; i < pool_size; ++i) {
    free_list_.push_back(i);
    pages_[i].reset();
  }

  // Start background flusher thread.
  flusher_thread_ = std::thread(&BufferPoolManager::flusherLoop, this);
}

BufferPoolManager::~BufferPoolManager() {
  try {
    shutdown();
  } catch (...) { /* swallow */ }
}

Page* BufferPoolManager::fetchPage(page_id_t page_id) {
  std::lock_guard<std::mutex> guard(latch_);

  // Case 1: page already in pool.
  auto it = page_table_.find(page_id);
  if (it != page_table_.end()) {
    frame_id_t fid = it->second;
    pages_[fid].incrementPinCount();
    replacer_->recordAccess(fid);
    replacer_->setEvictable(fid, false);
    return &pages_[fid];
  }

  // Case 2: need to bring page from disk.
  Page* frame = getVictimFrame();
  if (frame == nullptr) return nullptr;

  frame->reset();
  disk_manager_->readPage(page_id, frame);
  frame->setPageId(page_id);
  frame->incrementPinCount();
  page_table_[page_id] = static_cast<frame_id_t>(frame - &pages_[0]);
  replacer_->recordAccess(static_cast<frame_id_t>(frame - &pages_[0]));
  replacer_->setEvictable(static_cast<frame_id_t>(frame - &pages_[0]), false);
  return frame;
}

Page* BufferPoolManager::newPage(page_id_t* page_id) {
  std::lock_guard<std::mutex> guard(latch_);

  *page_id = disk_manager_->allocatePage();

  Page* frame = getVictimFrame();
  if (frame == nullptr) {
    // No free frame: give the page id back.
    disk_manager_->deallocatePage(*page_id);
    return nullptr;
  }

  frame->reset();
  frame->setPageId(*page_id);
  frame->incrementPinCount();
  frame->setDirty(true);
  page_table_[*page_id] = static_cast<frame_id_t>(frame - &pages_[0]);
  replacer_->recordAccess(static_cast<frame_id_t>(frame - &pages_[0]));
  replacer_->setEvictable(static_cast<frame_id_t>(frame - &pages_[0]), false);
  return frame;
}

bool BufferPoolManager::unpinPage(page_id_t page_id, bool is_dirty) {
  std::lock_guard<std::mutex> guard(latch_);
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) return false;

  frame_id_t fid = it->second;
  if (is_dirty) pages_[fid].setDirty(true);

  pages_[fid].decrementPinCount();
  if (!pages_[fid].isPinned()) {
    replacer_->setEvictable(fid, true);
  }
  return true;
}

bool BufferPoolManager::flushPage(page_id_t page_id) {
  std::lock_guard<std::mutex> guard(latch_);
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) return false;

  frame_id_t fid = it->second;
  if (pages_[fid].isDirty()) {
    disk_manager_->writePage(&pages_[fid]);
    pages_[fid].setDirty(false);
  }
  return true;
}

void BufferPoolManager::flushAllPages() {
  std::lock_guard<std::mutex> guard(latch_);
  for (auto& [pid, fid] : page_table_) {
    if (pages_[fid].isDirty()) {
      disk_manager_->writePage(&pages_[fid]);
      pages_[fid].setDirty(false);
    }
  }
}

bool BufferPoolManager::deletePage(page_id_t page_id) {
  std::lock_guard<std::mutex> guard(latch_);
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    // Not in pool: just deallocate on disk.
    disk_manager_->deallocatePage(page_id);
    return true;
  }

  frame_id_t fid = it->second;
  if (pages_[fid].isPinned()) return false;  // cannot delete pinned page

  // Flush if dirty (so we don't leave stale data), then free.
  if (pages_[fid].isDirty()) {
    disk_manager_->writePage(&pages_[fid]);
  }
  replacer_->remove(fid);
  pages_[fid].reset();
  page_table_.erase(it);
  free_list_.push_back(fid);
  disk_manager_->deallocatePage(page_id);
  return true;
}

Page* BufferPoolManager::getVictimFrame() {
  frame_id_t fid;
  // First try free list.
  if (!free_list_.empty()) {
    fid = free_list_.front();
    free_list_.pop_front();
    return &pages_[fid];
  }

  // Evict using the replacer.
  if (!replacer_->evict(&fid)) return nullptr;

  // Write back the old page if dirty.
  page_id_t old_pid = pages_[fid].getPageId();
  if (pages_[fid].isDirty()) {
    disk_manager_->writePage(&pages_[fid]);
  }
  page_table_.erase(old_pid);
  pages_[fid].reset();
  return &pages_[fid];
}

void BufferPoolManager::flusherLoop() {
  while (true) {
    {
      std::unique_lock<std::mutex> lock(latch_);
      flusher_cv_.wait_for(lock, std::chrono::seconds(2),
                           [this] { return stop_flusher_; });
      if (stop_flusher_) return;
    }
    // Periodically flush dirty pages that are not pinned.
    std::lock_guard<std::mutex> guard(latch_);
    for (auto& [pid, fid] : page_table_) {
      if (!pages_[fid].isPinned() && pages_[fid].isDirty()) {
        disk_manager_->writePage(&pages_[fid]);
        pages_[fid].setDirty(false);
      }
    }
  }
}

void BufferPoolManager::shutdown() {
  {
    std::lock_guard<std::mutex> guard(latch_);
    if (stop_flusher_) return;  // already shut down
    stop_flusher_ = true;
  }
  flusher_cv_.notify_all();
  if (flusher_thread_.joinable()) flusher_thread_.join();

  flushAllPages();
  disk_manager_->shutdown();
}

}  // namespace minidb