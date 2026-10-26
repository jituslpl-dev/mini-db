// mini-db - lru_k_replacer.cpp
#include "storage/lru_k_replacer.h"

#include <algorithm>
#include <stdexcept>

#include "common/config.h"

namespace minidb {

LRUKReplacer::LRUKReplacer(size_t num_frames, size_t k)
    : num_frames_(num_frames), k_(k) {}

void LRUKReplacer::recordAccess(frame_id_t frame_id) {
  std::lock_guard<std::mutex> guard(mutex_);
  current_ts_++;

  auto& meta = frames_[frame_id];
  meta.accesses.push_back(current_ts_);
  // Keep only the last k accesses.
  if (meta.accesses.size() > k_) {
    meta.accesses.erase(meta.accesses.begin());
  }

  // If we just reached k accesses, move from history list to cache list.
  if (meta.accesses.size() == k_) {
    // Remove from history list if present.
    auto h_it = history_map_.find(frame_id);
    if (h_it != history_map_.end()) {
      history_list_.erase(h_it->second);
      history_map_.erase(h_it);
    }
    cache_list_[frame_id] = meta.accesses;
  } else if (meta.accesses.size() == 1) {
    // First access: add to history list (back = newest).
    history_list_.push_back(frame_id);
    history_map_[frame_id] = std::prev(history_list_.end());
  }
}

void LRUKReplacer::setEvictable(frame_id_t frame_id, bool evictable) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto it = frames_.find(frame_id);
  if (it == frames_.end()) return;
  it->second.evictable = evictable;
}

bool LRUKReplacer::evict(frame_id_t* victim_frame) {
  std::lock_guard<std::mutex> guard(mutex_);

  // First, try to evict from the history list (frames with < k accesses).
  // Evict the one with the oldest first-access (front of history_list_).
  for (auto it = history_list_.begin(); it != history_list_.end(); ++it) {
    frame_id_t fid = *it;
    auto& meta = frames_[fid];
    if (meta.evictable) {
      *victim_frame = fid;
      // Remove all tracking for this frame.
      history_list_.erase(it);
      history_map_.erase(fid);
      cache_list_.erase(fid);
      frames_.erase(fid);
      return true;
    }
  }

  // Otherwise, evict from the cache list: find the frame whose k-th
  // (oldest retained) access timestamp is the smallest.
  frame_id_t victim = INVALID_PAGE_ID;  // reuse as sentinel
  uint64_t oldest_kth = UINT64_MAX;
  bool found = false;
  for (auto& [fid, accesses] : cache_list_) {
    auto& meta = frames_[fid];
    if (!meta.evictable) continue;
    uint64_t kth = accesses.front();  // oldest of the last-k accesses
    if (kth < oldest_kth) {
      oldest_kth = kth;
      victim = fid;
      found = true;
    }
  }

  if (found) {
    *victim_frame = victim;
    frames_.erase(victim);
    cache_list_.erase(victim);
    return true;
  }
  return false;
}

void LRUKReplacer::remove(frame_id_t frame_id) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto h_it = history_map_.find(frame_id);
  if (h_it != history_map_.end()) {
    history_list_.erase(h_it->second);
    history_map_.erase(h_it);
  }
  cache_list_.erase(frame_id);
  frames_.erase(frame_id);
}

size_t LRUKReplacer::size() const {
  std::lock_guard<std::mutex> guard(mutex_);
  size_t cnt = 0;
  for (auto& [fid, meta] : frames_) {
    if (meta.evictable) ++cnt;
  }
  return cnt;
}

}  // namespace minidb