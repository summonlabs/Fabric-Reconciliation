# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.
#
# Test targets.
#
# No test sets, and no test relies on, a timeout of any kind. A hanging test is
# a defect: the lifecycle, shutdown and blocking-I/O paths are designed so that
# every wait is released deterministically, and the suites exercise that.

function(fabric_reconciliation_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE SummonLabs::FabricReconciliation::fabric_reconciliation)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

fabric_reconciliation_test(fr_test_core
  test_main.cpp
  test_core.cpp)

fabric_reconciliation_test(fr_test_json
  test_main.cpp
  test_json.cpp)

fabric_reconciliation_test(fr_test_drift
  test_main.cpp
  test_drift.cpp)

fabric_reconciliation_test(fr_test_plan
  test_main.cpp
  test_plan.cpp)

fabric_reconciliation_test(fr_test_adversarial
  test_main.cpp
  test_adversarial.cpp)

fabric_reconciliation_test(fr_test_persistence
  test_main.cpp
  test_persistence.cpp)

fabric_reconciliation_test(fr_test_engine
  test_main.cpp
  test_engine.cpp)

fabric_reconciliation_test(fr_test_transport
  test_main.cpp
  test_transport.cpp)

fabric_reconciliation_test(fr_test_concurrency
  test_main.cpp
  test_concurrency.cpp)

fabric_reconciliation_test(fr_test_scale
  test_main.cpp
  test_scale.cpp)

fabric_reconciliation_test(fr_test_reference
  test_main.cpp
  test_reference.cpp)

if(TARGET fabric-reconcile-worker)
  fabric_reconciliation_test(fr_test_multiprocess
    test_main.cpp
    test_multiprocess.cpp)
  target_compile_definitions(fr_test_multiprocess PRIVATE
    FR_WORKER_EXE="$<TARGET_FILE:fabric-reconcile-worker>"
    FR_CLI_EXE="$<TARGET_FILE:fabric-reconciliation>")
endif()

# Deterministic helpers shared by several suites.
add_library(fr_test_support STATIC test_helpers.cpp)
target_link_libraries(fr_test_support PUBLIC SummonLabs::FabricReconciliation::fabric_reconciliation)
target_include_directories(fr_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fr_test_core PRIVATE fr_test_support)
target_link_libraries(fr_test_drift PRIVATE fr_test_support)
target_link_libraries(fr_test_plan PRIVATE fr_test_support)
target_link_libraries(fr_test_adversarial PRIVATE fr_test_support)
target_link_libraries(fr_test_persistence PRIVATE fr_test_support)
target_link_libraries(fr_test_engine PRIVATE fr_test_support)
target_link_libraries(fr_test_transport PRIVATE fr_test_support)
target_link_libraries(fr_test_concurrency PRIVATE fr_test_support)
target_link_libraries(fr_test_scale PRIVATE fr_test_support)
target_link_libraries(fr_test_reference PRIVATE fr_test_support)
if(TARGET fr_test_multiprocess)
  target_link_libraries(fr_test_multiprocess PRIVATE fr_test_support)
endif()
