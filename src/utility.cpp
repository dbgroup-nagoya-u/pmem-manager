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

// the corresponding header
#include "pmem/memory/utility.hpp"

// C++ standard libraries
#include <cstddef>
#include <stdexcept>

// external system libraries
#include <libpmemobj.h>

namespace dbgroup::pmem::memory
{
/*##############################################################################
 * Utility functions
 *############################################################################*/

void
Malloc(  //
    PMEMobjpool *pop,
    PMEMoid *oid,
    const size_t size)
{
  if (pmemobj_alloc(pop, oid, size, kPMDKNullType, nullptr, nullptr) != 0) {
    throw std::runtime_error{pmemobj_errormsg()};
  }
}

void
Zalloc(  //
    PMEMobjpool *pop,
    PMEMoid *oid,
    const size_t size)
{
  if (pmemobj_zalloc(pop, oid, size, kPMDKNullType) != 0) {
    throw std::runtime_error{pmemobj_errormsg()};
  }
}

}  // namespace dbgroup::pmem::memory
