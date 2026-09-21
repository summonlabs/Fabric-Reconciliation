# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.
#
# Fabric Reconciliation -- build and install.
#
# The runtime is portable C++20 with no third-party dependency. It uses the
# standard library and, on Windows, the platform sockets library that ships with
# the operating system.

cmake_minimum_required(VERSION 3.20)

project(FabricReconciliation
  VERSION 1.0.0
  DESCRIPTION "Post-disruption comparison and reconciliation intent between intended and observed fabric state"
  HOMEPAGE_URL "https://github.com/summonlabs/Fabric-Reconciliation"
  LANGUAGES CXX)

set(FABRIC_RECONCILIATION_VERSION "${PROJECT_VERSION}")

option(FABRIC_RECONCILIATION_BUILD_TOOLS "Build the command line tools" ON)
option(FABRIC_RECONCILIATION_BUILD_TESTS "Build the test suites" ON)
option(FABRIC_RECONCILIATION_BUILD_EXAMPLES "Build the examples" ON)
option(FABRIC_RECONCILIATION_BUILD_BENCH "Build the scale benchmark" ON)
option(FABRIC_RECONCILIATION_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(FABRIC_RECONCILIATION_ENABLE_SANITIZERS "Build with AddressSanitizer and UBSan where supported" OFF)
option(FABRIC_RECONCILIATION_BUILD_SHARED "Build the runtime as a shared library" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(FABRIC_RECONCILIATION_BUILD_SHARED)
  set(FABRIC_RECONCILIATION_LIBRARY_TYPE SHARED)
else()
  set(FABRIC_RECONCILIATION_LIBRARY_TYPE STATIC)
endif()

add_library(fabric_reconciliation ${FABRIC_RECONCILIATION_LIBRARY_TYPE}
  src/authority.cpp
  src/canonical.cpp
  src/client.cpp
  src/document.cpp
  src/document_json.cpp
  src/drift.cpp
  src/engine.cpp
  src/explain.cpp
  src/hash.cpp
  src/identity.cpp
  src/journal.cpp
  src/json.cpp
  src/lineage.cpp
  src/net.cpp
  src/plan.cpp
  src/policy.cpp
  src/server.cpp
  src/snapshot.cpp
  src/status.cpp
  src/store.cpp
  src/value.cpp
  src/wire.cpp)

add_library(SummonLabs::FabricReconciliation::fabric_reconciliation ALIAS fabric_reconciliation)

target_include_directories(fabric_reconciliation
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(fabric_reconciliation PUBLIC cxx_std_20)

if(FABRIC_RECONCILIATION_BUILD_SHARED)
  target_compile_definitions(fabric_reconciliation
    PUBLIC FABRIC_RECONCILIATION_SHARED
    PRIVATE FABRIC_RECONCILIATION_BUILDING_LIBRARY)
endif()

find_package(Threads REQUIRED)
target_link_libraries(fabric_reconciliation PRIVATE Threads::Threads)

if(WIN32)
  target_link_libraries(fabric_reconciliation PRIVATE ws2_32)
endif()

# ---- warnings ----
if(MSVC)
  set(FABRIC_RECONCILIATION_WARNINGS
    /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /Zc:preprocessor
    /w14242 /w14254 /w14263 /w14265 /w14287 /w14296 /w14311 /w14545 /w14546 /w14547
    /w14549 /w14555 /w14619 /w14640 /w14826 /w14905 /w14906 /w14928)
  if(FABRIC_RECONCILIATION_WARNINGS_AS_ERRORS)
    list(APPEND FABRIC_RECONCILIATION_WARNINGS /WX)
  endif()
  add_compile_options(${FABRIC_RECONCILIATION_WARNINGS})
else()
  set(FABRIC_RECONCILIATION_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
  if(FABRIC_RECONCILIATION_WARNINGS_AS_ERRORS)
    list(APPEND FABRIC_RECONCILIATION_WARNINGS -Werror)
  endif()
  add_compile_options(${FABRIC_RECONCILIATION_WARNINGS})
endif()

if(FABRIC_RECONCILIATION_ENABLE_SANITIZERS)
  if(MSVC)
    target_compile_options(fabric_reconciliation PUBLIC /fsanitize=address)
  else()
    target_compile_options(fabric_reconciliation PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(fabric_reconciliation PUBLIC -fsanitize=address,undefined)
  endif()
endif()

# ---- tools and tests ----
if(FABRIC_RECONCILIATION_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(FABRIC_RECONCILIATION_BUILD_BENCH)
  add_subdirectory(bench)
endif()

if(FABRIC_RECONCILIATION_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(FABRIC_RECONCILIATION_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---- installation ----
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS fabric_reconciliation
  EXPORT FabricReconciliationTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT FabricReconciliationTargets
  FILE FabricReconciliationTargets.cmake
  NAMESPACE SummonLabs::FabricReconciliation::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FabricReconciliation)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FabricReconciliationConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/FabricReconciliationConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FabricReconciliation)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/FabricReconciliationConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/FabricReconciliationConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/FabricReconciliationConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FabricReconciliation)

install(FILES README.md LICENSE
  DESTINATION ${CMAKE_INSTALL_DATADIR}/FabricReconciliation)
