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

#include "axiom/optimizer/SchemaResolver.h"
#include "axiom/optimizer/SchemaUtils.h"

namespace facebook::velox::optimizer {

const connector::Table* SchemaResolver::findTable(const std::string& name) {
  TableNameParser parser(name);
  if (!parser.valid()) {
    VELOX_USER_FAIL("Invalid table name: '{}'", name);
  }

  connector::Connector* connector = parser.catalog().has_value()
      ? connector::getConnector(parser.catalog().value()).get()
      : defaultConnector_.get();

  std::string lookupName;
  if (parser.schema().has_value()) {
    lookupName = fmt::format("{}.{}", parser.schema().value(), parser.table());
  } else if (!defaultSchema_.empty()) {
    lookupName = fmt::format("{}.{}", defaultSchema_, parser.table());
  } else {
    lookupName = parser.table();
  }

  return connector->metadata()->findTable(lookupName);
}

} // namespace facebook::velox::optimizer
