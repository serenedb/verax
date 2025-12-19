# Copyright (c) Meta Platforms, Inc. and its affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
cmake_minimum_required(VERSION 3.14)

# Set the project name.
project(axiom)

list(PREPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/CMake" "${PROJECT_SOURCE_DIR}/../velox/CMake")

add_subdirectory(axiom/common)
# TODO(mbkkt) add_subdirectory(axiom/sql/presto)
add_subdirectory(axiom/logical_plan)
add_subdirectory(axiom/runner)
add_subdirectory(axiom/connectors)
add_subdirectory(axiom/optimizer)
add_subdirectory(axiom/cli)

if(NOT AXIOM_BUILD_TESTING)
  return()
endif()

add_custom_target(
  axiom-tests
  DEPENDS
    # TODO(mbkkt) axiom_local_hive_connector_metadata_test
    axiom_test_connector_test
    axiom_connectors_tests
    # TODO(mbkkt) axiom_tpch_connector_metadata_test
    axiom_logical_plan_tests
    # TODO(mbkkt) axiom_optimizer_tpch_plan_test
    # TODO(mbkkt) axiom_optimizer_tests
    axiom_model_test
  # TODO(mbkkt) axiom_runner_local_runner_test
  # TODO(mbkkt) axiom_runner_presto_query_replay_runner_test
  # TODO(mbkkt) axiom_sql_presto_grammar_test
  # TODO(mbkkt) axiom_sql_presto_test
)
