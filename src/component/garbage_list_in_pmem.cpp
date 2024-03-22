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
#include "pmem/memory/component/garbage_list_in_pmem.hpp"

// C++ standard libraries
#include <cstddef>
#include <cstdint>

// external system libraries
#include <libpmem.h>
#include <libpmemobj.h>

// local sources
#include "pmem/memory/component/tls_fields.hpp"
#include "pmem/memory/utility.hpp"

namespace
{
/*##############################################################################
 * Local Constants
 *############################################################################*/

/// @brief The zero acts as nullptr.
constexpr uint64_t kNullOffset = 0;

}  // namespace

namespace dbgroup::pmem::memory::component
{
/*##############################################################################
 * Public APIs
 *############################################################################*/

auto
GarbageListInPMEM::ExchangeHead(  //
    GarbageListInPMEM *list,
    PMEMoid *head_addr,
    PMEMoid *tmp_addr)  //
    -> GarbageListInPMEM *
{
  *tmp_addr = *head_addr;
  head_addr->off = list->next.off;
  pmem_persist(head_addr, 2 * sizeof(PMEMoid));  // in the same cache line

  pmemobj_free(tmp_addr);
  return static_cast<GarbageListInPMEM *>(pmemobj_direct(*head_addr));
}

void
GarbageListInPMEM::ReleaseAllGarbages(  //
    TLSFields *tls)
{
  if (OID_IS_NULL(tls->head)) return;
  if (!OID_IS_NULL(tls->tmp_head)) {
    if (OID_EQUALS(tls->tmp_head, tls->head)) {
      tls->tmp_head = OID_NULL;
      pmem_persist(&(tls->tmp_head), sizeof(PMEMoid));
    } else {
      pmemobj_free(&(tls->tmp_head));
    }
  }

  auto *buf = reinterpret_cast<GarbageListInPMEM *>(pmemobj_direct(tls->head));
  while (true) {
    if (!OID_IS_NULL(buf->tmp)) {
      if (OID_EQUALS(buf->tmp, buf->next)) {
        buf->tmp = OID_NULL;
        pmem_persist(&(buf->tmp), sizeof(PMEMoid));
      } else {
        pmemobj_free(&(buf->tmp));
      }
    }
    for (size_t i = 0; i < kBufferSize; ++i) {
      auto *oid = &(buf->garbages_[i]);
      if (OID_IS_NULL(*oid) || tls->HasSamePMEMoid(*oid)) continue;
      pmemobj_free(oid);
    }
    if (OID_IS_NULL(buf->next)) break;
    buf = ExchangeHead(buf, &(tls->head), &(tls->tmp_head));
  }
  pmemobj_free(&(tls->head));
}

void
GarbageListInPMEM::AddGarbage(  //
    const size_t pos,
    PMEMoid *garbage)
{
  garbages_[pos].pool_uuid_lo = garbage->pool_uuid_lo;
  garbages_[pos].off = garbage->off;
  pmem_persist(&garbages_[pos], sizeof(PMEMoid));

  garbage->off = kNullOffset;
  pmem_persist(&(garbage->off), kWordSize);
}

void
GarbageListInPMEM::ReusePage(  //
    const size_t pos,
    PMEMoid *out_page)
{
  out_page->pool_uuid_lo = garbages_[pos].pool_uuid_lo;
  out_page->off = garbages_[pos].off;
  pmem_persist(out_page, sizeof(PMEMoid));

  garbages_[pos].off = kNullOffset;
  pmem_persist(&(garbages_[pos].off), kWordSize);
}

void
GarbageListInPMEM::ReleaseGarbage(  //
    const size_t pos)
{
  pmemobj_free(&(garbages_[pos]));
}

auto
GarbageListInPMEM::GetNext() const  //
    -> GarbageListInPMEM *
{
  return reinterpret_cast<GarbageListInPMEM *>(pmemobj_direct(next));
}

auto
GarbageListInPMEM::CreateNextList(  //
    PMEMobjpool *pop)               //
    -> GarbageListInPMEM *
{
  Zalloc(pop, &next, sizeof(GarbageListInPMEM));
  return GetNext();
}

/*##############################################################################
 * Static assertions
 *############################################################################*/

static_assert((kPMDKHeaderSize + sizeof(GarbageListInPMEM)) % kCacheLineSize == 0);

}  // namespace dbgroup::pmem::memory::component
