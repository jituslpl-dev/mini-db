// mini-db - lru_k_replacer.h
//
// LRU-K page replacement.
//
// The LRU-K algorithm evicts the frame whose K-th most recent access is
// the oldest.  Frames that have been accessed fewer than K times are kept
// in a separate "history" list and are preferred for eviction over any
// frame that has K accesses (because a frame with < K accesses is
// considered "cold").
//
// References:
//   O'Neil et al., "The LRU-K Page Replacement Algorithm", SIGMOD 1993.
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/types.h"

namespace minidb {

class LRUKReplacer {
 public:
  // k is the history parameter; num_frames is the capacity.
  LRUKReplacer(size_t num_frames, size_t k);

  // Record that frame `frame_id` was accessed at the current timestamp.
  void recordAccess(frame_id_t frame_id);

  // Mark a frame as evictable (or not).  Only evictable frames are
  // candidates for eviction.
  void setEvictable(frame_id_t frame_id, bool evictable);

  // Choose a victim frame and remove it.  Returns false if no frame
  // can be evicted.
  bool evict(frame_id_t* victim_frame);

  // Remove all tracking for a frame (called when a page is deleted).
  void remove(frame_id_t frame_id);

  size_t size() const;

 private:
  struct FrameMeta {
    std::vector<uint64_t> accesses;  // last K access timestamps (oldest first)
    bool evictable{false};
  };

  size_t num_frames_;
  size_t k_;
  uint64_t current_ts_{0};

  mutable std::mutex mutex_;
  std::unordered_map<frame_id_t, FrameMeta> frames_;
  // List of frames with < k accesses (history list), sorted by first access.
  std::list<frame_id_t> history_list_;
  std::unordered_map<frame_id_t, std::list<frame_id_t>::iterator>
      history_map_;
  // Set of frames with >= k accesses, for quick lookup.
  std::unordered_map<frame_id_t, std::vector<uint64_t>> cache_list_;
};

}  // namespace minidb