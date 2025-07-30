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

#include "axiom/logical_plan/PlanPrinter.h"
#include "axiom/logical_plan/ExprPrinter.h"
#include "axiom/logical_plan/PlanNodeVisitor.h"

namespace facebook::velox::logical_plan {

namespace {

class ToTextVisitor : public PlanNodeVisitor {
 public:
  struct Context : public PlanNodeVisitorContext {
    std::stringstream out;
    int32_t indent{0};
  };

  void visit(const ValuesNode& node, PlanNodeVisitorContext& context)
      const override {
    appendNode(
        "Values", node, fmt::format("{} rows", node.rows().size()), context);
  }

  void visit(const TableScanNode& node, PlanNodeVisitorContext& context)
      const override {
    appendNode(
        "TableScan",
        node,
        fmt::format("{}.{}", node.connectorId(), node.tableName()),
        context);
  }

  void visit(const FilterNode& node, PlanNodeVisitorContext& context)
      const override {
    appendNode("Filter", node, ExprPrinter::toText(*node.predicate()), context);
  }

  void visit(const ProjectNode& node, PlanNodeVisitorContext& context)
      const override {
    auto& myContext = static_cast<Context&>(context);
    myContext.out << makeIndent(myContext.indent) << "- Project:";

    appendOutputType(node, myContext);

    myContext.out << std::endl;

    const auto size = node.names().size();

    const auto indent = makeIndent(myContext.indent + 2);

    for (auto i = 0; i < size; ++i) {
      myContext.out << indent << node.names().at(i)
                    << " := " << ExprPrinter::toText(*node.expressions().at(i))
                    << std::endl;
    }

    appendInputs(node, myContext);
  }

  void visit(const AggregateNode& node, PlanNodeVisitorContext& context)
      const override {
    auto& myContext = static_cast<Context&>(context);
    myContext.out << makeIndent(myContext.indent) << "- Aggregate(";

    const auto numKeys = node.groupingKeys().size();
    for (auto i = 0; i < numKeys; ++i) {
      if (i > 0) {
        myContext.out << ", ";
      }
      myContext.out << ExprPrinter::toText(*node.groupingKeys().at(i));
    }

    appendOutputType(node, myContext);

    myContext.out << std::endl;

    const auto size = node.aggregates().size();

    const auto indent = makeIndent(myContext.indent + 2);

    for (auto i = 0; i < size; ++i) {
      myContext.out << indent << node.outputNames().at(numKeys + i)
                    << " := " << ExprPrinter::toText(*node.aggregates().at(i))
                    << std::endl;
    }

    appendInputs(node, myContext);
  }

  void visit(const JoinNode& node, PlanNodeVisitorContext& context)
      const override {
    appendNode(
        fmt::format("Join {}", JoinTypeName::toName(node.joinType())),
        node,
        node.condition() != nullptr ? ExprPrinter::toText(*node.condition())
                                    : "",
        context);
  }

  void visit(const SortNode& node, PlanNodeVisitorContext& context)
      const override {
    auto& myContext = static_cast<Context&>(context);
    myContext.out << makeIndent(myContext.indent) << "- Sort: ";

    const auto size = node.ordering().size();
    for (auto i = 0; i < size; ++i) {
      if (i > 0) {
        myContext.out << ", ";
      }
      myContext.out << ExprPrinter::toText(*node.ordering().at(i).expression)
                    << " " << node.ordering().at(i).order.toString();
    }

    appendOutputType(node, myContext);
    myContext.out << std::endl;
    appendInputs(node, myContext);
  }

  void visit(const LimitNode& node, PlanNodeVisitorContext& context)
      const override {
    appendNode(
        "Limit",
        node,
        node.offset() > 0
            ? fmt::format("{} (offset: {})", node.count(), node.offset())
            : fmt::format("{}", node.count()),
        context);
  }

  void visit(const SetNode& node, PlanNodeVisitorContext& context)
      const override {
    appendNode(SetOperationName::toName(node.operation()), node, context);
  }

  void visit(const UnnestNode& node, PlanNodeVisitorContext& context)
      const override {
    appendNode("Unnest", node, context);
  }

 private:
  static std::string makeIndent(int32_t size) {
    return std::string(size * 2, ' ');
  }

  void appendNode(
      std::string_view name,
      const LogicalPlanNode& node,
      const std::optional<std::string>& details,
      PlanNodeVisitorContext& context) const {
    auto& myContext = static_cast<Context&>(context);
    myContext.out << makeIndent(myContext.indent) << "- " << name << ":";

    if (details.has_value()) {
      myContext.out << " " << details.value();
    }

    appendOutputType(node, myContext);

    myContext.out << std::endl;

    appendInputs(node, myContext);
  }

  void appendNode(
      std::string_view name,
      const LogicalPlanNode& node,
      PlanNodeVisitorContext& context) const {
    appendNode(name, node, std::nullopt, context);
  }

  void appendOutputType(const LogicalPlanNode& node, Context& context) const {
    context.out << " -> " << node.outputType()->toString();
  }

  void appendInputs(const LogicalPlanNode& node, Context& context) const {
    context.indent++;
    PlanNodeVisitor::visitInputs(node, context);
    context.indent--;
  }
};

std::string truncate(const std::string& str, size_t maxLength = 50) {
  if (str.size() > maxLength) {
    return str.substr(0, maxLength) + "...";
  }
  return str;
}

class ExprStats : public ExprVisitorContext {
 public:
  std::unordered_map<std::string, int64_t>& functionCounts() {
    return functionCounts_;
  }

  const std::unordered_map<std::string, int64_t>& functionCounts() const {
    return functionCounts_;
  }

  std::unordered_map<std::string, int64_t>& expressionCounts() {
    return expressionCounts_;
  }

  const std::unordered_map<std::string, int64_t>& expressionCounts() const {
    return expressionCounts_;
  }

  std::unordered_map<velox::TypePtr, int64_t>& constantCounts() {
    return constantCounts_;
  }

  const std::unordered_map<velox::TypePtr, int64_t>& constantCounts() const {
    return constantCounts_;
  }

 private:
  std::unordered_map<std::string, int64_t> functionCounts_;
  std::unordered_map<std::string, int64_t> expressionCounts_;
  std::unordered_map<velox::TypePtr, int64_t> constantCounts_;
};

class SummarizeExprVisitor : public ExprVisitor {
 public:
  using Context = ExprStats;

  void visit(const InputReferenceExpr& expr, ExprVisitorContext& ctx)
      const override {
    auto& myCtx = static_cast<Context&>(ctx);
    myCtx.expressionCounts()["field"]++;

    visitInputs(expr, ctx);
  }

  void visit(const CallExpr& expr, ExprVisitorContext& ctx) const override {
    const auto& name = expr.name();

    auto& myCtx = static_cast<Context&>(ctx);
    myCtx.expressionCounts()["call"]++;

    auto& counts = myCtx.functionCounts();
    counts[name]++;

    visitInputs(expr, ctx);
  }

  void visit(const SpecialFormExpr& expr, ExprVisitorContext& ctx)
      const override {
    auto& myCtx = static_cast<Context&>(ctx);
    myCtx.expressionCounts()[std::string(
        SpecialFormName::toName(expr.form()))]++;
    visitInputs(expr, ctx);
  }

  void visit(const AggregateExpr& expr, ExprVisitorContext& ctx)
      const override {
    VELOX_NYI();
  }

  void visit(const WindowExpr& expr, ExprVisitorContext& ctx) const override {
    VELOX_NYI();
  }

  void visit(const ConstantExpr& expr, ExprVisitorContext& ctx) const override {
    auto& myCtx = static_cast<Context&>(ctx);
    myCtx.expressionCounts()["constant"]++;
    myCtx.constantCounts()[expr.type()]++;
  }

  void visit(const LambdaExpr& expr, ExprVisitorContext& ctx) const override {
    VELOX_NYI();
  }

  void visit(const SubqueryExpr& expr, ExprVisitorContext& ctx) const override {
    VELOX_NYI();
  }
};

class SummarizeToTextVisitor : public PlanNodeVisitor {
 public:
  struct Context : public PlanNodeVisitorContext {
    const PlanSummaryOptions options;

    std::stringstream out;
    int32_t indent{0};

    explicit Context(const PlanSummaryOptions& _options) : options(_options) {}
  };

  void visit(const ValuesNode& node, PlanNodeVisitorContext& context)
      const override {
    auto& myContext = static_cast<Context&>(context);
    appendHeader(node, myContext);

    const auto indent = makeIndent(myContext.indent + 3);
    myContext.out << indent << "rows: " << node.rows().size() << std::endl;
  }

  void visit(const TableScanNode& node, PlanNodeVisitorContext& context)
      const override {
    auto& myContext = static_cast<Context&>(context);
    appendHeader(node, myContext);

    const auto indent = makeIndent(myContext.indent + 3);

    myContext.out << indent << "table: " << node.tableName() << std::endl;
    myContext.out << indent << "connector: " << node.connectorId() << std::endl;
  }

  void visit(const FilterNode& node, PlanNodeVisitorContext& context)
      const override {
    auto& myContext = static_cast<Context&>(context);
    appendHeader(node, myContext);

    const auto indent = makeIndent(myContext.indent + 3);
    myContext.out << indent << "predicate: "
                  << truncate(ExprPrinter::toText(*node.predicate()))
                  << std::endl;

    appendExpressions(std::vector<ExprPtr>{node.predicate()}, myContext);

    appendInputs(node, myContext);
  }

  void visit(const ProjectNode& node, PlanNodeVisitorContext& context)
      const override {
    auto& myContext = static_cast<Context&>(context);
    appendHeader(node, myContext);
    appendExpressions(node.expressions(), myContext);
    appendInputs(node, myContext);
  }

  void visit(const AggregateNode& node, PlanNodeVisitorContext& context)
      const override {
    appendNode(node, context);
  }

  void visit(const JoinNode& node, PlanNodeVisitorContext& context)
      const override {
    auto& myContext = static_cast<Context&>(context);
    appendHeader(
        fmt::format("Join {}", JoinTypeName::toName(node.joinType())),
        node,
        myContext);

    const auto indent = makeIndent(myContext.indent + 3);
    if (node.condition() != nullptr) {
      myContext.out << indent << "condition: "
                    << truncate(ExprPrinter::toText(*node.condition()))
                    << std::endl;
    }

    appendInputs(node, myContext);
  }

  void visit(const SortNode& node, PlanNodeVisitorContext& context)
      const override {
    appendNode(node, context);
  }

  void visit(const LimitNode& node, PlanNodeVisitorContext& context)
      const override {
    auto& myContext = static_cast<Context&>(context);
    appendHeader(node, myContext);

    const auto indent = makeIndent(myContext.indent + 3);
    myContext.out << indent << "limit: " << node.count();
    if (node.offset() > 0) {
      myContext.out << " offset: " << node.offset();
    }
    myContext.out << std::endl;

    appendInputs(node, myContext);
  }

  void visit(const SetNode& node, PlanNodeVisitorContext& context)
      const override {
    appendNode(
        std::string(SetOperationName::toName(node.operation())), node, context);
  }

  void visit(const UnnestNode& node, PlanNodeVisitorContext& context)
      const override {
    appendNode(node, context);
  }

 private:
  static std::string makeIndent(int32_t size) {
    return std::string(size * 2, ' ');
  }

  void appendNode(const LogicalPlanNode& node, PlanNodeVisitorContext& context)
      const {
    appendNode(std::string(NodeKindName::toName(node.kind())), node, context);
  }

  void appendHeader(const LogicalPlanNode& node, Context& context) const {
    appendHeader(std::string(NodeKindName::toName(node.kind())), node, context);
  }

  void appendNode(
      const std::string& name,
      const LogicalPlanNode& node,
      PlanNodeVisitorContext& context) const {
    auto& myContext = static_cast<Context&>(context);
    appendHeader(name, node, myContext);
    appendInputs(node, myContext);
  }

  void appendHeader(
      const std::string& name,
      const LogicalPlanNode& node,
      Context& context) const {
    context.out << makeIndent(context.indent) << "- " << name << " ["
                << node.id() << "]: "
                << summarizeOutputType(node.outputType(), context.options)
                << std::endl;
  }

  void appendInputs(const LogicalPlanNode& node, Context& context) const {
    context.indent++;
    PlanNodeVisitor::visitInputs(node, context);
    context.indent--;
  }

  void appendExpressions(
      const std::vector<ExprPtr>& expressions,
      Context& context) const {
    const auto indent = makeIndent(context.indent + 3);

    SummarizeExprVisitor::Context exprCtx;
    SummarizeExprVisitor visitor;
    for (const auto& expression : expressions) {
      expression->accept(visitor, exprCtx);
    }

    appendExpressionStats(exprCtx, indent, context.options, context.out);
  }

  // @param sortByKey Indicated whether to sort counts by key asc (default) or
  // 'count' desc.
  static void appendCounts(
      const std::unordered_map<std::string, int64_t>& counts,
      std::ostream& text,
      bool sortByKey = true) {
    std::vector<std::pair<std::string, int64_t>> sortedCounts;
    sortedCounts.reserve(counts.size());
    for (const auto& [key, count] : counts) {
      sortedCounts.emplace_back(key, count);
    }

    if (sortByKey) {
      std::sort(
          sortedCounts.begin(),
          sortedCounts.end(),
          [&](const auto& a, const auto& b) { return a.first < b.first; });
    } else {
      std::sort(
          sortedCounts.begin(),
          sortedCounts.end(),
          [&](const auto& a, const auto& b) { return a.second > b.second; });
    }

    bool first = true;
    for (const auto& [key, count] : sortedCounts) {
      if (first) {
        first = false;
      } else {
        text << ", ";
      }
      text << key << ": " << count;
    }
  }

  static void appendExpressionStats(
      const ExprStats& stats,
      const std::string& indent,
      const PlanSummaryOptions& options,
      std::ostream& out) {
    out << indent << "expressions: ";
    appendCounts(stats.expressionCounts(), out);
    out << std::endl;

    if (!stats.functionCounts().empty()) {
      out << indent << "functions: ";
      appendCounts(stats.functionCounts(), out);
      out << std::endl;
    }

    if (!stats.constantCounts().empty()) {
      out << indent << "constants: ";
      std::unordered_map<std::string, int64_t> counts;
      for (const auto& [type, count] : stats.constantCounts()) {
        counts[type->toSummaryString(
            {.maxChildren = (uint32_t)options.maxChildTypes})] += count;
      }

      appendCounts(counts, out);
      out << std::endl;
    }
  }

  static std::string summarizeOutputType(
      const velox::RowTypePtr& type,
      const PlanSummaryOptions& options) {
    std::ostringstream out;
    out << type->size() << " fields";

    // Include names and types for the first few fields.
    const auto cnt = std::min<size_t>(options.maxOutputFields, type->size());
    if (cnt > 0) {
      out << ": ";
      for (auto i = 0; i < cnt; ++i) {
        if (i > 0) {
          out << ", ";
        }
        out << type->nameOf(i) << " "
            << type->childAt(i)->toSummaryString(
                   {.maxChildren = (uint32_t)options.maxChildTypes});
      }

      if (cnt < type->size()) {
        out << ", ...";

        // TODO Include counts of fields by type kind.
      }
    }

    return out.str();
  }
};

} // namespace

// static
std::string PlanPrinter::toText(const LogicalPlanNode& root) {
  ToTextVisitor::Context context;
  ToTextVisitor visitor;
  root.accept(visitor, context);
  return context.out.str();
}

// static
std::string PlanPrinter::summarizeToText(
    const LogicalPlanNode& root,
    const PlanSummaryOptions& options) {
  SummarizeToTextVisitor::Context context{options};
  SummarizeToTextVisitor visitor;
  root.accept(visitor, context);
  return context.out.str();
}

} // namespace facebook::velox::logical_plan
