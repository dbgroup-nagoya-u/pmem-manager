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

#ifndef PMEM_MEMORY_COMPONENT_GARBAGE_LIST_IN_DRAM_HPP
#define PMEM_MEMORY_COMPONENT_GARBAGE_LIST_IN_DRAM_HPP

// C++ standard libraries
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

// external system libraries
#include <libpmemobj.h>

// local sources
#include "pmem/memory/component/garbage_list_in_pmem.hpp"
#include "pmem/memory/utility.hpp"

namespace dbgroup::pmem::memory::component
{
/**
 * @brief A class to represent a buffer of garbage instances.
 *
 */
class alignas(kCacheLineSize) GarbageListInDRAM
{
 public:
  /*############################################################################
   * Public constructors and assignment operators
   *##########################################################################*/

  /**
   * @brief Construct a new instance.
   *
   */
  constexpr GarbageListInDRAM() = default;

  GarbageListInDRAM(const GarbageListInDRAM &) = delete;
  GarbageListInDRAM(GarbageListInDRAM &&) = delete;

  auto operator=(const GarbageListInDRAM &) -> GarbageListInDRAM & = delete;
  auto operator=(GarbageListInDRAM &&) -> GarbageListInDRAM & = delete;

  /*############################################################################
   * Public destructors
   *##########################################################################*/

  /**
   * @brief Destroy the instance.
   *
   */
  ~GarbageListInDRAM() = default;

  /*############################################################################
   * Public getters/setters
   *##########################################################################*/

  /**
   * @retval true if this list is empty.
   * @retval false otherwise
   */
  [[nodiscard]] auto Empty() const  //
      -> bool;

  /*############################################################################
   * Public utility functions
   *##########################################################################*/

  /**
   * @brief Add a new garbage instance to the list tail.
   *
   * @param[in,out] list_addr The address of the pointer of a target list.
   * @param[in] epoch An epoch in which garbage was added.
   * @param[in,out] garbage A new garbage instance.
   * @param[in] pop A pmemobj_pool instance for allocation.
   * @note If the list becomes full, this function creates a new list and link
   * them.
   * @note After adding garbage to the list, a given PMEMoid pointer will be
   * NULL.
   */
  static void AddGarbage(  //
      GarbageListInPMEM **list_addr,
      size_t epoch,
      PMEMoid *garbage,
      PMEMobjpool *pop);

  /**
   * @brief Reuse a destructed page.
   *
   * @param[in,out] list_addr The address of the pointer of a target list.
   * @param[out] out_page The address of a PMEMoid to store a reusable page.
   */
  static void ReusePage(  //
      GarbageListInPMEM **list_addr,
      PMEMoid *out_page);

  /**
   * @brief Destruct garbage if its epoch is less than a protected epoch.
   *
   * @param[in,out] list_oid The address of a target PMEMoid.
   * @param[in] protected_epoch A protected epoch.
   * @param[in] tmp_oid Thread local fields.
   */
  template <class T>
  static void
  Destruct(  //
      PMEMoid *list_oid,
      const size_t protected_epoch,
      PMEMoid *tmp_oid)
  {
    GarbageListInDRAM *reuse_head = nullptr;

    while (true) {
      auto *pmem = reinterpret_cast<GarbageListInPMEM *>(pmemobj_direct(*list_oid));
      auto *dram = pmem->dram;

      // destruct obsolete garbage
      const auto end_pos = dram->end_pos_.load(kAcquire);
      auto mid_pos = dram->mid_pos_.load(kRelaxed);
      for (; mid_pos < end_pos && dram->epochs_[mid_pos] < protected_epoch; ++mid_pos) {
        if constexpr (!std::is_same_v<T, void>) {
          pmem->template DestructGarbage<T>(mid_pos);
        }
      }
      dram->mid_pos_.store(mid_pos, kRelease);
      if (mid_pos < kBufferSize) break;

      // check the list can be released
      auto pos = dram->begin_pos_.load(kAcquire);
      if (pos > 0) {
        reuse_head = nullptr;
        if (pos == kBufferSize) {
          pmem = GarbageListInPMEM::ExchangeHead(pmem, list_oid, tmp_oid);
          delete dram;
          continue;
        }
      } else {
        if (reuse_head != nullptr && reuse_head->begin_pos_.load(kRelaxed) == 0) {
          auto cur = reuse_head->next_.load(kRelaxed);
          const auto next = dram->next_.load(kRelaxed);
          if ((cur & kUsed) == 0
              && reuse_head->next_.compare_exchange_strong(cur, next, kRelease, kRelaxed)) {
            for (; pos < kBufferSize; ++pos) {
              pmem->ReleaseGarbage(pos);
            }
            pmem = GarbageListInPMEM::ExchangeHead(pmem, list_oid, tmp_oid);
            delete dram;
            continue;
          }
        }
        reuse_head = dram;
      }
      list_oid = &(pmem->next);
      tmp_oid = &(pmem->tmp);
    }
  }

  /**
   * @brief Release garbage if its epoch is less than a protected epoch.
   *
   * @param[in,out] list_oid The address of a target PMEMoid.
   * @param[in] protected_epoch A protected epoch.
   * @param[in] tmp_oid Thread local fields.
   */
  template <class T>
  static void
  Clear(  //
      PMEMoid *list_oid,
      const size_t protected_epoch,
      PMEMoid *tmp_oid)
  {
    while (true) {
      auto *pmem = reinterpret_cast<GarbageListInPMEM *>(pmemobj_direct(*list_oid));
      auto *dram = pmem->dram;

      const auto mid_pos = dram->mid_pos_.load(kRelaxed);
      auto pos = dram->begin_pos_.load(kRelaxed);
      for (; pos < mid_pos; ++pos) {
        pmem->ReleaseGarbage(pos);
      }
      const auto end_pos = dram->end_pos_.load(kAcquire);
      for (; pos < end_pos && dram->epochs_[pos] < protected_epoch; ++pos) {
        if constexpr (!std::is_same_v<T, void>) {
          pmem->template DestructGarbage<T>(pos);
        }
        pmem->ReleaseGarbage(pos);
      }
      dram->begin_pos_.store(pos, kRelaxed);
      dram->mid_pos_.store(pos, kRelaxed);
      if (pos < kBufferSize) break;

      pmem = GarbageListInPMEM::ExchangeHead(pmem, list_oid, tmp_oid);
      delete dram;
    }
  }

 private:
  /*############################################################################
   * Internal constants
   *##########################################################################*/

  static constexpr auto kUsed = 1UL << 63UL;

  /*############################################################################
   * Internal utilities
   *##########################################################################*/

  /**
   * @brief Release a given garbage list and swap to the next list.
   *
   * @param[in,out] list_addr The address of the pointer of a target list.
   * @param[in] list A garbage list to be released.
   * @param[in,out] tls Thread local fields.
   */
  static void ReleaseList(  //
      PMEMoid *list_addr,
      GarbageListInDRAM *list,
      PMEMoid *tmp);

  /*############################################################################
   * Internal member variables
   *##########################################################################*/

  /// @brief The position of the first reusable page.
  std::atomic_size_t begin_pos_{0};

  /// @brief The position of the first not destructed page.
  std::atomic_size_t mid_pos_{0};

  /// @brief Epochs when each garbage is registered.
  size_t epochs_[kBufferSize]{};

  /// @brief The position of the last garbage.
  std::atomic_size_t end_pos_{0};

  /// @brief The next garbage list address for client threads.
  std::atomic_uintptr_t next_{};
};

}  // namespace dbgroup::pmem::memory::component

#endif  // PMEM_MEMORY_COMPONENT_GARBAGE_LIST_IN_DRAM_HPP
