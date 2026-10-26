// mini-db - config.h
// Central configuration constants for the database engine.
#pragma once

#include <cstddef>
#include <cstdint>

namespace minidb {

// ---------- Storage ----------
constexpr uint32_t PAGE_SIZE = 4096;            // bytes per page on disk
constexpr uint32_t DEFAULT_POOL_SIZE = 1024;    // number of frames in buffer pool
constexpr uint32_t LRU_K = 2;                   // LRU-K "k" parameter
constexpr uint32_t INVALID_PAGE_ID = 0xFFFFFFFF;

// ---------- Transactions ----------
constexpr uint32_t INVALID_TXN_ID = 0;
constexpr uint32_t INVALID_LSN = 0;

// ---------- Index ----------
constexpr uint32_t BPLUS_ORDER = 32;             // branching factor of B+ tree

// ---------- Recovery ----------
constexpr uint32_t WAL_BUFFER_SIZE = 1 << 20;   // 1MB log buffer

}  // namespace minidb