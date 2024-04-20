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

#ifndef PMEM_MEMORY_COMPONENT_LIST_HEADER_HPP
#define PMEM_MEMORY_COMPONENT_LIST_HEADER_HPP

// C++ standard libraries
#include <array>
#include <atomic>
#include <cassert>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <utility>

// external system libraries
#include <libpmem.h>
#include <libpmemobj.h>

// external sources
#include "thread/id_manager.hpp"

// local sources
#include "pmem/memory/component/garbage_list_in_dram.hpp"
#include "pmem/memory/component/garbage_list_in_pmem.hpp"
#include "pmem/memory/component/tls_fields.hpp"
#include "pmem/memory/utility.hpp"

namespace dbgroup::pmem::memory::component
{
/**
 * @brief A class for representing a garbage list header in volatile memory.
 *
 * @tparam Target a target class of garbage collection.
 */
template <class Target>
class alignas(kCacheLineSize) ListHeader
{
  /*############################################################################
   * Type aliases
   *##########################################################################*/

  using T = typename Target::T;
  using IDManager = ::dbgroup::thread::IDManager;

 public:
  /*############################################################################
   * Public constructors and assignment operators
   *##########################################################################*/

  /**
   * @brief Construct a new ListHeader object.
   *
   */
  constexpr ListHeader() = default;

  ListHeader(const ListHeader &) = delete;
  ListHeader(ListHeader &&) = delete;

  auto operator=(const ListHeader &) -> ListHeader & = delete;
  auto operator=(ListHeader &&) -> ListHeader & = delete;

  /*############################################################################
   * Public destructors
   *##########################################################################*/

  /**
   * @brief Destroy the ListHeader object.
   *
   * If the list contains unreleased garbage, the destructor will forcibly release it.
   */
  ~ListHeader()
  {
    if (gc_head_ != nullptr && !OID_IS_NULL(*gc_head_)) {
      GarbageListInDRAM::Clear<T>(gc_head_, std::numeric_limits<size_t>::max(), gc_tmp_);
      delete reinterpret_cast<GarbageListInPMEM *>(pmemobj_direct(*gc_head_))->dram;
      pmemobj_free(gc_head_);
    }
  }

  /*############################################################################
   * Public utilities for clients
   *##########################################################################*/

  /**
   * @brief Get the temporary field for memory allocation.
   *
   * @param i the position of fields (0 <= i <= 12).
   * @return the address of the specified temporary field.
   */
  auto
  GetTmpField(         //
      const size_t i)  //
      -> PMEMoid *
  {
    assert(i < kTmpFieldNum);

    AssignCurrentThreadIfNeeded();
    return &(tls_fields_->tmp_oids[i]);
  }

  /**
   * @brief Add a new garbage instance.
   *
   * @param epoch An epoch value when a garbage is added.
   * @param garbage_ptr a pointer to a target garbage.
   */
  void
  AddGarbage(  //
      const size_t epoch,
      PMEMoid *garbage_ptr)
  {
    AssignCurrentThreadIfNeeded();
    GarbageListInDRAM::AddGarbage(&cli_tail_, epoch, garbage_ptr, pop_);
  }

  /**
   * @brief Reuse a released memory page if it exists in the list.
   *
   * @param out_page an address to be stored a reusable page.
   */
  void
  GetPageIfPossible(  //
      PMEMoid *out_page)
  {
    AssignCurrentThreadIfNeeded();
    GarbageListInDRAM::ReusePage(&cli_head_, out_page);
  }

  /*############################################################################
   * Public utilities for garbage collection
   *##########################################################################*/

  /**
   * @param pop a pmemobj_pool instance for allocation.
   * @param tls thread-local fields.
   */
  void
  SetPMEMInfo(  //
      PMEMobjpool *pop,
      TLSFields *tls)
  {
    pop_ = pop;
    tls_fields_ = tls;
  }

  /**
   * @brief Release registered garbage if possible.
   *
   * @param protected_epoch an epoch value to check whether garbage can be freed.
   */
  void
  ClearGarbage(  //
      const size_t protected_epoch)
  {
    std::unique_lock guard{mtx_, std::defer_lock};
    if (!guard.try_lock() || gc_head_ == nullptr || OID_IS_NULL(*gc_head_)) return;

    // destruct or release garbages
    if constexpr (!Target::kReusePages) {
      GarbageListInDRAM::Clear<T>(gc_head_, protected_epoch, gc_tmp_);
    } else {
      if (!heartbeat_.expired()) {
        GarbageListInDRAM::Destruct<T>(gc_head_, protected_epoch, gc_tmp_);
      } else {
        GarbageListInDRAM::Clear<T>(gc_head_, protected_epoch, gc_tmp_);
      }
    }

    auto *dram = reinterpret_cast<GarbageListInPMEM *>(pmemobj_direct(*gc_head_))->dram;
    if (!heartbeat_.expired() || !dram->Empty()) return;

    delete dram;
    cli_tail_ = nullptr;
    cli_head_ = nullptr;
    pmemobj_free(gc_head_);
  }

 private:
  /*############################################################################
   * Internal utility functions
   *##########################################################################*/

  /**
   * @brief Assign this list to the current thread.
   *
   */
  void
  AssignCurrentThreadIfNeeded()
  {
    if (!heartbeat_.expired()) return;

    std::lock_guard guard{mtx_};
    gc_head_ = &(tls_fields_->head);
    gc_tmp_ = &(tls_fields_->tmp_head);

    if (OID_IS_NULL(*gc_head_)) {
      Zalloc(pop_, gc_head_, sizeof(GarbageListInPMEM));
    }
    cli_tail_ = reinterpret_cast<GarbageListInPMEM *>(pmemobj_direct(*gc_head_));
    cli_tail_->dram = new GarbageListInDRAM{};
    cli_head_ = cli_tail_;

    heartbeat_ = IDManager::GetHeartBeat();
  }

  /*############################################################################
   * Internal member variables
   *##########################################################################*/

  /// @brief A flag for indicating the corresponding thread has exited.
  std::weak_ptr<size_t> heartbeat_{};

  /// @brief A garbage list that has destructed pages.
  GarbageListInPMEM *cli_head_{nullptr};

  /// @brief A garbage list that has free space for new garbages.
  GarbageListInPMEM *cli_tail_{nullptr};

  /// @brief The pointer to a pmemobj_pool object.
  PMEMobjpool *pop_{nullptr};

  /// @brief The pointer to the thread local fields.
  TLSFields *tls_fields_{nullptr};

  /// @brief A dummy array for alignment.
  uint64_t dummy_for_alignment_[2]{};

  /// @brief A mutex instance for modifying buffer pointers.
  std::mutex mtx_{};

  /// @brief The head of garbage lists.
  PMEMoid *gc_head_{nullptr};

  /// @brief A temporary region for swapping PMEMoids.
  PMEMoid *gc_tmp_{nullptr};
};

}  // namespace dbgroup::pmem::memory::component

#endif  // PMEM_MEMORY_COMPONENT_LIST_HEADER_HPP
