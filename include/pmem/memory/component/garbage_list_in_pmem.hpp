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

#ifndef PMEM_MEMORY_COMPONENT_GARBAGE_LIST_IN_PMEM_HPP
#define PMEM_MEMORY_COMPONENT_GARBAGE_LIST_IN_PMEM_HPP

// C++ standard libraries
#include <cstddef>
#include <cstdint>

// external system libraries
#include <libpmemobj.h>

// local sources
#include "pmem/memory/component/tls_fields.hpp"
#include "pmem/memory/utility.hpp"

namespace dbgroup::pmem::memory::component
{
// forward declaration
class GarbageListInDRAM;

/**
 * @brief A class for representing a garbage list in persistent memory.
 *
 */
class GarbageListInPMEM
{
 public:
  /*############################################################################
   * Public constructors and assignment operators
   *##########################################################################*/

  /**
   * @brief Construct a new instance.
   *
   */
  constexpr GarbageListInPMEM() = default;

  GarbageListInPMEM(const GarbageListInPMEM &) = delete;
  GarbageListInPMEM(GarbageListInPMEM &&) = delete;

  auto operator=(const GarbageListInPMEM &) -> GarbageListInPMEM & = delete;
  auto operator=(GarbageListInPMEM &&) -> GarbageListInPMEM & = delete;

  /*############################################################################
   * Public destructor
   *##########################################################################*/

  /**
   * @brief Destroy the GarbageListInPMEM object.
   *
   */
  ~GarbageListInPMEM() = default;

  /*############################################################################
   * Public static utilities
   *##########################################################################*/

  /**
   * @brief Exchange the current head to the next one.
   *
   * @param list The current head of a garbage list.
   * @param head_addr The address of a head.
   * @param tmp_addr A temporary field for swapping.
   * @return The next head pointer.
   */
  static auto ExchangeHead(  //
      GarbageListInPMEM *list,
      PMEMoid *head_addr,
      PMEMoid *tmp_addr)  //
      -> GarbageListInPMEM *;

  /**
   * @brief Release all garbage for recovery.
   *
   * @param tls The pointer to thread-local fields.
   * @note This function does not perform any destruction for garbage.
   */
  static void ReleaseAllGarbages(  //
      TLSFields *tls);

  /*############################################################################
   * Public utilities
   *##########################################################################*/

  /**
   * @brief Add a given garbage PMEMoid to this garbage list.
   *
   * @param[in] pos The position to be added.
   * @param[in,out] garbage A PMEMoid to be reclainmed.
   * @note When this function successfully completes its process, the specified
   * PMEMoid becomes NULL.
   */
  void AddGarbage(  //
      size_t pos,
      PMEMoid *garbage);

  /**
   * @brief Reuse PMEMoid.
   *
   * @param[in] pos The position of PMEMoid to be reused.
   * @param[out] out_page The destination address to return a page.
   */
  void ReusePage(  //
      size_t pos,
      PMEMoid *out_page);

  /**
   * @brief Release a target PMEMoid.
   *
   * @param pos The position of PMEMoid to be released.
   */
  void ReleaseGarbage(  //
      size_t pos);

  /**
   * @return The next garbage list.
   */
  [[nodiscard]] auto GetNext() const  //
      -> GarbageListInPMEM *;

  /**
   * @brief Create the next garbage list and link to this.
   *
   * @param pop A pmemobj_pool instance for allocation.
   * @return The next garbage list.
   */
  auto CreateNextList(   //
      PMEMobjpool *pop)  //
      -> GarbageListInPMEM *;

  /**
   * @brief Destruct a target PMEMoid.
   *
   * @tparam T A class that has a target destruction procedure.
   * @param pos The position of PMEMoid to be destructed.
   * @note This function only performs destruction and does not free garbage.
   */
  template <class T>
  void
  DestructGarbage(  //
      const size_t pos)
  {
    auto *ptr = reinterpret_cast<T *>(pmemobj_direct(garbages_[pos]));
    ptr->~T();
  }

  /*############################################################################
   * Public member variables
   *##########################################################################*/

  GarbageListInDRAM *dram{nullptr};

  /// @brief A dummy array for alignment.
  uint64_t _dummy_for_alignment[1]{};  // NOLINT

  /// @brief The next garbage list if exist.
  PMEMoid next{};

  /// @brief A temporary region for swapping OIDs.
  PMEMoid tmp{};

 private:
  /*############################################################################
   * Internal member variables
   *##########################################################################*/

  /// @brief Offset values of garbage PMEMoids.
  PMEMoid garbages_[kBufferSize]{};
};

}  // namespace dbgroup::pmem::memory::component

#endif  // PMEM_MEMORY_COMPONENT_GARBAGE_LIST_IN_PMEM_HPP
