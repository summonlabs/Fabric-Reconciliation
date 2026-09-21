# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.

add_executable(fabric-reconciliation-example basic_plan.cpp)
target_link_libraries(fabric-reconciliation-example PRIVATE SummonLabs::FabricReconciliation::fabric_reconciliation)
