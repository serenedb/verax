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

#include <folly/init/Init.h>
#include <gtest/gtest.h>
#include "axiom/optimizer/tests/ParquetTpchTest.h"
#include "axiom/optimizer/tests/QueryTestBase.h"
#include "velox/exec/tests/utils/TpchQueryBuilder.h"

DEFINE_int32(num_repeats, 1, "Number of repeats for optimization timing");

DECLARE_int32(optimizer_trace);
DECLARE_int32(num_workers);
DECLARE_string(history_save_path);

namespace facebook::velox::optimizer {
namespace {

class TpchPlanTest : public virtual test::ParquetTpchTest,
                     public virtual test::QueryTestBase {
 protected:
  static void SetUpTestCase() {
    ParquetTpchTest::SetUpTestCase();
    LocalRunnerTestBase::testDataPath_ = FLAGS_data_path;
    LocalRunnerTestBase::localFileFormat_ = "parquet";
    connector::unregisterConnector(exec::test::kHiveConnectorId);
    connector::unregisterConnectorFactory("hive");
    LocalRunnerTestBase::SetUpTestCase();
  }

  static void TearDownTestCase() {
    if (!FLAGS_history_save_path.empty()) {
      suiteHistory().saveToFile(FLAGS_history_save_path);
    }
    LocalRunnerTestBase::TearDownTestCase();
    ParquetTpchTest::TearDownTestCase();
  }

  void SetUp() override {
    ParquetTpchTest::SetUp();
    QueryTestBase::SetUp();
    allocator_ = std::make_unique<HashStringAllocator>(pool_.get());
    context_ = std::make_unique<QueryGraphContext>(*allocator_);
    queryCtx() = context_.get();
    builder_ = std::make_unique<exec::test::TpchQueryBuilder>(
        dwio::common::FileFormat::PARQUET, true);
    builder_->initialize(FLAGS_data_path);
    referenceBuilder_ = std::make_unique<exec::test::TpchQueryBuilder>(
        dwio::common::FileFormat::PARQUET);
    referenceBuilder_->initialize(FLAGS_data_path);
  }

  void TearDown() override {
    context_.reset();
    queryCtx() = nullptr;
    allocator_.reset();
    ParquetTpchTest::TearDown();
    QueryTestBase::TearDown();
  }

  void checkSameTpch(
      const core::PlanNodePtr& planNode,
      core::PlanNodePtr referencePlan = nullptr,
      std::string* planString = nullptr) {
    auto fragmentedPlan = planVelox(planNode, planString);

    auto reference = referencePlan ? referencePlan : planNode;
    test::TestResult referenceResult;
    assertSame(reference, fragmentedPlan, &referenceResult);

    const auto numWorkers = FLAGS_num_workers;
    if (numWorkers != 1) {
      gflags::FlagSaver saver;
      FLAGS_num_workers = 1;

      auto singlePlan = planVelox(planNode, planString);
      ASSERT_TRUE(singlePlan.plan != nullptr);
      auto singleResult = runFragmentedPlan(singlePlan);
      exec::test::assertEqualResults(
          referenceResult.results, singleResult.results);
    }
  }

  void checkTpch(int32_t query, const std::string& expected = "") {
    auto q = builder_->getQueryPlan(query).plan;
    auto rq = referenceBuilder_->getQueryPlan(query).plan;
    std::string planText;
    checkSameTpch(q, rq, &planText);
    if (!expected.empty()) {
      expectPlan(planText, expected);
    } else {
      std::cout << " -- plan = " << planText << std::endl;
    }
  }

  std::unique_ptr<HashStringAllocator> allocator_;
  std::unique_ptr<QueryGraphContext> context_;
  std::unique_ptr<exec::test::TpchQueryBuilder> builder_;
  std::unique_ptr<exec::test::TpchQueryBuilder> referenceBuilder_;
};

TEST_F(TpchPlanTest, q1) {
  checkTpch(1);
}

TEST_F(TpchPlanTest, DISABLED_q2) {
  checkTpch(2);
}

TEST_F(TpchPlanTest, q3) {
  checkTpch(
      3,
      "lineitem t3 project 3 columns *H  (orders t5 project 4 columns   Build )*H  (customer t7 project 1 columns   Build ) PARTIAL agg FINAL agg project 4 columns");
}
TEST_F(TpchPlanTest, DISABLED_q4) {
  // Incorrect with distributed plan at larger scales.
  checkTpch(4);
}

TEST_F(TpchPlanTest, q5) {
  checkTpch(5);
}

TEST_F(TpchPlanTest, q6) {
  checkTpch(6);
}

TEST_F(TpchPlanTest, q7) {
  checkTpch(7);
}

TEST_F(TpchPlanTest, q8) {
  checkTpch(8);
}

TEST_F(TpchPlanTest, q9) {
  // Plan does not minimize build size. To adjust build cost and check that
  // import of existences to build side does not affect join cardinality.
  checkTpch(9);
}

TEST_F(TpchPlanTest, q10) {
  checkTpch(10);
}

TEST_F(TpchPlanTest, q11) {
  checkTpch(11);
}

TEST_F(TpchPlanTest, q12) {
  // Fix string in filter
  checkTpch(12);
}

TEST_F(TpchPlanTest, q13) {
  checkTpch(13);
}

TEST_F(TpchPlanTest, q14) {
  checkTpch(14);
}

TEST_F(TpchPlanTest, DISABLED_q15) {
  checkTpch(15);
}

TEST_F(TpchPlanTest, DISABLED_q16) {
  checkTpch(16);
}

TEST_F(TpchPlanTest, DISABLED_q17) {
  checkTpch(17);
}

TEST_F(TpchPlanTest, DISABLED_q18) {
  checkTpch(18);
}

TEST_F(TpchPlanTest, q19) {
  checkTpch(19);
}

TEST_F(TpchPlanTest, DISABLED_q20) {
  checkTpch(20);
}

TEST_F(TpchPlanTest, DISABLED_q21) {
  checkTpch(21);
}

TEST_F(TpchPlanTest, DISABLED_q22) {
  checkTpch(22);
}

} // namespace
} // namespace facebook::velox::optimizer

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  folly::Init init(&argc, &argv, false);
  return RUN_ALL_TESTS();
}
