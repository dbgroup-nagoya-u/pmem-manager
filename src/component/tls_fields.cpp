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
#include "pmem/memory/component/tls_fields.hpp"

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

/*##############################################################################
 * Public APIs
 *############################################################################*/
auto
TLSFields::HasSamePMEMoid(  //
    const PMEMoid &oid)     //
    -> bool
{
  for (size_t i = 0; i < kTmpFieldNum; ++i) {
    if (OID_EQUALS(tmp_oids[i], oid)) return true;
  }
  return false;
}

auto
TLSFields::GetRemainingFields()  //
    -> std::pair<bool, std::array<PMEMoid *, kTmpFieldNum>>
{
  auto has_dirty = false;
  std::array<PMEMoid *, kTmpFieldNum> oids{};
  for (size_t i = 0; i < kTmpFieldNum; ++i) {
    if (OID_IS_NULL(tmp_oids[i])) continue;
    oids[i] = &(tmp_oids[i]);
    has_dirty = true;
  }
  return {has_dirty, oids};
}

}  // namespace dbgroup::pmem::memory::component
