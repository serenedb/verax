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

#include "axiom/optimizer/tests/ParquetTpchTest.h"
#include "velox/common/file/FileSystems.h"
#include "velox/connectors/hive/HiveConnector.h"
#include "velox/connectors/tpch/TpchConnector.h"
#include "velox/dwio/parquet/RegisterParquetReader.h"
#include "velox/dwio/parquet/RegisterParquetWriter.h"
#include "velox/exec/tests/utils/AssertQueryBuilder.h"
#include "velox/exec/tests/utils/PlanBuilder.h"
#include "velox/functions/prestosql/aggregates/RegisterAggregateFunctions.h"
#include "velox/functions/prestosql/registration/RegistrationFunctions.h"
#include "velox/parse/TypeResolver.h"

DEFINE_string(
    data_path,
    "",
    "Path to TPCH data directory. If empty, the test creates a temp directory and deletes it on exit");
DEFINE_bool(create_dataset, true, "Creates the TPCH tables");
DEFINE_double(tpch_scale, 0.01, "Scale factor");

using namespace facebook::velox::exec;
using namespace facebook::velox::exec::test;

namespace facebook::velox::optimizer::test {

std::string ParquetTpchTest::createPath_;
std::string ParquetTpchTest::path_;
std::shared_ptr<TempDirectoryPath> ParquetTpchTest::tempDirectory_;

//  static
void ParquetTpchTest::SetUpTestCase() {
  memory::MemoryManager::testingSetInstance(memory::MemoryManagerOptions{});

  if (FLAGS_data_path.empty()) {
    tempDirectory_ = TempDirectoryPath::create();
    createPath_ = tempDirectory_->getPath();
    path_ = createPath_;
    FLAGS_data_path = createPath_;
  } else if (FLAGS_create_dataset) {
    VELOX_CHECK(!FLAGS_data_path.empty());
    createPath_ = FLAGS_data_path;
    path_ = createPath_;
  }

  functions::prestosql::registerAllScalarFunctions();
  aggregate::prestosql::registerAllAggregateFunctions();

  parse::registerTypeResolver();
  filesystems::registerLocalFileSystem();
  dwio::common::registerFileSinks();

  parquet::registerParquetReaderFactory();
  parquet::registerParquetWriterFactory();

  auto emptyConfig = std::make_shared<config::ConfigBase>(
      std::unordered_map<std::string, std::string>());

  connector::hive::HiveConnectorFactory hiveConnectorFactory;
  auto hiveConnector = hiveConnectorFactory.newConnector(
      std::string(PlanBuilder::kHiveDefaultConnectorId), emptyConfig);
  connector::registerConnector(std::move(hiveConnector));

  connector::tpch::TpchConnectorFactory tpchConnectorFactory;
  auto tpchConnector = tpchConnectorFactory.newConnector(
      std::string(PlanBuilder::kTpchDefaultConnectorId), emptyConfig);
  connector::registerConnector(std::move(tpchConnector));

  if (!createPath_.empty()) {
    saveTpchTablesAsParquet();
  }
}

//  static
void ParquetTpchTest::TearDownTestCase() {
  connector::unregisterConnector(
      std::string(PlanBuilder::kHiveDefaultConnectorId));
  connector::unregisterConnector(
      std::string(PlanBuilder::kTpchDefaultConnectorId));
  parquet::unregisterParquetReaderFactory();
  parquet::unregisterParquetWriterFactory();
}

void ParquetTpchTest::saveTpchTablesAsParquet() {
  std::shared_ptr<memory::MemoryPool> rootPool{
      memory::memoryManager()->addRootPool()};
  std::shared_ptr<memory::MemoryPool> pool{rootPool->addLeafChild("leaf")};

  for (const auto& table : tpch::tables) {
    const auto tableName = tpch::toTableName(table);
    const auto tableSchema = tpch::getTableSchema(table);

    int32_t numSplits = 1;
    if (tableName != "nation" && tableName != "region" &&
        FLAGS_tpch_scale > 1) {
      numSplits = std::min<int32_t>(FLAGS_tpch_scale, 200);
    }

    const auto tableDirectory = fmt::format("{}/{}", createPath_, tableName);
    auto plan =
        PlanBuilder()
            .tpchTableScan(table, tableSchema->names(), FLAGS_tpch_scale)
            .startTableWriter()
            .outputDirectoryPath(tableDirectory)
            .fileFormat(dwio::common::FileFormat::PARQUET)
            .compressionKind(common::CompressionKind::CompressionKind_SNAPPY)
            .endTableWriter()
            .planNode();

    std::vector<std::shared_ptr<connector::ConnectorSplit>> splits;
    for (auto i = 0; i < numSplits; ++i) {
      splits.push_back(std::make_shared<connector::tpch::TpchConnectorSplit>(
          std::string(PlanBuilder::kTpchDefaultConnectorId), numSplits, i));
    }

    const int32_t numDrivers =
        std::min<int32_t>(numSplits, std::thread::hardware_concurrency());

    LOG(INFO) << "Creating TPC-H table " << tableName
              << " scaleFactor=" << FLAGS_tpch_scale
              << " numSplits=" << numSplits << " numDrivers=" << numDrivers
              << " hw concurrency=" << std::thread::hardware_concurrency();
    auto rows = AssertQueryBuilder(plan)
                    .splits(std::move(splits))
                    .maxDrivers(numDrivers)
                    .copyResults(pool.get());
  }
}

} // namespace facebook::velox::optimizer::test
