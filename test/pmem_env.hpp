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

#ifndef TEST_PMEM_ENV_HPP
#define TEST_PMEM_ENV_HPP

// C++ standard libraries
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

// system libraries
#include <sys/stat.h>

// external libraries
#include "gtest/gtest.h"

// utility macros for expanding compile definitions as std::string
#define DBGROUP_ADD_QUOTES_INNER(x) #x                     // NOLINT
#define DBGROUP_ADD_QUOTES(x) DBGROUP_ADD_QUOTES_INNER(x)  // NOLINT

namespace dbgroup::pmem
{
/*##############################################################################
 * Global constants
 *############################################################################*/

constexpr int kModeRW = S_IWUSR | S_IRUSR;  // NOLINT

constexpr std::string_view kTmpPMEMPath = DBGROUP_ADD_QUOTES(DBGROUP_TEST_TMP_PMEM_PATH);

const std::string_view user_name = std::getenv("USER");

/*##############################################################################
 * Global utilities
 *############################################################################*/

inline auto
GetTmpPoolPath()  //
    -> std::filesystem::path
{
  std::filesystem::path pool_path{kTmpPMEMPath};
  pool_path /= user_name;
  pool_path /= "tmp_test_dir";
  return pool_path;
}

struct TmpDirManager : public ::testing::Environment {
  void
  SetUp() override
  {
    // check the specified path
    if (kTmpPMEMPath.empty() || !std::filesystem::exists(kTmpPMEMPath)) {
      std::cerr << "WARN: The correct path to persistent memory is not set." << std::endl;
      GTEST_SKIP();
    }

    // prepare a temporary directory for testing
    const auto &pool_path = GetTmpPoolPath();
    std::filesystem::remove_all(pool_path);
    std::filesystem::create_directories(pool_path);
  }

  void
  TearDown() override
  {
    const auto &pool_path = GetTmpPoolPath();
    std::filesystem::remove_all(pool_path);
  }
};

}  // namespace dbgroup::pmem

#endif  // TEST_PMEM_ENV_HPP
