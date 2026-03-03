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
#pragma once

#include <memory>
#include <string>
#include <utility>

namespace facebook::velox::memory {
class MemoryPool;
} // namespace facebook::velox::memory

namespace facebook::axiom::connector {

/// Read-only query-specific information.
class ConnectorSession final {
 public:
  explicit ConnectorSession(std::string queryId)
      : queryId_{std::move(queryId)} {}

  ConnectorSession(
      std::string queryId,
      std::shared_ptr<velox::memory::MemoryPool> connectorPool,
      std::shared_ptr<velox::memory::MemoryPool> operatorPool)
      : queryId_{std::move(queryId)},
        connectorPool_{std::move(connectorPool)},
        operatorPool_{std::move(operatorPool)} {}

  const std::string& queryId() const {
    return queryId_;
  }

  /// Aggregate memory pool.
  const std::shared_ptr<velox::memory::MemoryPool>& connectorPool() const {
    return connectorPool_;
  }

  /// Leaf memory pool.
  const std::shared_ptr<velox::memory::MemoryPool>& operatorPool() const {
    return operatorPool_;
  }

 private:
  const std::string queryId_;
  std::shared_ptr<velox::memory::MemoryPool> connectorPool_;
  std::shared_ptr<velox::memory::MemoryPool> operatorPool_;
};

using ConnectorSessionPtr = std::shared_ptr<ConnectorSession>;

} // namespace facebook::axiom::connector
