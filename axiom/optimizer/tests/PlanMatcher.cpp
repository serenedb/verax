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

#include "axiom/optimizer/tests/PlanMatcher.h"
#include <gtest/gtest.h>

namespace facebook::velox::core {
namespace {

template <typename T = PlanNode>
class PlanMatcherImpl : public PlanMatcher {
 public:
  PlanMatcherImpl() = default;

  PlanMatcherImpl(
      const std::vector<std::shared_ptr<PlanMatcher>>& sourceMatchers)
      : sourceMatchers_{sourceMatchers} {}

  bool match(const PlanNodePtr& plan) const override {
    const auto* specificNode = dynamic_cast<const T*>(plan.get());

    EXPECT_TRUE(specificNode != nullptr)
        << "Expected " << folly::demangle(typeid(T).name()) << ", but got "
        << plan->toString(false, false);
    if (::testing::Test::HasNonfatalFailure()) {
      return false;
    }

    EXPECT_EQ(plan->sources().size(), sourceMatchers_.size());
    if (::testing::Test::HasNonfatalFailure()) {
      return false;
    }

    for (auto i = 0; i < sourceMatchers_.size(); ++i) {
      EXPECT_TRUE(sourceMatchers_[i]->match(plan->sources()[i]));
      if (::testing::Test::HasNonfatalFailure()) {
        return false;
      }
    }

    return matchDetails(*specificNode);
  }

 private:
  virtual bool matchDetails(const T& plan) const {
    return true;
  }

  const std::vector<std::shared_ptr<PlanMatcher>> sourceMatchers_;
};

} // namespace

PlanMatcherBuilder& PlanMatcherBuilder::tableScan() {
  VELOX_USER_CHECK_NULL(matcher_);
  matcher_ = std::make_shared<PlanMatcherImpl<TableScanNode>>();
  return *this;
}

PlanMatcherBuilder& PlanMatcherBuilder::filter() {
  VELOX_USER_CHECK_NOT_NULL(matcher_);
  matcher_ = std::make_shared<PlanMatcherImpl<FilterNode>>(
      std::vector<std::shared_ptr<PlanMatcher>>{matcher_});
  return *this;
}

PlanMatcherBuilder& PlanMatcherBuilder::project() {
  VELOX_USER_CHECK_NOT_NULL(matcher_);
  matcher_ = std::make_shared<PlanMatcherImpl<ProjectNode>>(
      std::vector<std::shared_ptr<PlanMatcher>>{matcher_});
  return *this;
}

PlanMatcherBuilder& PlanMatcherBuilder::aggregation() {
  VELOX_USER_CHECK_NOT_NULL(matcher_);
  matcher_ = std::make_shared<PlanMatcherImpl<AggregationNode>>(
      std::vector<std::shared_ptr<PlanMatcher>>{matcher_});
  return *this;
}

PlanMatcherBuilder& PlanMatcherBuilder::localPartition() {
  VELOX_USER_CHECK_NOT_NULL(matcher_);
  matcher_ = std::make_shared<PlanMatcherImpl<LocalPartitionNode>>(
      std::vector<std::shared_ptr<PlanMatcher>>{matcher_});
  return *this;
}

} // namespace facebook::velox::core
