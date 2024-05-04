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

#ifndef PMEM_MEMORY_EPOCH_BASED_GC_HPP
#define PMEM_MEMORY_EPOCH_BASED_GC_HPP

// system headers
#include <sys/stat.h>

// C++ standard libraries
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <shared_mutex>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

// external sources
#include "thread/epoch_guard.hpp"
#include "thread/epoch_manager.hpp"
#include "thread/id_manager.hpp"

// local sources
#include "pmem/memory/component/list_header.hpp"
#include "pmem/memory/utility.hpp"

namespace dbgroup::pmem::memory
{
/**
 * @brief A class to manage garbage collection.
 *
 * @tparam GCTargets Target classes of garbage collection.
 */
template <class... GCTargets>
class EpochBasedGC
{
  /*############################################################################
   * Type aliases
   *##########################################################################*/

  using Clock_t = ::std::chrono::high_resolution_clock;
  using TLSFields = component::TLSFields;

  template <class Target>
  using GarbageList = component::ListHeader<Target>;

 public:
  /*############################################################################
   * Public constructors and assignment operators
   *##########################################################################*/

  /**
   * @brief Construct a new instance.
   *
   * @param pmem_path The path to a pmemobj pool for GC.
   * @param gc_size The memory capacity for GC.
   * @param layout_name The layout name.
   * @param gc_interval_micro_sec The duration of interval for GC.
   * @param gc_thread_num The maximum number of threads to perform GC.
   */
  explicit EpochBasedGC(  //
      const std::string &pmem_path,
      const size_t gc_size = PMEMOBJ_MIN_POOL * 2,  // about 1M garbage instances
      const std::string &layout_name = "gc_on_pmem",
      const size_t gc_interval_micro_sec = kDefaultGCTime,
      const size_t gc_thread_num = kDefaultGCThreadNum)
      : gc_interval_{gc_interval_micro_sec}, gc_thread_num_{gc_thread_num}
  {
    const auto *path = pmem_path.c_str();
    const auto *layout = layout_name.c_str();
    pop_ = std::filesystem::exists(pmem_path)
               ? pmemobj_open(path, layout)
               : pmemobj_create(path, layout, gc_size + PMEMOBJ_MIN_POOL, S_IRUSR | S_IWUSR);
    if (pop_ == nullptr) {
      throw std::runtime_error{pmemobj_errormsg()};
    }

    auto &&root = pmemobj_root(pop_, sizeof(PMEMoid) * (sizeof...(GCTargets) + 1));
    root_ = reinterpret_cast<PMEMoid *>(pmemobj_direct(root));

    InitializeGarbageLists<DefaultTarget, GCTargets...>();
    cleaner_threads_.reserve(gc_thread_num_);
  }

  EpochBasedGC(const EpochBasedGC &) = delete;
  EpochBasedGC(EpochBasedGC &&) = delete;

  auto operator=(const EpochBasedGC &) -> EpochBasedGC & = delete;
  auto operator=(EpochBasedGC &&) -> EpochBasedGC & = delete;

  /*############################################################################
   * Public destructors
   *##########################################################################*/

  /**
   * @brief Destroy the instance.
   *
   * If protected garbage remains, this destructor waits for them to be free.
   */
  ~EpochBasedGC()
  {
    // stop garbage collection
    StopGC();

    if (pop_ != nullptr) {
      pmemobj_close(pop_);
    }
  }

  /*############################################################################
   * Public utility functions
   *##########################################################################*/

  /**
   * @brief Create a guard instance to protect garbage based on the scoped
   * locking pattern.
   *
   * @return A guard instance to keep the current epoch.
   */
  auto
  CreateEpochGuard()  //
      -> ::dbgroup::thread::EpochGuard
  {
    return epoch_manager_.CreateEpochGuard();
  }

  /*############################################################################
   * Public utility functions for persistent memory
   *##########################################################################*/

  /**
   * @brief Get the temporary field for memory allocation.
   *
   * @tparam Target A class for representing target garbage.
   * @param i The position of fields (0 <= i <= 12).
   * @return The address of the specified temporary field.
   */
  template <class Target = DefaultTarget>
  auto
  GetTmpField(         //
      const size_t i)  //
      -> PMEMoid *
  {
    return GetGarbageList<Target>()->GetTmpField(i);
  }

  /**
   * @brief Get the unreleased temporary fields of each thread.
   *
   * @tparam Target A class for representing target garbage.
   * @return Unreleased temporary fields if exist.
   */
  template <class Target = DefaultTarget>
  auto
  GetUnreleasedFields()  //
      -> std::vector<PMEMoid *>
  {
    return GetRemainingPMEMoids<Target, DefaultTarget, GCTargets...>();
  }

  /**
   * @brief Add a new garbage instance.
   *
   * @tparam Target A class for representing target garbage.
   * @param oid A pointer to a target garbage.
   */
  template <class Target = DefaultTarget>
  void
  AddGarbage(  //
      PMEMoid *oid)
  {
    GetGarbageList<Target>()->AddGarbage(epoch_manager_.GetCurrentEpoch(), oid);
  }

  /**
   * @brief Reuse a released memory page if it exists.
   *
   * @tparam Target A class for representing target garbage.
   * @param[out] out_oid The address to be stored a reusable page.
   */
  template <class Target = DefaultTarget>
  void
  GetPageIfPossible(  //
      PMEMoid *out_oid)
  {
    static_assert(Target::kReusePages);
    GetGarbageList<Target>()->GetPageIfPossible(out_oid);
  }

  /*############################################################################
   * Public GC control functions
   *##########################################################################*/

  /**
   * @brief Start garbage collection.
   *
   * @retval true if garbage collection has started.
   * @retval false if garbage collection is already running.
   */
  auto
  StartGC()  //
      -> bool
  {
    if (gc_is_running_.load(std::memory_order_relaxed)) return false;

    gc_is_running_.store(true, std::memory_order_relaxed);
    gc_thread_ = std::thread{&EpochBasedGC::RunGC, this};
    return true;
  }

  /**
   * @brief Stop garbage collection.
   *
   * @retval true if garbage collection has stopped.
   * @retval false if garbage collection is not running.
   */
  auto
  StopGC()  //
      -> bool
  {
    if (!gc_is_running_.load(std::memory_order_relaxed)) return false;

    gc_is_running_.store(false, std::memory_order_relaxed);
    gc_thread_.join();
    DestroyGarbageLists<DefaultTarget, GCTargets...>();
    return true;
  }

 private:
  /*############################################################################
   * Internal constants
   *##########################################################################*/

  /// @brief The expected maximum number of threads.
  static constexpr size_t kMaxThreadNum = ::dbgroup::thread::kMaxThreadNum;

  /*############################################################################
   * Internal utilities for initialization and finalization
   *##########################################################################*/

  /**
   * @brief A dummy function for creating type aliases.
   *
   */
  template <class Target, class... Tails>
  static auto
  ConvToTuple()
  {
    using ListsPtr = std::unique_ptr<GarbageList<Target>[]>;
    if constexpr (sizeof...(Tails) > 0) {
      return std::tuple_cat(std::tuple<ListsPtr>{}, ConvToTuple<Tails...>());
    } else {
      return std::tuple<ListsPtr>{};
    }
  }

  /**
   * @param addr The begin address.
   * @return The head of thread local regions.
   */
  static constexpr auto
  GetTLSHead(            //
      const void *addr)  //
      -> TLSFields *
  {
    constexpr uintptr_t kMask = kPMEMLineSize - 1;
    auto ptr = reinterpret_cast<uintptr_t>(addr);
    if ((ptr & kMask) > 0) {
      // move the address to the Intel Optane alignment
      ptr = (ptr & ~kMask) + kPMEMLineSize;
    }
    return reinterpret_cast<TLSFields *>(ptr);
  }

  /**
   * @brief Create the space for garbage lists for all the target garbage.
   *
   * @tparam Target The current class in garbage targets.
   * @tparam Tails The remaining classes in garbage targets.
   * @param pos The position of the current target in a root region.
   */
  template <class Target, class... Tails>
  void
  InitializeGarbageLists(  //
      const size_t pos = 0)
  {
    using ListsPtr = std::unique_ptr<GarbageList<Target>[]>;
    auto &lists = std::get<ListsPtr>(garbage_lists_);
    lists.reset(new GarbageList<Target>[kMaxThreadNum]);

    auto *list_oid = &(root_[pos]);
    if (OID_IS_NULL(*list_oid)) {  // the first call
      Zalloc(pop_, list_oid, sizeof(TLSFields) * (kMaxThreadNum + 1));
    }

    auto *tls_fields = GetTLSHead(pmemobj_direct(*list_oid));
    for (size_t i = 0; i < kMaxThreadNum; ++i) {
      auto *tls_field = &(tls_fields[i]);
      if (!OID_IS_NULL(tls_field->head)) {  // need recovery
        component::GarbageListInPMEM::ReleaseAllGarbages(tls_field);
      }
      lists[i].SetPMEMInfo(pop_, tls_field);
    }

    if constexpr (sizeof...(Tails) > 0) {
      InitializeGarbageLists<Tails...>(pos + 1);
    }
  }

  /**
   * @brief Destroy all the garbage lists for destruction.
   *
   * @tparam Target The current class in garbage targets.
   * @tparam Tails The remaining classes in garbage targets.
   * @param pos The position of the current target in a root region.
   */
  template <class Target, class... Tails>
  void
  DestroyGarbageLists(  //
      const size_t pos = 0)
  {
    using ListsPtr = std::unique_ptr<GarbageList<Target>[]>;
    auto &lists = std::get<ListsPtr>(garbage_lists_);
    lists.reset(nullptr);

    if constexpr (sizeof...(Tails) > 0) {
      DestroyGarbageLists<Tails...>(pos + 1);
    }
  }

  /*############################################################################
   * Additional utilities for persistent memory
   *##########################################################################*/

  /**
   * @tparam Target a class for representing a target garbage.
   * @tparam Head The current class in garbage targets.
   * @tparam Tails The remaining classes in garbage targets.
   * @param pos The position of the current target in a root region.
   * @return Unreleased temporary fields if exist.
   */
  template <class Target, class Head, class... Tails>
  auto
  GetRemainingPMEMoids(      //
      const size_t pos = 0)  //
      -> std::vector<PMEMoid *>
  {
    if constexpr (std::is_same_v<Target, Head>) {
      std::vector<PMEMoid *> list_vec{};
      list_vec.reserve(kMaxThreadNum);

      auto *tls_fields = GetTLSHead(pmemobj_direct(root_[pos]));
      for (size_t i = 0; i < kMaxThreadNum; ++i) {
        auto *tls = &(tls_fields[i]);
        auto *arr = tls->GetRemainingFields();
        if (arr == nullptr) continue;
        list_vec.emplace_back(arr);
      }
      return list_vec;
    } else {
      return GetRemainingPMEMoids<Target, Tails...>(pos + 1);
    }
  }

  /*############################################################################
   * Internal utility functions
   *##########################################################################*/

  /**
   * @tparam Target A class for representing target garbage.
   * @return The head of a linked list of garbage nodes and its mutex object.
   */
  template <class Target>
  [[nodiscard]] auto
  GetGarbageList()
  {
    using ListsPtr = std::unique_ptr<GarbageList<Target>[]>;

    return &(std::get<ListsPtr>(garbage_lists_)[::dbgroup::thread::IDManager::GetThreadID()]);
  }

  /**
   * @brief Clear registered garbage if possible.
   *
   * @tparam Target The current class in garbage targets.
   * @tparam Tails The remaining classes in garbage targets.
   * @param protected_epoch An epoch to be protected.
   */
  template <class Target, class... Tails>
  void
  ClearGarbage(const size_t protected_epoch)
  {
    using ListsPtr = std::unique_ptr<GarbageList<Target>[]>;

    auto &lists = std::get<ListsPtr>(garbage_lists_);
    for (size_t i = 0; i < kMaxThreadNum; ++i) {
      lists[i].ClearGarbage(protected_epoch);
    }

    if constexpr (sizeof...(Tails) > 0) {
      ClearGarbage<Tails...>(protected_epoch);
    }
  }

  /**
   * @brief Run a procedure of garbage collection.
   *
   */
  void
  RunGC()
  {
    // create cleaner threads
    for (size_t i = 0; i < gc_thread_num_; ++i) {
      cleaner_threads_.emplace_back([&]() {
        for (auto wake_time = Clock_t::now() + gc_interval_;  //
             gc_is_running_.load(std::memory_order_relaxed);  //
             wake_time += gc_interval_)                       //
        {
          // release unprotected garbage
          ClearGarbage<DefaultTarget, GCTargets...>(epoch_manager_.GetMinEpoch());

          // wait until the next epoch
          std::this_thread::sleep_until(wake_time);
        }
      });
    }

    // manage the global epoch
    for (auto wake_time = Clock_t::now() + gc_interval_;  //
         gc_is_running_.load(std::memory_order_relaxed);  //
         wake_time += gc_interval_)                       //
    {
      // wait until the next epoch
      std::this_thread::sleep_until(wake_time);
      epoch_manager_.ForwardGlobalEpoch();
    }

    // wait all the cleaner threads return
    for (auto &&t : cleaner_threads_) {
      t.join();
    }
    cleaner_threads_.clear();
  }

  /*############################################################################
   * Internal member variables
   *##########################################################################*/

  /// @brief The duration of garbage collection in micro seconds.
  const std::chrono::microseconds gc_interval_{};

  /// @brief The maximum number of cleaner threads
  const size_t gc_thread_num_{1};

  /// @brief An epoch manager.
  ::dbgroup::thread::EpochManager epoch_manager_{};

  /// @brief A thread to run garbage collection.
  std::thread gc_thread_{};

  /// @brief Worker threads to release garbage
  std::vector<std::thread> cleaner_threads_{};

  /// @brief A flag to check whether garbage collection is running.
  std::atomic_bool gc_is_running_{false};

  /// @brief The heads of linked lists for each GC target.
  decltype(ConvToTuple<DefaultTarget, GCTargets...>()) garbage_lists_ =
      ConvToTuple<DefaultTarget, GCTargets...>();

  /// @brief The pmemobj_pool for holding garbage lists.
  PMEMobjpool *pop_{nullptr};

  /// @brief The root object for accessing each garbage list.
  PMEMoid *root_{nullptr};
};

}  // namespace dbgroup::pmem::memory

#endif  // PMEM_MEMORY_EPOCH_BASED_GC_HPP
