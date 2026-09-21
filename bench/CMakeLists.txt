# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.

add_executable(fabric-reconcile-bench fabric_reconcile_bench.cpp)
target_link_libraries(fabric-reconcile-bench PRIVATE SummonLabs::FabricReconciliation::fabric_reconciliation)
install(TARGETS fabric-reconcile-bench RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
