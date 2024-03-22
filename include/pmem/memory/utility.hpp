/*
 * Copyright 2024 Database Group, Nagoya University
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef PMEM_MEMORY_UTILITY_HPP
#define PMEM_MEMORY_UTILITY_HPP

// C++ standard libraries
#include <atomic>
#include <cstddef>
#include <cstdint>

// external system libraries
#include <libpmemobj.h>

namespace dbgroup::pmem::memory
{
/*##############################################################################
 * Global constants
 *############################################################################*/

/// @brief The default time interval for garbage collection [us].
constexpr size_t kDefaultGCTime = 100000;

/// @brief The default number of worker threads for garbage collection.
constexpr size_t kDefaultGCThreadNum = 1;

/// @brief The size of words.
constexpr size_t kWordSize = 8;

/// @brief The expected cache-line size.
constexpr size_t kCacheLineSize = 64;

/// @brief The number of temporary fields per thread.
constexpr size_t kTmpFieldNum = 13;

/// @brief The number of garbage in each list.
constexpr size_t kBufferSize = 252;

/// @brief An alias of the acquire memory order.
constexpr std::memory_order kAcquire = std::memory_order_acquire;

/// @brief An alias of the release memory order.
constexpr std::memory_order kRelease = std::memory_order_release;

/// @brief An alias of the relaxed memory order.
constexpr std::memory_order kRelaxed = std::memory_order_relaxed;

/*##############################################################################
 * Constants for PMDK
 *############################################################################*/

/// @brief The default alignment size for dynamically allocated instances.
constexpr size_t kPMDKHeaderSize = 16;

/// @brief We do not use type checks in PMDK.
constexpr uint64_t kPMDKNullType = 0;

/*##############################################################################
 * Utility classes
 *############################################################################*/

/**
 * @brief A default GC information.
 *
 */
struct DefaultTarget {
  /// @brief Use the void type and do not perform destructors.
  using T = void;

  /// @brief Do not reuse pages after GC (release immediately).
  static constexpr bool kReusePages = false;
};

/*##############################################################################
 * Utility functions
 *############################################################################*/

/**
 * @brief Allocate a region of persistent memory using a given pool.
 * *
 * @param[in] pop A pmemobj pool instance.
 * @param[out] oid A PMEMoid to store an allocated region.
 * @param[in] size The desired size of allocation.
 */
void Malloc(  //
    PMEMobjpool *pop,
    PMEMoid *oid,
    const size_t size);

/**
 * @brief Allocate a region of persistent memory using a given pool.
 *
 * @param[in] pop A pmemobj pool instance.
 * @param[out] oid A PMEMoid to store an allocated region.
 * @param[in] size The desired size of allocation.
 */
void Zalloc(  //
    PMEMobjpool *pop,
    PMEMoid *oid,
    const size_t size);

}  // namespace dbgroup::pmem::memory

#endif  // PMEM_MEMORY_UTILITY_HPP
