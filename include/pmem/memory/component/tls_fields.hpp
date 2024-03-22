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

#ifndef PMEM_MEMORY_COMPONENT_TLS_FIELDS_HPP
#define PMEM_MEMORY_COMPONENT_TLS_FIELDS_HPP

// C++ standard libraries
#include <array>
#include <cstddef>
#include <utility>

// external system libraries
#include <libpmemobj.h>

// local sources
#include "pmem/memory/utility.hpp"

namespace dbgroup::pmem::memory::component
{
/**
 * @brief A struct for holding thread-local PMEMoid instances.
 *
 */
struct TLSFields {
  /*############################################################################
   * Public member variables
   *##########################################################################*/

  /// @brief Temporary fields to ensure fault tolerance of user-defined data.
  PMEMoid tmp_oids[kTmpFieldNum]{};

  /// @brief The head pointer of garbage lists.
  PMEMoid head{};

  /// @brief A temporary field to swap head pointers.
  PMEMoid tmp_head{};

  /*############################################################################
   * Public utilities
   *##########################################################################*/

  /**
   * @param oid A target PMEMoid instance.
   * @retval true if there is the same PMEMoid with a given one.
   * @retval false otherwise.
   */
  auto HasSamePMEMoid(     //
      const PMEMoid &oid)  //
      -> bool;

  /**
   * @retval 1st: true if temporary fields have not released PMEMoid.
   * @retval 1st: false otherwise.
   * @retval 2nd: Addresses of each temporary field.
   */
  auto GetRemainingFields()  //
      -> std::pair<bool, std::array<PMEMoid *, kTmpFieldNum>>;
};

}  // namespace dbgroup::pmem::memory::component

#endif  // PMEM_MEMORY_COMPONENT_TLS_FIELDS_HPP
