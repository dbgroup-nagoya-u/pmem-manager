# Persistent Memory Manager

[![Ubuntu 22.04](https://github.com/dbgroup-nagoya-u/pmem-manager/actions/workflows/ubuntu_22.yaml/badge.svg)](https://github.com/dbgroup-nagoya-u/pmem-manager/actions/workflows/ubuntu_22.yaml) [![Ubuntu 20.04](https://github.com/dbgroup-nagoya-u/pmem-manager/actions/workflows/ubuntu_20.yaml/badge.svg)](https://github.com/dbgroup-nagoya-u/pmem-manager/actions/workflows/ubuntu_20.yaml)

This repository is an open source implementation of epoch-based garbage collection in persistent memory for reseach use.

- [Build](#build)
    - [Prerequisites](#prerequisites)
    - [Build Options](#build-options)
    - [Build and Run Unit Tests](#build-and-run-unit-tests)
- [Usage](#usage)
    - [Linking by CMake](#linking-by-cmake)
    - [Collect and Release Garbage Pages](#collect-and-release-garbage-pages)
    - [Check Temporary Fields After Machine Failures](#check-temporary-fields-after-machine-failures)
    - [Destruct Garbage before Releasing](#destruct-garbage-before-releasing)
    - [Reuse Garbage-Collected Pages](#reuse-garbage-collected-pages)
- [Acknowledgments](#acknowledgments)

## Build

### Prerequisites

```bash
sudo apt update && sudo apt install -y build-essential cmake libpmemobj-dev
```

### Build Options

#### Tuning Parameters

- `DBGROUP_MAX_THREAD_NUM`: The maximum number of worker threads (please refer to [cpp-utility](https://github.com/dbgroup-nagoya-u/cpp-utility)).

#### Parameters for Unit Testing

- `PMEM_MANAGER_BUILD_TESTS`: Build unit tests for this repository if `ON` (default `OFF`).
- `DBGROUP_TEST_THREAD_NUM`: The number of threads to run unit tests (default `2`).
- `DBGROUP_TEST_TMP_PMEM_PATH`: The path to persistent memory (default: `""`).

### Build and Run Unit Tests

```bash
mkdir build && cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DPMEM_MANAGER_BUILD_TESTS=ON \
  -DDBGROUP_TEST_TMP_PMEM_PATH="/pmem_tmp"
cmake --build . --parallel --config Release
ctest -C Release
```

## Usage

### Linking by CMake

Add this library to your build in `CMakeLists.txt`.

```cmake
FetchContent_Declare(
    pmem-manager
    GIT_REPOSITORY "https://github.com/dbgroup-nagoya-u/pmem-manager.git"
    GIT_TAG "<commit_tag_you_want_to_use>"
)
FetchContent_MakeAvailable(pmem-manager)

add_executable(
    <target_bin_name>
    [<source> ...]
)
target_link_libraries(<target_bin_name> PRIVATE
    dbgroup::pmem_manager
)
```

### Collect and Release Garbage Pages

If you wish to only release garbage, you can use our garbage collector as follows.

```cpp
// C++ standard libraries
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

// system libraries
#include <sys/stat.h>

// external system libraries
#include <libpmemobj.h>

// our libraries
#include "pmem/memory/epoch_based_gc.hpp"
#include "pmem/memory/utility.hpp"

auto
main(  //
    [[maybe_unused]] const int argc,
    const char *argv[])  //
    -> int
{
  // set the path to persistent memory as the first argument
  std::filesystem::path pool_path = argv[1];
  pool_path /= "test";
  std::filesystem::path gc_path = argv[1];
  gc_path /= "gc";

  // open a pmemobj pool
  auto *pop = std::filesystem::exists(pool_path)
                  ? pmemobj_open(pool_path.c_str(), "test")
                  : pmemobj_create(pool_path.c_str(), "test", PMEMOBJ_MIN_POOL, S_IRUSR | S_IWUSR);

  // create and run a garbage collector
  ::dbgroup::pmem::memory::EpochBasedGC gc{gc_path};
  gc.StartGC();

  // prepare a sample worker procedure
  auto worker = [&]() {
    for (size_t loop = 0; loop < 100; ++loop) {
      // this thread has not enter a current epoch yet
      {
        // we use the scoped pattern to prevent garbage from releasing
        const auto &guard = gc.CreateEpochGuard();

        // use a temporary field for preventing memory leak
        auto *tmp_oid = gc.GetTmpField(0);  // an index is in [0, 12]
        ::dbgroup::pmem::memory::Malloc(pop, tmp_oid, sizeof(size_t));

        // garbage OIDs must be moved to temporary fields before adding to GC
        gc.AddGarbage(tmp_oid);
      }
      // this thread has left the epoch

      std::this_thread::sleep_for(std::chrono::microseconds{100});  // dummy sleep
    }
  };

  {
    // prevent all garbage from releasing
    const auto &guard = gc.CreateEpochGuard();

    // create threads and wait for them to add garbage
    std::vector<std::thread> threads{};
    for (size_t i = 0; i < 8; ++i) {
      threads.emplace_back(worker);
    }
    for (auto &&t : threads) {
      t.join();
    }

    // check the garbage collector retains all the garbage
    size_t count = 0;
    for (auto &&oid = pmemobj_first(pop); !OID_IS_NULL(oid); oid = pmemobj_next(oid), ++count) {
      // traverse all the garbage OIDs
    }
    std::cout << "# of allocated OIDs: " << count << "\n";
  }

  // check there is no allocated OIDs in the pool after stopping GC
  gc.StopGC();
  if (OID_IS_NULL(pmemobj_first(pop))) {
    std::cout << "All the garbage OIDs are released by GC.\n";
  }
  pmemobj_close(pop);

  return 0;
}
```

This example will return the following output.

```
# of allocated OIDs: 800
All the garbage OIDs are released by GC.
```

### Check Temporary Fields After Machine Failures

We prepare a function `GetUnreleasedFields` to scan temporary fields for recovery procedures.

```cpp
const auto &tmp_fields = gc.GetUnreleasedFields();
if (!tmp_fields.empty()) {
  // perform a recovery procedure for your data structure
}
```

### Destruct Garbage before Releasing

You can call a specific destructor before releasing garbage.

NOTE: We only call a given destructor and do not guarantee consistency. Therefore, you must implement a destructor that can guarantee the consistency of your data and is idempotent (a destructor can be called multiple times before/after machine failures).

```cpp
// C++ standard libraries
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// system libraries
#include <sys/stat.h>

// external system libraries
#include <libpmemobj.h>

// our libraries
#include "pmem/memory/epoch_based_gc.hpp"
#include "pmem/memory/utility.hpp"

// prepare the information of target garbage
struct SharedPtrTarget : public ::dbgroup::pmem::memory::DefaultTarget {
  // set the type of garbage to perform destructor
  using T = std::shared_ptr<size_t>;
};

auto
main(  //
    [[maybe_unused]] const int argc,
    const char *argv[])  //
    -> int
{
  // set the path to persistent memory as the first argument
  std::filesystem::path pool_path = argv[1];
  pool_path /= "test";
  std::filesystem::path gc_path = argv[1];
  gc_path /= "gc";

  // open a pmemobj pool
  auto *pop = std::filesystem::exists(pool_path)
                  ? pmemobj_open(pool_path.c_str(), "test")
                  : pmemobj_create(pool_path.c_str(), "test", PMEMOBJ_MIN_POOL, S_IRUSR | S_IWUSR);

  // specify a target class as template
  ::dbgroup::pmem::memory::EpochBasedGC<SharedPtrTarget> gc{gc_path};
  gc.StartGC();

  // prepare weak_ptr for checking garbage's lifetime
  std::vector<std::weak_ptr<size_t>> weak_pointers{};
  std::mutex lock{};

  // prepare a sample worker procedure
  auto worker = [&]() {
    for (size_t loop = 0; loop < 100; ++loop) {
      {
        const auto &guard = gc.CreateEpochGuard();

        // create a shared pointer in persistent memory
        auto *tmp_oid = gc.GetTmpField<SharedPtrTarget>(0);
        ::dbgroup::pmem::memory::Malloc(pop, tmp_oid, sizeof(std::shared_ptr<size_t>));
        auto *page = new (pmemobj_direct(*tmp_oid)) std::shared_ptr<size_t>{new size_t{loop}};

        // track the lifetime of this garbage
        {
          const auto &lock_guard = std::lock_guard{lock};
          weak_pointers.emplace_back(*page);
        }

        gc.AddGarbage<SharedPtrTarget>(tmp_oid);
      }
      std::this_thread::sleep_for(std::chrono::microseconds{100});
    }
  };

  // create threads and wait for them to add garbage
  std::vector<std::thread> threads{};
  for (size_t i = 0; i < 8; ++i) {
    threads.emplace_back(worker);
  }
  for (auto &&t : threads) {
    t.join();
  }

  // check the specified destructor is called for all the garbage
  gc.StopGC();
  for (const auto &weak_p : weak_pointers) {
    if (!weak_p.expired()) {
      std::cout << "Failed: there is the unreleased garbage." << std::endl;
      std::terminate();
    }
  }
  std::cout << "Succeeded: all the garbage has been released." << std::endl;
  pmemobj_close(pop);

  return 0;
}
```

### Reuse Garbage-Collected Pages

You can reuse garbage-collected pages. Our GC maintains garbage lists in thread local storage of each thread, so reusing pages can avoid the contention due to memory allocation.

```cpp
// C++ standard libraries
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

// system libraries
#include <sys/stat.h>

// external system libraries
#include <libpmemobj.h>

// our libraries
#include "pmem/memory/epoch_based_gc.hpp"
#include "pmem/memory/utility.hpp"

// prepare the information of target garbage
struct ReusableTarget : public ::dbgroup::pmem::memory::DefaultTarget {
  // reuse garbage-collected pages
  static constexpr bool kReusePages = true;
};

auto
main(  //
    [[maybe_unused]] const int argc,
    const char *argv[])  //
    -> int
{
  // set the path to persistent memory as the first argument
  std::filesystem::path pool_path = argv[1];
  pool_path /= "test";
  std::filesystem::path gc_path = argv[1];
  gc_path /= "gc";

  // open a pmemobj pool
  auto *pop = std::filesystem::exists(pool_path)
                  ? pmemobj_open(pool_path.c_str(), "test")
                  : pmemobj_create(pool_path.c_str(), "test", PMEMOBJ_MIN_POOL, S_IRUSR | S_IWUSR);

  // create and run a garbage collector
  ::dbgroup::pmem::memory::EpochBasedGC<ReusableTarget> gc{gc_path};
  gc.StartGC();

  // prepare a sample worker procedure
  std::mutex lock{};
  size_t count = 0;
  auto worker = [&]() {
    for (size_t loop = 0; loop < 100; ++loop) {
      {
        const auto &guard = gc.CreateEpochGuard();

        auto *tmp_oid = gc.GetTmpField<ReusableTarget>(0);

        // get a page if exist
        gc.GetPageIfPossible<ReusableTarget>(tmp_oid);
        if (OID_IS_NULL(*tmp_oid)) {
          ::dbgroup::pmem::memory::Malloc(pop, tmp_oid, sizeof(size_t));
        } else {
          const auto &lock_guard = std::lock_guard{lock};
          ++count;
        }

        gc.AddGarbage<ReusableTarget>(tmp_oid);
      }

      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
  };

  // create threads and wait for them to add garbage
  std::vector<std::thread> threads{};
  for (size_t i = 0; i < 8; ++i) {
    threads.emplace_back(worker);
  }
  for (auto &&t : threads) {
    t.join();
  }

  // check there is no allocated OIDs in the pool after stopping GC
  gc.StopGC();
  if (OID_IS_NULL(pmemobj_first(pop))) {
    std::cout << "# of reused OIDs: " << count << "\n"
              << "All the garbage OIDs are released by GC.\n";
  }
  pmemobj_close(pop);

  return 0;
}
```

This example will return the following output (the number of reused OIDs will vary depending on the machine being run).

```
# of reused OIDs: 560
All the garbage OIDs are released by GC.
```

## Acknowledgments

This work is based on results from project JPNP16007 commissioned by the New Energy and Industrial Technology Development Organization (NEDO), and it was supported partially by KAKENHI (JP20K19804, JP21H03555, and JP22H03594).
