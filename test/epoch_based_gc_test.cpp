/*
 * Copyright 2021 Database Group, Nagoya University
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
#include "pmem/memory/epoch_based_gc.hpp"

// C++ standard libraries
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

// external libraries
#include "gtest/gtest.h"

// local sources
#include "pmem_env.hpp"

namespace dbgroup::pmem::memory::test
{
// prepare a temporary directory
auto *const env = testing::AddGlobalTestEnvironment(new TmpDirManager);

/*##############################################################################
 * Global constants
 *############################################################################*/

constexpr size_t kThreadNum = DBGROUP_TEST_THREAD_NUM;
constexpr size_t kSize = PMEMOBJ_MIN_POOL * 2 * kThreadNum;
constexpr const char *kPoolName = "memory_manager_epoch_based_gc_on_pmem_test";
constexpr const char *kGCName = "memory_manager_epoch_based_gc_on_pmem_test_gc";
constexpr const char *kLayout = "gc_on_pmem_test";
constexpr auto kModeRW = S_IRUSR | S_IWUSR;  // NOLINT

/*##############################################################################
 * Global type aliases
 *############################################################################*/

using Target = uint64_t;

class EpochBasedGCFixture : public ::testing::Test
{
 protected:
  /*##############################################################################
   * Internal classes
   *############################################################################*/

  struct SharedPtrTarget : public DefaultTarget {
    using T = std::shared_ptr<Target>;
    static constexpr bool kReusePages = true;
  };

  /*############################################################################
   * Type aliases
   *##########################################################################*/

  using EpochBasedGC_t = EpochBasedGC<SharedPtrTarget>;
  using GarbageRef = std::vector<std::weak_ptr<Target>>;

  /*############################################################################
   * Internal constants
   *##########################################################################*/

  static constexpr size_t kGCInterval = 100000;
  static constexpr size_t kGarbageNumLarge = 1E5;
  static constexpr size_t kGarbageNumSmall = 10;

  /*############################################################################
   * Test setup/teardown
   *##########################################################################*/

  void
  SetUp() override
  {
    // create a user directory for testing
    auto &&pool_path = GetTmpPoolPath();
    gc_path_ = pool_path;
    pool_path /= kPoolName;
    if (std::filesystem::exists(pool_path)) {
      pop_ = pmemobj_open(pool_path.c_str(), kLayout);
    } else {
      pop_ = pmemobj_create(pool_path.c_str(), kLayout, kSize, kModeRW);
    }

    gc_path_ /= kGCName;
    gc_ = std::make_unique<EpochBasedGC_t>(gc_path_, kSize, kLayout, kGCInterval, kThreadNum);
    gc_->StartGC();
  }

  void
  TearDown() override
  {
    gc_.reset(nullptr);

    EXPECT_TRUE(OID_IS_NULL(pmemobj_first(pop_)));
    pmemobj_close(pop_);
  }

  /*############################################################################
   * Internal utility functions
   *##########################################################################*/

  void
  AddGarbage(  //
      std::promise<GarbageRef> p,
      const size_t garbage_num)
  {
    GarbageRef target_weak_ptrs;
    auto *garbage = gc_->GetTmpField<SharedPtrTarget>(0);
    for (size_t loop = 0; loop < garbage_num; ++loop) {
      gc_->GetPageIfPossible<SharedPtrTarget>(garbage);
      if (OID_IS_NULL(*garbage)) {
        Malloc(pop_, garbage, sizeof(std::shared_ptr<Target>));
      }
      auto *target = new Target{0};
      auto *shared = new (pmemobj_direct(*garbage)) std::shared_ptr<Target>{target};
      target_weak_ptrs.emplace_back(*shared);
      gc_->AddGarbage<SharedPtrTarget>(garbage);
    }
    p.set_value(std::move(target_weak_ptrs));
  }

  void
  KeepEpochGuard(std::promise<Target> p)
  {
    const auto guard = gc_->CreateEpochGuard();
    p.set_value(0);  // set promise to notice
    const auto lock = std::unique_lock<std::mutex>{mtx_};
  }

  auto
  TestGC(  //
      const size_t thread_num,
      const size_t garbage_num)  //
      -> GarbageRef
  {
    std::vector<std::future<GarbageRef>> futures;
    for (size_t i = 0; i < thread_num; ++i) {
      std::promise<GarbageRef> p;
      futures.emplace_back(p.get_future());
      std::thread{&EpochBasedGCFixture::AddGarbage, this, std::move(p), garbage_num}.detach();
    }

    GarbageRef target_weak_ptrs;
    for (auto &&future : futures) {
      auto weak_ptrs = future.get();
      target_weak_ptrs.insert(target_weak_ptrs.end(), weak_ptrs.begin(), weak_ptrs.end());
    }

    return target_weak_ptrs;
  }

  auto
  TestReuse(const size_t garbage_num)  //
      -> GarbageRef
  {
    // an array for embedding reserved pages
    std::array<std::pair<std::mutex, PMEMoid>, kThreadNum> arr{};
    for (auto &&elem : arr) {
      elem.second = OID_NULL;
    }

    // a lambda function for embedding pages in each thread
    auto f = [&](std::promise<GarbageRef> p) {
      // prepare a random-value engine
      std::mt19937_64 rand_engine(std::random_device{}());
      std::uniform_int_distribution<size_t> id_dist{0, arr.size() - 1};

      GarbageRef target_weak_ptrs;
      for (size_t loop = 0; loop < garbage_num; ++loop) {
        const auto guard = gc_->CreateEpochGuard();

        // prepare a page for embedding
        auto *target = new Target{loop};
        auto *garbage = gc_->GetTmpField<SharedPtrTarget>(0);
        gc_->GetPageIfPossible<SharedPtrTarget>(garbage);
        if (OID_IS_NULL(*garbage)) {
          Malloc(pop_, garbage, sizeof(std::shared_ptr<Target>));
        }
        auto *shared = new (pmemobj_direct(*garbage)) std::shared_ptr<Target>{target};
        target_weak_ptrs.emplace_back(*shared);

        // embed the page
        {
          const auto pos = id_dist(rand_engine);
          [[maybe_unused]] std::lock_guard guard{arr.at(pos).first};
          auto old_target = arr.at(pos).second;
          arr.at(pos).second = *garbage;
          *garbage = old_target;
        }

        if (!OID_IS_NULL(*garbage)) {
          gc_->AddGarbage<SharedPtrTarget>(garbage);
        }
      }

      p.set_value(std::move(target_weak_ptrs));
    };

    // run a reuse test with multi-threads
    std::vector<std::future<GarbageRef>> futures;
    for (size_t i = 0; i < kThreadNum; ++i) {
      std::promise<GarbageRef> p;
      futures.emplace_back(p.get_future());
      std::thread{f, std::move(p)}.detach();
    }

    // gather weak pointers of GC targets
    GarbageRef target_weak_ptrs;
    for (auto &&future : futures) {
      auto weak_ptrs = future.get();
      target_weak_ptrs.insert(target_weak_ptrs.end(), weak_ptrs.begin(), weak_ptrs.end());
    }

    // delete remaining instances
    for (auto &&elem : arr) {
      if (!OID_IS_NULL(elem.second)) {
        gc_->AddGarbage<SharedPtrTarget>(&(elem.second));
      }
    }

    return target_weak_ptrs;
  }

  /*############################################################################
   * Functions for verification
   *##########################################################################*/

  void
  VerifyDestructor(const size_t thread_num)
  {
    // register garbage to GC
    auto target_weak_ptrs = TestGC(thread_num, kGarbageNumLarge);

    // GC deletes all targets during its deconstruction
    gc_.reset(nullptr);

    // check there is no referece to target pointers
    for (auto &&target_weak : target_weak_ptrs) {
      EXPECT_TRUE(target_weak.expired());
    }
  }

  void
  VerifyStopGC(const size_t thread_num)
  {
    // register garbage to GC
    auto target_weak_ptrs = TestGC(thread_num, kGarbageNumLarge);

    // GC deletes all targets
    gc_->StopGC();

    // check there is no referece to target pointers
    for (auto &&target_weak : target_weak_ptrs) {
      EXPECT_TRUE(target_weak.expired());
    }
  }

  void
  VerifyCreateEpochGuard(const size_t thread_num)
  {
    // a lambda function to keep an epoch guard
    auto create_guard = [&](std::promise<Target> p) {
      const auto guard = gc_->CreateEpochGuard();
      p.set_value(0);  // set promise to notice
      const auto lock = std::unique_lock<std::mutex>{mtx_};
    };

    // create an epoch guard on anther thread
    std::thread guarder;

    {
      // create a lock to keep an epoch guard
      const auto thread_lock = std::unique_lock<std::mutex>{mtx_};

      // create an epoch guard
      std::promise<Target> p;
      auto f = p.get_future();
      guarder = std::thread{create_guard, std::move(p)};
      f.get();

      // register garbage to GC
      auto target_weak_ptrs = TestGC(thread_num, kGarbageNumLarge);

      // check target pointers remain
      for (auto &&target_weak : target_weak_ptrs) {
        EXPECT_FALSE(target_weak.expired());
      }
    }

    guarder.join();
  }

  void
  VerifyReusePageIfPossible()
  {
    // register garbage to GC
    auto target_weak_ptrs = TestReuse(kGarbageNumLarge);
    gc_->StopGC();

    // check there is no referece to target pointers
    for (auto &&target_weak : target_weak_ptrs) {
      EXPECT_TRUE(target_weak.expired());
    }
  }

  /*############################################################################
   * Internal member variables
   *##########################################################################*/

  std::unique_ptr<EpochBasedGC_t> gc_{};

  std::mutex mtx_{};

  std::filesystem::path gc_path_{};

  PMEMobjpool *pop_{nullptr};
};

/*##############################################################################
 * Unit test definitions
 *############################################################################*/

TEST_F(EpochBasedGCFixture, DestructorWithSingleThreadReleaseAllGarbage)
{  //
  VerifyDestructor(1);
}

TEST_F(EpochBasedGCFixture, DestructorWithMultiThreadsReleaseAllGarbage)
{  //
  VerifyDestructor(kThreadNum);
}

TEST_F(EpochBasedGCFixture, StopGCWithSingleThreadReleaseAllGarbage)
{  //
  VerifyStopGC(1);
}

TEST_F(EpochBasedGCFixture, StopGCWithMultiThreadsReleaseAllGarbage)
{  //
  VerifyStopGC(kThreadNum);
}

TEST_F(EpochBasedGCFixture, CreateEpochGuardWithSingleThreadProtectGarbage)
{
  VerifyCreateEpochGuard(1);
}

TEST_F(EpochBasedGCFixture, CreateEpochGuardWithMultiThreadsProtectGarbage)
{
  VerifyCreateEpochGuard(kThreadNum);
}

TEST_F(EpochBasedGCFixture, ReusePageIfPossibleWithMultiThreadsReleasePageOnlyOnce)
{  //
  VerifyReusePageIfPossible();
}

TEST_F(EpochBasedGCFixture, RunGCMultipleTimesWithSamePool)
{
  constexpr size_t kRpeatNum = 2;
  for (size_t i = 0; i < kRpeatNum; ++i) {
    VerifyCreateEpochGuard(kThreadNum);

    // reset GC
    gc_.reset(nullptr);

    // reuse the same pmemobj pool
    gc_ = std::make_unique<EpochBasedGC_t>(gc_path_, kSize, kLayout, kGCInterval, kThreadNum);
    gc_->StartGC();
  }
}

}  // namespace dbgroup::pmem::memory::test
