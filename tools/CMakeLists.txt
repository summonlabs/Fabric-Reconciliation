# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.

add_executable(fabric-reconciliation fabric_reconciliation_cli.cpp)
target_link_libraries(fabric-reconciliation PRIVATE SummonLabs::FabricReconciliation::fabric_reconciliation)
install(TARGETS fabric-reconciliation RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_executable(fabric-reconcile-worker fabric_reconcile_worker.cpp)
target_link_libraries(fabric-reconcile-worker PRIVATE SummonLabs::FabricReconciliation::fabric_reconciliation)
install(TARGETS fabric-reconcile-worker RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
