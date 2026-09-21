# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.
#
# Downstream consumer of the installed Fabric Reconciliation package.
#
# This project lives outside the source tree on purpose: it proves that the
# installed artifacts alone are enough to build, link and run against the
# runtime. It is copied to a scratch directory, configured with
# CMAKE_PREFIX_PATH pointing at an install prefix, and built there.

cmake_minimum_required(VERSION 3.20)
project(FabricReconciliationConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(FabricReconciliation CONFIG REQUIRED)

add_executable(consumer consumer.cpp)
target_link_libraries(consumer PRIVATE SummonLabs::FabricReconciliation::fabric_reconciliation)

message(STATUS "FabricReconciliation version: ${FabricReconciliation_VERSION}")
message(STATUS "consumer links: SummonLabs::FabricReconciliation::fabric_reconciliation")
