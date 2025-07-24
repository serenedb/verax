/*
 * Copyright (c) Meta Platforms, Inc. and its affiliates.
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

#include "axiom/optimizer/connectors/tests/TestConnector.h"

namespace facebook::velox::connector {

ConnectorTableHandlePtr TestConnectorMetadata::createTableHandle(
    const TableLayout& /* layout */,
    std::vector<ColumnHandlePtr> /* columnHandles */,
    core::ExpressionEvaluator& /* evaluator */,
    std::vector<core::TypedExprPtr> filters,
    std::vector<core::TypedExprPtr>& rejectedFilters,
    RowTypePtr /* dataColumns */,
    std::optional<LookupKeys>) {
  rejectedFilters = std::move(filters);
  return std::make_shared<TestTableHandle>(connector_->connectorId());
}

} // namespace facebook::velox::connector
