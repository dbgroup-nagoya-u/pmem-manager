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
#include "pmem/memory/component/garbage_list_in_dram.hpp"

// C++ standard libraries
#include <atomic>
#include <cstddef>
#include <cstdint>

// external system libraries
#include <libpmemobj.h>

// local sources
#include "pmem/memory/component/garbage_list_in_pmem.hpp"
#include "pmem/memory/utility.hpp"

namespace dbgroup::pmem::memory::component
{
/*##############################################################################
 * Public APIs
 *############################################################################*/

auto
GarbageListInDRAM::Empty() const  //
    -> bool
{
  const auto end_pos = end_pos_.load(kRelaxed);
  const auto size = end_pos - begin_pos_.load(kRelaxed);
  return (size == 0) && (end_pos < kBufferSize);
}

void
GarbageListInDRAM::AddGarbage(  //
    GarbageListInPMEM **list_addr,
    const size_t epoch,
    PMEMoid *garbage,
    PMEMobjpool *pop)
{
  auto *pmem = *list_addr;
  auto *dram = pmem->dram;

  const auto pos = dram->end_pos_.load(kRelaxed);
  dram->epochs_[pos] = epoch;
  pmem->AddGarbage(pos, garbage);
  if (pos == kBufferSize - 1) {
    auto *new_tail = pmem->CreateNextList(pop);
    dram->next_.store(reinterpret_cast<uintptr_t>(new_tail), kRelaxed);
    new_tail->dram = new GarbageListInDRAM{};
    *list_addr = new_tail;
  }
  dram->end_pos_.fetch_add(1, kRelease);
}

void
GarbageListInDRAM::ReusePage(  //
    GarbageListInPMEM **list_addr,
    PMEMoid *out_page)
{
  auto *pmem = *list_addr;
  auto *dram = pmem->dram;

  const auto pos = dram->begin_pos_.load(kRelaxed);
  const auto mid_pos = dram->mid_pos_.load(kAcquire);
  if (pos == mid_pos) return;

  pmem->ReusePage(pos, out_page);
  if (pos == kBufferSize - 1) {
    auto next = dram->next_.load(kAcquire);
    while (!dram->next_.compare_exchange_weak(next, next | kUsed, kRelaxed, kAcquire)) {
      // continue until reading a valid pointer
    }
    *list_addr = reinterpret_cast<GarbageListInPMEM *>(next);
  }
  dram->begin_pos_.fetch_add(1, kRelease);
}

}  // namespace dbgroup::pmem::memory::component
