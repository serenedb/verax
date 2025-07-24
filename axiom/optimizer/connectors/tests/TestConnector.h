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

#include "axiom/optimizer/connectors/ConnectorMetadata.h"

namespace facebook::velox::connector {

class TestConnector;

class TestTableLayout : public TableLayout {
 public:
  TestTableLayout(
      const std::string& name,
      const Table* table,
      connector::Connector* connector,
      std::vector<const Column*> columns)
      : TableLayout(
            name,
            table,
            connector,
            std::move(columns),
            std::vector<const Column*>{},
            std::vector<const Column*>{},
            std::vector<SortOrder>{},
            std::vector<const Column*>{},
            true) {}

  std::pair<int64_t, int64_t> sample(
      const connector::ConnectorTableHandlePtr& /* handle */,
      float /* pct */,
      std::vector<core::TypedExprPtr> /* extraFilters */,
      RowTypePtr /* outputType */ = nullptr,
      const std::vector<common::Subfield>& /* fields */ = {},
      HashStringAllocator* /* allocator */ = nullptr,
      std::vector<ColumnStatistics>* /* statistics */ = nullptr) const {
    return std::make_pair(1'000, 1'000);
  }
};

class TestTable : public Table {
 public:
  TestTable(
      const std::string& name,
      const RowTypePtr& schema,
      TestConnector* connector)
      : Table(name), connector_{connector} {
    type_ = schema;

    columns_.reserve(schema->size());

    std::vector<const Column*> columnPtrs;
    columnPtrs.reserve(schema->size());

    for (auto i = 0; i < schema->size(); ++i) {
      auto column =
          std::make_shared<Column>(schema->nameOf(i), schema->childAt(i));
      columns_.emplace_back(column);
      columnPtrMap_.emplace(column->name(), column.get());
      columnPtrs.emplace_back(column.get());
    }

    layout_ =
        std::make_shared<TestTableLayout>(name, this, connector, columnPtrs);
    layoutPtrs_.emplace_back(layout_.get());
  }

  const std::unordered_map<std::string, const Column*>& columnMap()
      const override {
    return columnPtrMap_;
  }

  const std::vector<const TableLayout*>& layouts() const override {
    return layoutPtrs_;
  }

  uint64_t numRows() const override {
    return 100;
  }

 private:
  TestConnector* connector_;
  std::vector<std::shared_ptr<const Column>> columns_;
  std::unordered_map<std::string, const Column*> columnPtrMap_;
  std::shared_ptr<const TableLayout> layout_;
  std::vector<const TableLayout*> layoutPtrs_;
};

class TestColumnHandle : public ColumnHandle {
 public:
  explicit TestColumnHandle(const std::string& name) : name_{name} {}

  const std::string& name() const override {
    return name_;
  }

 private:
  const std::string name_;
};

class TestTableHandle : public ConnectorTableHandle {
 public:
  explicit TestTableHandle(const std::string& connectorId)
      : ConnectorTableHandle{connectorId} {}

  std::string toString() const override {
    return "TestTableHandle";
  }
};

class TestConnectorMetadata : public ConnectorMetadata {
 public:
  explicit TestConnectorMetadata(TestConnector* connector)
      : connector_{connector} {}

  void initialize() override {}

  const Table* findTable(const std::string& name) override {
    auto it = tables_.find(name);
    VELOX_USER_CHECK(it != tables_.end(), "Test table not found: {}", name);
    return it->second.get();
  }

  ConnectorSplitManager* splitManager() override {
    VELOX_NYI();
  }

  void addTable(const std::string& name, const RowTypePtr& schema) {
    tables_.emplace(
        name, std::make_unique<TestTable>(name, schema, connector_));
  }

  ColumnHandlePtr createColumnHandle(
      const TableLayout& /* layoutData */,
      const std::string& columnName,
      std::vector<common::Subfield> /* subfields */,
      std::optional<TypePtr> /* castToType */,
      SubfieldMapping /* subfieldMapping */) override {
    return std::make_shared<TestColumnHandle>(columnName);
  }

  ConnectorTableHandlePtr createTableHandle(
      const TableLayout& layout,
      std::vector<ColumnHandlePtr> columnHandles,
      core::ExpressionEvaluator& evaluator,
      std::vector<core::TypedExprPtr> filters,
      std::vector<core::TypedExprPtr>& rejectedFilters,
      RowTypePtr dataColumns,
      std::optional<LookupKeys>) override;

 private:
  TestConnector* connector_;
  std::unordered_map<std::string, std::unique_ptr<TestTable>> tables_;
};

class TestConnector : public Connector {
 public:
  explicit TestConnector(const std::string& id)
      : Connector(id),
        metadata_{std::make_unique<TestConnectorMetadata>(this)} {}

  ConnectorMetadata* metadata() const override {
    return metadata_.get();
  }

  std::unique_ptr<DataSource> createDataSource(
      const RowTypePtr& /* outputType */,
      const ConnectorTableHandlePtr& /* tableHandle */,
      const ColumnHandleMap& /* columnHandles */,
      ConnectorQueryCtx* /* connectorQueryCtx */) override {
    VELOX_NYI();
  }

  std::unique_ptr<DataSink> createDataSink(
      RowTypePtr /* inputType */,
      ConnectorInsertTableHandlePtr /* connectorInsertTableHandle */,
      ConnectorQueryCtx* /* connectorQueryCtx */,
      CommitStrategy /* commitStrategy */) override {
    VELOX_NYI();
  }

  void addTable(const std::string& name, const RowTypePtr& schema) {
    metadata_->addTable(name, schema);
  }

  void addTable(const std::string& name) {
    metadata_->addTable(name, ROW({}, {}));
  }

 private:
  const std::unique_ptr<TestConnectorMetadata> metadata_;
};

} // namespace facebook::velox::connector
