cmake_minimum_required(VERSION "3.11")

# decline in-source build
if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_BINARY_DIR}")
  message(FATAL_ERROR "In-source builds are not allowed.")
endif()

# project information
project(
  pmem_manager
  VERSION "0.2.0"
  DESCRIPTION "A utility library for managing persistent memory."
  LANGUAGES "CXX"
)

if(NOT TARGET dbgroup::${PROJECT_NAME})
  # enable features
  include(FetchContent)
  find_package(PkgConfig)

  #----------------------------------------------------------------------------#
  # Configurations
  #----------------------------------------------------------------------------#

  # use multi-threads
  find_package(Threads)

  # use PMDK
  pkg_check_modules(LIBPMEM REQUIRED libpmem)
  pkg_check_modules(LIBPMEMOBJ REQUIRED libpmemobj)

  # use our thread ID manager
  FetchContent_Declare(
    cpp-utility
    GIT_REPOSITORY "https://github.com/dbgroup-nagoya-u/cpp-utility.git"
    GIT_TAG "d78f5873934ce67b73215b8d44dc97e1d7140199"
  )
  FetchContent_MakeAvailable(cpp-utility)

  #----------------------------------------------------------------------------#
  # Build targets
  #----------------------------------------------------------------------------#

  add_library(${PROJECT_NAME} STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/component/tls_fields.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/component/garbage_list_in_pmem.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/component/garbage_list_in_dram.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/utility.cpp"
  )
  add_library(dbgroup::${PROJECT_NAME} ALIAS ${PROJECT_NAME})
  target_compile_features(${PROJECT_NAME} PUBLIC
    "cxx_std_17"
  )
  target_include_directories(${PROJECT_NAME} PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${LIBPMEM_INCLUDE_DIRS}"
    "${LIBPMEMOBJ_INCLUDE_DIRS}"
  )
  target_link_libraries(${PROJECT_NAME} PUBLIC
    Threads::Threads
    ${LIBPMEM_LIBRARIES}
    ${LIBPMEMOBJ_LIBRARIES}
    dbgroup::cpp_utility
  )

  #----------------------------------------------------------------------------#
  # Build unit tests
  #----------------------------------------------------------------------------#

  # build unit tests if required
  option(PMEM_MANAGER_BUILD_TESTS "build unit tests for this repository" OFF)
  if(${PMEM_MANAGER_BUILD_TESTS})
    enable_testing()
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/test")
  endif()
endif()
