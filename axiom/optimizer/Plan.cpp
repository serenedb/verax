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

#include "axiom/optimizer/Plan.h"
#include "axiom/optimizer/Cost.h"
#include "velox/functions/FunctionRegistry.h"

#include <iostream>

namespace facebook::velox::optimizer {

namespace {

/// True if single worker, i.e. do not plan remote exchanges
bool isSingleWorker() {
  return queryCtx()->optimization()->options().numWorkers == 1;
}

} // namespace

/// The dt for which we set a breakpoint for plan candidate.
int32_t debugDt{-1};

/// Number of tables in  'debugPlacedTables'
int32_t debugNumPlaced = 0;

/// Tables for setting a breakpoint. Join order selection calls planBreakpoint()
/// right before evaluating the cost for the tables in 'debugPlacedTables'.
int32_t debugPlaced[10];

void planBreakpoint() {
  // Set breakpoint here for looking at cost of join order in
  // 'debugPlacedTables'.
  LOG(INFO) << "Join order breakpoint";
}

void PlanState::setFirstTable(int32_t id) {
  if (dt->id() == debugDt) {
    debugPlacedTables.resize(1);
    debugPlacedTables[0] = id;
  }
}

PlanStateSaver::PlanStateSaver(PlanState& state, const JoinCandidate& candidate)
    : PlanStateSaver(state) {
  if (state.dt->id() != debugDt) {
    return;
  }
  state.debugPlacedTables.push_back(candidate.tables[0]->id());
  if (debugNumPlaced == 0) {
    return;
  }

  for (auto i = 0; i < debugNumPlaced; ++i) {
    if (debugPlaced[i] != state.debugPlacedTables[i]) {
      return;
    }
  }
  planBreakpoint();
}

Optimization::Optimization(
    const core::PlanNode& plan,
    const Schema& schema,
    History& history,
    std::shared_ptr<core::QueryCtx> _queryCtx,
    core::ExpressionEvaluator& evaluator,
    OptimizerOptions opts,
    runner::MultiFragmentPlan::Options options)
    : schema_(schema),
      opts_(std::move(opts)),
      inputPlan_(&plan),
      history_(history),
      queryCtx_(std::move(_queryCtx)),
      evaluator_(evaluator),
      options_(std::move(options)),
      isSingle_(options_.numWorkers == 1) {
  initialize();
}

Optimization::Optimization(
    const logical_plan::LogicalPlanNode& plan,
    const Schema& schema,
    History& history,
    std::shared_ptr<core::QueryCtx> _queryCtx,
    velox::core::ExpressionEvaluator& evaluator,
    OptimizerOptions opts,
    runner::MultiFragmentPlan::Options options)
    : schema_(schema),
      opts_(std::move(opts)),
      logicalPlan_(&plan),
      history_(history),
      queryCtx_(std::move(_queryCtx)),
      evaluator_(evaluator),
      options_(std::move(options)),
      isSingle_(options_.numWorkers == 1) {
  initialize();
}

void Optimization::initialize() {
  queryCtx()->optimization() = this;
  if (inputPlan_) {
    root_ = makeQueryGraph();
  } else {
    root_ = makeQueryGraphFromLogical();
  }
  root_->distributeConjuncts();
  root_->addImpliedJoins();
  root_->linkTablesToJoins();
  for (auto* join : root_->joins) {
    join->guessFanout();
  }
  if (inputPlan_) {
    setDerivedTableOutput(root_, *inputPlan_);
  } else {
    setDerivedTableOutput(root_, *logicalPlan_);
  }
}

void Optimization::trace(
    int32_t event,
    int32_t id,
    const Cost& cost,
    RelationOp& plan) {
  if (event & opts_.traceFlags) {
    std::cout << (event == kRetained ? "Retained: " : "Abandoned: ") << id
              << ": " << cost.toString(true, true) << ": " << " "
              << plan.toString(true, false) << std::endl;
  }
}

PlanPtr Optimization::bestPlan() {
  topState_.dt = root_;
  PlanObjectSet targetColumns;
  targetColumns.unionColumns(root_->columns);
  topState_.setTargetColumnsForDt(targetColumns);
  makeJoins(nullptr, topState_);
  Distribution empty;
  bool ignore;
  return topState_.plans.best(empty, ignore);
}

std::unordered_map<std::string, float>& baseSelectivities() {
  static std::unordered_map<std::string, float> map;
  return map;
}

FunctionSet functionBits(Name name) {
  if (auto* md = functionMetadata(name)) {
    return md->functionSet;
  }
  auto deterministic = isDeterministic(name);
  if (deterministic.has_value() && !deterministic.value()) {
    return FunctionSet(FunctionSet::kNonDeterministic);
  }
  return FunctionSet(0);
}

Plan::Plan(RelationOpPtr _op, const PlanState& state)
    : op(_op),
      cost(state.cost),
      tables(state.placed),
      columns(state.targetColumns),
      fullyImported(state.dt->fullyImported) {}

bool Plan::isStateBetter(const PlanState& state, float perRowMargin) const {
  return cost.unitCost * cost.inputCardinality + cost.setupCost >
      state.cost.unitCost * state.cost.inputCardinality + state.cost.setupCost +
      perRowMargin * state.cost.fanout;
}

std::string Plan::printCost() const {
  return cost.toString(true, false);
}

std::string Plan::toString(bool detail) const {
  queryCtx()->contextPlan() = const_cast<Plan*>(this);
  auto result = op->toString(true, detail);
  queryCtx()->contextPlan() = nullptr;
  return result;
}

void PlanState::addCost(RelationOp& op) {
  if (!static_cast<bool>(op.cost().unitCost)) {
    op.setCost(*this);
  }
  cost.unitCost += cost.inputCardinality * cost.fanout * op.cost().unitCost;
  cost.setupCost += op.cost().setupCost;
  cost.fanout *= op.cost().fanout;
  cost.totalBytes += op.cost().totalBytes;
  cost.transferBytes += op.cost().transferBytes;
}

void PlanState::addNextJoin(
    const JoinCandidate* candidate,
    RelationOpPtr plan,
    BuildSet builds,
    std::vector<NextJoin>& toTry) const {
  if (!isOverBest()) {
    toTry.emplace_back(candidate, plan, cost, placed, columns, builds);
  } else {
    optimization.trace(Optimization::kExceededBest, dt->id(), cost, *plan);
  }
}

void PlanState::addBuilds(const BuildSet& added) {
  for (auto build : added) {
    if (std::find(builds.begin(), builds.end(), build) == builds.end()) {
      builds.push_back(build);
    }
  }
}

void PlanState::setTargetColumnsForDt(const PlanObjectSet& target) {
  targetColumns = target;
  for (auto i = 0; i < dt->columns.size(); ++i) {
    if (target.contains(dt->columns[i])) {
      targetColumns.unionColumns(dt->exprs[i]);
    }
  }
  for (auto& having : dt->having) {
    targetColumns.unionColumns(having);
  }
}

PlanObjectSet PlanState::downstreamColumns() const {
  auto it = downstreamPrecomputed.find(placed);
  if (it != downstreamPrecomputed.end()) {
    return it->second;
  }
  PlanObjectSet result;
  for (auto join : dt->joins) {
    bool addFilter = false;
    if (!placed.contains(join->rightTable())) {
      addFilter = true;
      result.unionColumns(join->leftKeys());
    }
    if (join->leftTable() && !placed.contains(join->leftTable())) {
      addFilter = true;
      result.unionColumns(join->rightKeys());
    }
    if (addFilter && !join->filter().empty()) {
      result.unionColumns(join->filter());
    }
  }
  for (auto& filter : dt->conjuncts) {
    if (!placed.contains(filter)) {
      result.unionColumns(filter);
    }
  }
  for (auto& conjunct : dt->conjuncts) {
    if (!placed.contains(conjunct)) {
      result.unionColumns(conjunct);
    }
  }
  if (dt->aggregation && !placed.contains(dt->aggregation)) {
    auto aggToPlace = dt->aggregation->aggregation;
    for (auto i = 0; i < aggToPlace->columns().size(); ++i) {
      // Grouping columns must be computed anyway, aggregates only if referenced
      // by enclosing.
      if (i < aggToPlace->grouping.size()) {
        result.unionColumns(aggToPlace->grouping[i]);
      } else if (targetColumns.contains(aggToPlace->columns()[i])) {
        result.unionColumns(
            aggToPlace->aggregates[i - aggToPlace->grouping.size()]);
      }
    }
  }
  result.unionSet(targetColumns);
  downstreamPrecomputed[placed] = result;
  return result;
}

std::string PlanState::printCost() const {
  return cost.toString(true, true);
}

std::string PlanState::printPlan(RelationOpPtr op, bool detail) const {
  auto plan = std::make_unique<Plan>(op, *this);
  return plan->toString(detail);
}

PlanPtr PlanSet::addPlan(RelationOpPtr plan, PlanState& state) {
  bool insert = plans.empty();
  int32_t replaceIndex = -1;
  float shuffle = shuffleCost(plan->columns()) * state.cost.fanout;
  if (!insert) {
    // Compare with existing. If there is one with same distribution
    // and new is better, replace. If there is one with a different
    // distribution and the new one can produce the same distribution
    // by repartition, for cheaper, add the new one and delete the old
    // one.
    for (auto i = 0; i < plans.size(); ++i) {
      auto old = plans[i].get();
      if (!(state.input == old->input)) {
        continue;
      }
      bool newIsBetter = old->isStateBetter(state);
      bool newIsBetterWithShuffle = old->isStateBetter(state, shuffle);
      bool sameDist =
          old->op->distribution().isSamePartition(plan->distribution());
      bool sameOrder =
          old->op->distribution().isSameOrder(plan->distribution());
      if (sameDist && sameOrder) {
        if (newIsBetter) {
          replaceIndex = i;
          continue;
        }
        // There's a better one with same dist and partition.
        return nullptr;
      }
      if (newIsBetterWithShuffle && old->op->distribution().order.empty()) {
        // Old plan has no order and is worse than new plus shuffle. Can't win.
        // rase.
        queryCtx()->optimization()->trace(
            Optimization::kExceededBest, state.dt->id(), old->cost, *old->op);
        plans.erase(plans.begin() + i);
        --i;
        continue;
      }
      if (plan->distribution().order.empty() &&
          !old->isStateBetter(state, -shuffle)) {
        // New has no order and old would beat it even after adding shuffle.
        return nullptr;
      }
    }
  }
  auto newPlan = std::make_unique<Plan>(plan, state);
  auto* result = newPlan.get();
  auto newPlanCost = result->cost.unitCost + result->cost.setupCost + shuffle;
  if (bestCostWithShuffle == 0 || newPlanCost < bestCostWithShuffle) {
    bestCostWithShuffle = newPlanCost;
  }
  if (replaceIndex >= 0) {
    plans[replaceIndex] = std::move(newPlan);
  } else {
    plans.push_back(std::move(newPlan));
  }
  return result;
}

PlanPtr PlanSet::best(const Distribution& distribution, bool& needsShuffle) {
  PlanPtr best = nullptr;
  PlanPtr match = nullptr;
  float bestCost = -1;
  float matchCost = -1;
  bool single = isSingleWorker();
  for (auto i = 0; i < plans.size(); ++i) {
    float cost = plans[i]->cost.fanout * plans[i]->cost.unitCost +
        plans[i]->cost.setupCost;
    if (!best || bestCost > cost) {
      best = plans[i].get();
      bestCost = cost;
    }
    if (single || plans[i]->op->distribution().isSamePartition(distribution)) {
      match = plans[i].get();
      matchCost = cost;
    }
  }
  if (best != match && match) {
    float shuffle = shuffleCost(best->op->columns()) * best->cost.fanout;
    if (bestCost + shuffle < matchCost) {
      needsShuffle = true;
      assert(best != nullptr);
      return best;
    }
  }
  needsShuffle = best != match;
  assert(best != nullptr);
  return best;
}

float startingScore(PlanObjectCP table) {
  if (table->type() == PlanType::kTable) {
    return table->as<BaseTable>()
        ->schemaTable->columnGroups[0]
        ->distribution()
        .cardinality;
  } else if (table->type() == PlanType::kValuesTable) {
    return table->as<ValuesTable>()->cardinality();
  }
  return 10;
}

const JoinEdgeVector& joinedBy(PlanObjectCP table) {
  if (table->type() == PlanType::kTable) {
    return table->as<BaseTable>()->joinedBy;
  } else if (table->type() == PlanType::kValuesTable) {
    return table->as<ValuesTable>()->joinedBy;
  }
  VELOX_DCHECK_EQ(table->type(), PlanType::kDerivedTable);
  return table->as<DerivedTable>()->joinedBy;
}

// Traverses joins from 'candidate'. Follows any join that goes to a
// table not in 'visited' with a fanout <
// 'maxFanout'. 'fanoutFromRoot' is the product of the fanouts
// between 'candidate' and the 'candidate' of the top level call to
// this. 'path' is the set of joined tables between this invocation
// and the top level. 'fanoutFromRoot' is thus the selectivity of
// the linear join sequence in 'path'.  When a reducing join
// sequence is found, the tables on the path are added to
// 'result'. 'reduction' is the product of the fanouts of all the
// reducing join paths added to 'result'.
void reducingJoinsRecursive(
    const PlanState& state,
    PlanObjectCP candidate,
    float fanoutFromRoot,
    float maxFanout,
    std::vector<PlanObjectCP>& path,
    PlanObjectSet& visited,
    PlanObjectSet& result,
    float& reduction,
    std::function<void(const std::vector<PlanObjectCP>& path, float reduction)>
        resultFunc = nullptr) {
  bool isLeaf = true;
  for (auto join : joinedBy(candidate)) {
    if (join->leftOptional() || join->rightOptional()) {
      continue;
    }
    JoinSide other = join->sideOf(candidate, true);
    if (!state.dt->tableSet.contains(other.table) || !state.dt->hasJoin(join)) {
      continue;
    }
    if (other.table->type() != PlanType::kTable &&
        other.table->type() != PlanType::kValuesTable) {
      continue;
    }
    if (visited.contains(other.table)) {
      continue;
    }
    if (other.fanout > maxFanout) {
      continue;
    }
    visited.add(other.table);
    auto fanout = fanoutFromRoot * other.fanout;
    if (fanout < 0.9) {
      result.add(other.table);
      for (auto step : path) {
        result.add(step);
        maxFanout = 1;
      }
    }
    path.push_back(other.table);
    isLeaf = false;
    reducingJoinsRecursive(
        state,
        other.table,
        fanout,
        maxFanout,
        path,
        visited,
        result,
        reduction,
        resultFunc);
    path.pop_back();
  }
  if (fanoutFromRoot < 1 && isLeaf) {
    // We are at the end of a reducing sequence of joins. Update the total
    // fanout for the set of all reducing join paths from the top level
    // 'candidate'.
    reduction *= fanoutFromRoot;
    if (resultFunc) {
      resultFunc(path, fanoutFromRoot);
    }
  }
}

JoinCandidate reducingJoins(
    const PlanState& state,
    const JoinCandidate& candidate) {
  // For an inner join, see if can bundle reducing joins on the build.
  JoinCandidate reducing;
  reducing.join = candidate.join;
  reducing.fanout = candidate.fanout;
  PlanObjectSet reducingSet;
  if (candidate.join->isInner()) {
    PlanObjectSet visited = state.placed;
    assert(!candidate.tables.empty()); // lint
    visited.add(candidate.tables[0]);
    reducingSet.add(candidate.tables[0]);
    std::vector<PlanObjectCP> path{candidate.tables[0]};
    float reduction = 1;
    reducingJoinsRecursive(
        state,
        candidate.tables[0],
        1,
        1.2,
        path,
        visited,
        reducingSet,
        reduction);
    if (reduction < 0.9) {
      // The only table in 'candidate' must be first in the bushy table list.
      reducing.tables = candidate.tables;
      reducingSet.forEach([&](auto object) {
        if (object != reducing.tables[0]) {
          reducing.tables.push_back(object);
        }
      });
      reducing.fanout = candidate.fanout * reduction;
    }
  }
  if (!state.dt->noImportOfExists) {
    PlanObjectSet exists;
    float reduction = 1;
    assert(!candidate.tables.empty());
    std::vector<PlanObjectCP> path{candidate.tables[0]};
    // Look for reducing joins that were not added before, also covering already
    // placed tables. This may copy reducing joins from a probe to the
    // corresponding build.
    reducingSet.add(candidate.tables[0]);
    reducingSet.unionSet(state.dt->importedExistences);
    reducingJoinsRecursive(
        state,
        candidate.tables[0],
        1,
        1.2,
        path,
        reducingSet,
        exists,
        reduction,
        [&](auto& path, float reduction) {
          if (reduction < 0.7) {
            // The original table is added to the reducing existences because
            // the path starts with it but it is not joined twice since it
            // already is the start of the main join.
            PlanObjectSet added;
            for (auto i = 1; i < path.size(); ++i) {
              added.add(path[i]);
            }
            reducing.existences.push_back(std::move(added));
          }
        });
  }
  if (reducing.tables.empty() && reducing.existences.empty()) {
    // No reduction.
    return JoinCandidate{};
  }
  if (reducing.tables.empty()) {
    // No reducing joins but reducing existences from probe side.
    reducing.tables = candidate.tables;
  }
  return reducing;
}

// Calls 'func' with join, joined table and fanout for the joinable tables.
template <typename Func>
void forJoinedTables(const PlanState& state, Func func) {
  std::unordered_set<JoinEdgeP> visited;
  state.placed.forEach([&](PlanObjectCP placedTable) {
    if (!placedTable->isTable()) {
      return;
    }
    for (auto join : joinedBy(placedTable)) {
      if (join->isNonCommutative()) {
        if (!visited.insert(join).second) {
          continue;
        }
        bool usable = true;
        for (auto key : join->leftKeys()) {
          if (!key->allTables().isSubset(state.placed)) {
            // All items that the left key depends on must be placed.
            usable = false;
            break;
          }
        }
        if (usable) {
          func(join, join->rightTable(), join->lrFanout());
        }
      } else {
        auto [table, fanout] = join->otherTable(placedTable);
        if (!state.dt->tableSet.contains(table)) {
          continue;
        }
        func(join, table, fanout);
      }
    }
  });
}

JoinSide JoinCandidate::sideOf(PlanObjectCP side, bool other) const {
  return join->sideOf(side, other);
}

bool hasEqual(ExprCP key, const ExprVector& keys) {
  if (key->type() != PlanType::kColumn || !key->as<Column>()->equivalence()) {
    return false;
  }
  for (auto& e : keys) {
    if (key->sameOrEqual(*e)) {
      return true;
    }
  }
  return false;
}

void JoinCandidate::addEdge(PlanState& state, JoinEdgeP edge) {
  auto* joined = tables[0];
  auto newTableSide = edge->sideOf(joined);
  auto newPlacedSide = edge->sideOf(joined, true);
  VELOX_CHECK_NOT_NULL(newPlacedSide.table);
  if (!state.placed.contains(newPlacedSide.table)) {
    return;
  }
  auto tableSide = join->sideOf(joined);
  auto placedSide = join->sideOf(joined, true);
  for (auto i = 0; i < newPlacedSide.keys.size(); ++i) {
    auto* key = newPlacedSide.keys[i];
    if (!hasEqual(key, tableSide.keys)) {
      if (!compositeEdge) {
        compositeEdge = make<JoinEdge>(*join);
        join = compositeEdge;
      }
      auto [other, preFanout] = join->otherTable(placedSide.table);
      // do not recompute a fanout after adding more equalities. This makes the
      // join edge non-binary and it cannot be sampled.
      join->setFanouts(join->rlFanout(), join->lrFanout());
      if (joined == join->rightTable()) {
        join->addEquality(key, newTableSide.keys[i]);
      } else {
        join->addEquality(newTableSide.keys[i], key);
      }
      auto [other2, postFanout] = join->otherTable(placedSide.table);
      auto change = postFanout > 0 ? preFanout / postFanout : 0;
      fanout = change > 0 ? fanout / change : preFanout / 2;
    }
  }
}

bool JoinCandidate::isDominantEdge(PlanState& state, JoinEdgeP edge) {
  auto* joined = tables[0];
  auto newPlacedSide = edge->sideOf(joined, true);
  VELOX_CHECK_NOT_NULL(newPlacedSide.table);
  if (!state.placed.contains(newPlacedSide.table)) {
    return false;
  }
  auto tableSide = join->sideOf(joined);
  auto placedSide = join->sideOf(joined, true);
  for (auto i = 0; i < newPlacedSide.keys.size(); ++i) {
    auto* key = newPlacedSide.keys[i];
    if (!hasEqual(key, tableSide.keys)) {
      return false;
    }
  }
  return newPlacedSide.keys.size() > placedSide.keys.size();
}

std::string JoinCandidate::toString() const {
  std::stringstream out;
  out << join->toString() << " fanout " << fanout;
  for (auto i = 1; i < tables.size(); ++i) {
    out << " + " << tables[i]->toString();
  }
  if (!existences.empty()) {
    out << " exists " << existences[0].toString(false);
  }
  return out.str();
}

bool NextJoin::isWorse(const NextJoin& other) const {
  float shuffle =
      plan->distribution().isSamePartition(other.plan->distribution())
      ? 0
      : plan->cost().fanout * shuffleCost(plan->columns());
  return cost.unitCost + cost.setupCost + shuffle >
      other.cost.unitCost + other.cost.setupCost;
}

bool addExtraEdges(PlanState& state, JoinCandidate& candidate) {
  // See if there are more join edges from the first of 'candidate' to already
  // placed tables. Fill in the non-redundant equalities into the join edge.
  // Make a new edge if the edge would be altered.
  auto* originalJoin = candidate.join;
  auto* table = candidate.tables[0];
  for (auto* otherJoin : joinedBy(table)) {
    if (otherJoin == originalJoin || !otherJoin->isInner()) {
      continue;
    }
    auto [otherTable, fanout] = otherJoin->otherTable(table);
    if (!state.dt->tableSet.contains(otherTable)) {
      continue;
    }
    if (candidate.isDominantEdge(state, otherJoin)) {
      return false;
    }
    candidate.addEdge(state, otherJoin);
  }
  return true;
}

std::vector<JoinCandidate> Optimization::nextJoins(PlanState& state) {
  std::vector<JoinCandidate> candidates;
  candidates.reserve(state.dt->tables.size());
  forJoinedTables(
      state, [&](JoinEdgeP join, PlanObjectCP joined, float fanout) {
        if (!state.placed.contains(joined) && state.dt->hasJoin(join) &&
            state.dt->hasTable(joined)) {
          candidates.emplace_back(join, joined, fanout);
          if (join->isInner()) {
            if (!addExtraEdges(state, candidates.back())) {
              // Drop the candidate if the edge was a subsumed in some other
              // edge.
              candidates.pop_back();
            }
          }
        }
      });

  std::vector<JoinCandidate> bushes;
  // Take the  first hand joined tables and bundle them with reducing joins that
  // can go on the build side.
  for (auto& candidate : candidates) {
    auto bush = reducingJoins(state, candidate);
    if (!bush.tables.empty()) {
      bushes.push_back(std::move(bush));
    }
  }
  candidates.insert(candidates.begin(), bushes.begin(), bushes.end());
  std::sort(
      candidates.begin(),
      candidates.end(),
      [](const JoinCandidate& left, const JoinCandidate& right) {
        return left.fanout < right.fanout;
      });
  if (candidates.empty()) {
    // There are no join edges. There could still be cross joins.
    state.dt->startTables.forEach([&](PlanObjectCP object) {
      if (!state.placed.contains(object)) {
        candidates.emplace_back(nullptr, object, tableCardinality(object));
      }
    });
  }
  return candidates;
}

size_t MemoKey::hash() const {
  size_t hash = tables.hash();
  for (auto& exists : existences) {
    hash = bits::commutativeHashMix(hash, exists.hash());
  }
  return hash;
}

bool MemoKey::operator==(const MemoKey& other) const {
  if (firstTable == other.firstTable && columns == other.columns &&
      tables == other.tables) {
    if (existences.size() != other.existences.size()) {
      return false;
    }
    for (auto& e : existences) {
      for (auto& e2 : other.existences) {
        if (e2 == e) {
          break;
        }
      }
    }
    return true;
  }
  return false;
}

namespace {
constexpr uint32_t kNotFound = ~0U;

/// Returns index of 'expr' in collection 'exprs'. kNotFound if not found.
/// Compares with equivalence classes, so that equal columns are
/// interchangeable.
template <typename V>
uint32_t position(const V& exprs, const Expr& expr) {
  for (auto i = 0; i < exprs.size(); ++i) {
    if (exprs[i]->sameOrEqual(expr)) {
      return i;
    }
  }
  return kNotFound;
}

/// Returns index of 'expr' in collection 'exprs'. kNotFound if not found.
/// Compares with equivalence classes, so that equal columns are
/// interchangeable. Applies 'getter' to each element of 'exprs' before
/// comparison.
template <typename V, typename Getter>
uint32_t position(const V& exprs, Getter getter, const Expr& expr) {
  for (auto i = 0; i < exprs.size(); ++i) {
    if (getter(exprs[i])->sameOrEqual(expr)) {
      return i;
    }
  }
  return kNotFound;
}
} // namespace

RelationOpPtr repartitionForAgg(const RelationOpPtr& plan, PlanState& state) {
  // No shuffle if all grouping keys are in partitioning.
  if (isSingleWorker()) {
    return plan;
  }
  bool shuffle = false;
  ExprVector keyValues;
  auto* agg = state.dt->aggregation->aggregation;
  for (auto i = 0; i < agg->grouping.size(); ++i) {
    keyValues.push_back(agg->intermediateColumns[i]);
  }
  // If no grouping and not yet gathered on a single node, add a gather before
  // final agg.
  if (keyValues.empty() && !plan->distribution().distributionType.isGather) {
    auto* gather = make<Repartition>(
        plan,
        Distribution::gather(plan->distribution().distributionType),
        plan->columns());
    state.addCost(*gather);
    return gather;
  }
  for (auto& key : keyValues) {
    auto nthKey = position(plan->distribution().partition, *key);
    if (nthKey == kNotFound) {
      shuffle = true;
      break;
    }
  }
  if (!shuffle) {
    return plan;
  }

  Distribution distribution(
      plan->distribution().distributionType,
      plan->resultCardinality(),
      std::move(keyValues));
  auto* repartition =
      make<Repartition>(plan, std::move(distribution), plan->columns());
  state.addCost(*repartition);
  return repartition;
}

void Optimization::addPostprocess(
    DerivedTableCP dt,
    RelationOpPtr& plan,
    PlanState& state) {
  if (dt->aggregation) {
    auto* partialAgg = make<Aggregation>(
        *dt->aggregation->aggregation,
        plan,
        core::AggregationNode::Step::kPartial);
    state.placed.add(dt->aggregation);
    state.addCost(*partialAgg);
    plan = repartitionForAgg(partialAgg, state);
    auto* finalAgg = make<Aggregation>(
        *dt->aggregation->aggregation,
        plan,
        core::AggregationNode::Step::kFinal);
    state.addCost(*finalAgg);
    plan = finalAgg;
  }
  if (!dt->having.empty()) {
    auto filter = make<Filter>(plan, dt->having);
    state.addCost(*filter);
    plan = filter;
  }
  if (dt->orderBy) {
    auto* orderBy = make<OrderBy>(
        plan,
        dt->orderBy->distribution().order,
        dt->orderBy->distribution().orderType);
    state.addCost(*orderBy);
    plan = orderBy;
  }
  if (!dt->columns.empty()) {
    auto* project = make<Project>(plan, dt->exprs, dt->columns);
    plan = project;
  }
}

std::vector<ColumnGroupP> chooseLeafIndex(const BaseTable* table) {
  assert(!table->schemaTable->columnGroups.empty());
  return {table->schemaTable->columnGroups[0]};
}

template <typename V>
CPSpan<Column> leadingColumns(V& exprs) {
  int32_t i = 0;
  for (; i < exprs.size(); ++i) {
    if (exprs[i]->type() != PlanType::kColumn) {
      break;
    }
  }
  return CPSpan<Column>(reinterpret_cast<const Column* const*>(&exprs[0]), i);
}

bool isIndexColocated(
    const IndexInfo& info,
    const ExprVector& lookupValues,
    const RelationOpPtr& input) {
  if (info.index->distribution().isBroadcast &&
      input->distribution().distributionType.locus ==
          info.index->distribution().distributionType.locus) {
    return true;
  }

  // True if 'input' is partitioned so that each partitioning key is joined to
  // the corresponding partition key in 'info'.
  if (!(input->distribution().distributionType ==
        info.index->distribution().distributionType)) {
    return false;
  }
  if (input->distribution().partition.empty()) {
    return false;
  }
  if (input->distribution().partition.size() !=
      info.index->distribution().partition.size()) {
    return false;
  }
  for (auto i = 0; i < input->distribution().partition.size(); ++i) {
    auto nthKey = position(lookupValues, *input->distribution().partition[i]);
    if (nthKey != kNotFound) {
      if (info.schemaColumn(info.lookupKeys.at(nthKey)) !=
          info.index->distribution().partition.at(i)) {
        return false;
      }
    } else {
      return false;
    }
  }
  return true;
}

RelationOpPtr repartitionForIndex(
    const IndexInfo& info,
    const ExprVector& lookupValues,
    const RelationOpPtr& plan,
    PlanState& state) {
  if (isSingleWorker() || isIndexColocated(info, lookupValues, plan)) {
    return plan;
  }
  ExprVector keyExprs;
  auto& partition = info.index->distribution().partition;
  for (auto key : partition) {
    // partition is in schema columns, lookupKeys is in BaseTable columns. Use
    // the schema column of lookup key for matching.
    auto nthKey = position(
        info.lookupKeys,
        [](auto c) {
          return c->type() == PlanType::kColumn
              ? c->template as<Column>()->schemaColumn()
              : c;
        },
        *key);
    if (nthKey != kNotFound) {
      keyExprs.push_back(lookupValues[nthKey]);
    } else {
      return nullptr;
    }
  }

  Distribution distribution(
      info.index->distribution().distributionType,
      plan->resultCardinality(),
      std::move(keyExprs));
  auto* repartition =
      make<Repartition>(plan, std::move(distribution), plan->columns());
  state.addCost(*repartition);
  return repartition;
}

float fanoutJoinTypeLimit(core::JoinType joinType, float fanout) {
  switch (joinType) {
    case core::JoinType::kLeft:
      return std::max<float>(1, fanout);
    case core::JoinType::kLeftSemiFilter:
      return std::min<float>(1, fanout);
    case core::JoinType::kAnti:
      return 1 - std::min<float>(1, fanout);
    case core::JoinType::kLeftSemiProject:
    case core::JoinType::kRightSemiProject:
      return 1;
    default:
      return fanout;
  }
}

void Optimization::joinByIndex(
    const RelationOpPtr& plan,
    const JoinCandidate& candidate,
    PlanState& state,
    std::vector<NextJoin>& toTry) {
  if (candidate.tables.size() != 1 ||
      candidate.tables[0]->type() != PlanType::kTable ||
      !candidate.existences.empty()) {
    // Index applies to single base tables.
    return;
  }
  auto rightTable = candidate.tables.at(0)->as<BaseTable>();
  auto left = candidate.sideOf(rightTable, true);
  auto right = candidate.sideOf(rightTable);
  auto& keys = right.keys;
  auto keyColumns = leadingColumns(keys);
  if (keyColumns.empty()) {
    return;
  }
  for (auto& index : rightTable->schemaTable->columnGroups) {
    auto info = rightTable->schemaTable->indexInfo(index, keyColumns);
    if (info.lookupKeys.empty()) {
      continue;
    }
    PlanStateSaver save(state, candidate);
    auto newPartition = repartitionForIndex(info, left.keys, plan, state);
    if (!newPartition) {
      continue;
    }
    state.placed.add(candidate.tables.at(0));
    auto joinType = right.leftJoinType();
    if (joinType == core::JoinType::kFull ||
        joinType == core::JoinType::kRight) {
      // Not available by index.
      return;
    }
    auto fanout = fanoutJoinTypeLimit(
        joinType, info.scanCardinality * rightTable->filterSelectivity);

    auto lookupKeys = left.keys;
    // The number of keys is  the prefix that matches index order.
    lookupKeys.resize(info.lookupKeys.size());
    state.columns.unionSet(TableScan::availableColumns(rightTable, index));
    PlanObjectSet c = state.downstreamColumns();
    c.intersect(state.columns);
    for (auto& filter : rightTable->filter) {
      c.unionSet(filter->columns());
    }

    ColumnVector columns;
    c.forEach([&](PlanObjectCP o) { columns.push_back(o->as<Column>()); });

    auto* scan = make<TableScan>(
        newPartition,
        newPartition->distribution(),
        rightTable,
        info.index,
        fanout,
        columns,
        lookupKeys,
        joinType,
        candidate.join->filter());

    state.columns.unionSet(c);
    state.addCost(*scan);
    state.addNextJoin(&candidate, scan, {}, toTry);
  }
}

// Returns the positions in 'keys' for the expressions that determine the
// partition. empty if the partition is not decided by 'keys'
std::vector<uint32_t> joinKeyPartition(
    const RelationOpPtr& op,
    const ExprVector& keys) {
  std::vector<uint32_t> positions;
  for (unsigned i = 0; i < op->distribution().partition.size(); ++i) {
    auto nthKey = position(keys, *op->distribution().partition[i]);
    if (nthKey == kNotFound) {
      return {};
    }
    positions.push_back(nthKey);
  }
  return positions;
}

PlanObjectSet availableColumns(PlanObjectCP object) {
  PlanObjectSet set;
  if (object->type() == PlanType::kTable) {
    for (auto& c : object->as<BaseTable>()->columns) {
      set.add(c);
    }
  } else if (object->type() == PlanType::kValuesTable) {
    for (auto& c : object->as<ValuesTable>()->columns) {
      set.add(c);
    }
  } else if (object->type() == PlanType::kDerivedTable) {
    for (auto& c : object->as<DerivedTable>()->columns) {
      set.add(c);
    }
  } else {
    VELOX_UNREACHABLE("Joinable must be a table or derived table");
  }
  return set;
}

bool isBroadcastableSize(PlanPtr build, PlanState& /*state*/) {
  return build->cost.fanout < 100000;
}

void Optimization::joinByHash(
    const RelationOpPtr& plan,
    const JoinCandidate& candidate,
    PlanState& state,
    std::vector<NextJoin>& toTry) {
  assert(!candidate.tables.empty());
  auto build = candidate.sideOf(candidate.tables[0]);
  auto probe = candidate.sideOf(candidate.tables[0], true);
  ExprVector copartition;
  auto partKeys = joinKeyPartition(plan, probe.keys);
  if (partKeys.empty()) {
    // Prefer to make a build partitioned on join keys and shuffle probe to
    // align with build.
    copartition = build.keys;
  }
  PlanStateSaver save(state, candidate);
  PlanObjectSet buildTables;
  PlanObjectSet buildColumns;
  PlanObjectSet buildFilterColumns;
  for (auto& filter : candidate.join->filter()) {
    buildFilterColumns.unionColumns(filter);
  }
  buildFilterColumns.intersect(availableColumns(candidate.tables[0]));
  for (auto buildTable : candidate.tables) {
    buildColumns.unionSet(availableColumns(buildTable));
    buildTables.add(buildTable);
  }
  auto downstream = state.downstreamColumns();
  buildColumns.intersect(downstream);
  buildColumns.unionColumns(build.keys);
  buildColumns.unionSet(buildFilterColumns);
  state.columns.unionSet(buildColumns);
  auto memoKey = MemoKey{
      candidate.tables[0], buildColumns, buildTables, candidate.existences};
  PlanObjectSet empty;
  bool needsShuffle = false;
  auto buildPlan = makePlan(
      memoKey,
      Distribution(plan->distribution().distributionType, 0, copartition),
      empty,
      candidate.existsFanout,
      state,
      needsShuffle);
  // the build side tables are all joined if the first build is a
  // table but if it is a derived table (most often with aggregation),
  // only some of the tables may be fully joined.
  if (candidate.tables[0]->type() == PlanType::kDerivedTable) {
    state.placed.add(candidate.tables[0]);
    state.placed.unionSet(buildPlan->fullyImported);
  } else {
    state.placed.unionSet(buildTables);
  }
  PlanState buildState(state.optimization, state.dt, buildPlan);
  bool partitionByProbe = !isSingle_ && !partKeys.empty();
  RelationOpPtr buildInput = buildPlan->op;
  RelationOpPtr probeInput = plan;
  if (partitionByProbe) {
    if (needsShuffle) {
      if (copartition.empty()) {
        for (auto i : partKeys) {
          copartition.push_back(build.keys[i]);
        }
      }
      Distribution dist(plan->distribution().distributionType, 0, copartition);
      auto* shuffleTemp =
          make<Repartition>(buildInput, dist, buildInput->columns());
      buildState.addCost(*shuffleTemp);
      buildInput = shuffleTemp;
    }
  } else if (
      !isSingle_ && candidate.join->isBroadcastableType() &&
      isBroadcastableSize(buildPlan, state)) {
    auto* broadcast = make<Repartition>(
        buildInput,
        Distribution::broadcast(
            plan->distribution().distributionType, plan->resultCardinality()),
        buildInput->columns());
    buildState.addCost(*broadcast);
    buildInput = broadcast;
  } else {
    // The probe gets shuffled to align with build. If build is not partitioned
    // on its keys, shuffle the build too.
    auto buildPart = joinKeyPartition(buildInput, build.keys);
    if (!isSingle_ && buildPart.empty()) {
      // The build is not aligned on join keys.
      Distribution buildDist(
          plan->distribution().distributionType,
          plan->resultCardinality(),
          build.keys);
      auto* buildShuffle =
          make<Repartition>(buildInput, buildDist, buildInput->columns());
      buildState.addCost(*buildShuffle);
      buildInput = buildShuffle;
    }

    ExprVector distCols;
    for (size_t i = 0; i < probe.keys.size(); ++i) {
      auto key = build.keys[i];
      auto nthKey = position(buildInput->distribution().partition, *key);
      if (nthKey != kNotFound) {
        if (distCols.size() <= nthKey) {
          distCols.resize(nthKey + 1);
        }
        assert(!distCols.empty());
        distCols[nthKey] = probe.keys[i];
      }
    }
    Distribution probeDist(
        probeInput->distribution().distributionType,
        probeInput->resultCardinality(),
        std::move(distCols));
    if (!isSingle_) {
      auto* probeShuffle =
          make<Repartition>(plan, std::move(probeDist), plan->columns());
      state.addCost(*probeShuffle);
      probeInput = probeShuffle;
    }
  }
  auto* buildOp =
      make<HashBuild>(buildInput, ++buildCounter_, build.keys, buildPlan);
  buildState.addCost(*buildOp);

  ColumnVector columns;
  PlanObjectSet columnSet;
  ColumnCP mark = nullptr;
  PlanObjectSet probeColumns;
  probeColumns.unionColumns(plan->columns());
  auto joinType = build.leftJoinType();
  bool probeOnly = joinType == core::JoinType::kLeftSemiFilter ||
      joinType == core::JoinType::kLeftSemiProject ||
      joinType == core::JoinType::kAnti ||
      joinType == core::JoinType::kLeftSemiProject;
  downstream = state.downstreamColumns();
  downstream.forEach([&](auto object) {
    auto column = reinterpret_cast<ColumnCP>(object);
    if (column == build.markColumn) {
      mark = column;
      columnSet.add(object);
      return;
    }
    if (!(!probeOnly && buildColumns.contains(column)) &&
        !probeColumns.contains(column)) {
      return;
    }
    columnSet.add(object);
    columns.push_back(column);
  });
  // If there is an existence flag, it is the rightmost result column.
  if (mark) {
    const_cast<Value*>(&mark->value())->trueFraction =
        std::min<float>(1, candidate.fanout);
    columns.push_back(mark);
  }
  state.columns = columnSet;
  auto fanout = fanoutJoinTypeLimit(joinType, candidate.fanout);
  auto* join = make<Join>(
      JoinMethod::kHash,
      joinType,
      probeInput,
      buildOp,
      probe.keys,
      build.keys,
      candidate.join->filter(),
      fanout,
      std::move(columns));
  state.addCost(*join);
  state.cost.setupCost += buildState.cost.unitCost + buildState.cost.setupCost;
  state.cost.totalBytes += buildState.cost.totalBytes;
  state.cost.transferBytes += buildState.cost.transferBytes;
  join->buildCost = buildState.cost;
  state.addNextJoin(&candidate, join, {buildOp}, toTry);
}

core::JoinType reverseJoinType(core::JoinType joinType) {
  switch (joinType) {
    case core::JoinType::kLeft:
      return core::JoinType::kRight;
    case core::JoinType::kRight:
      return core::JoinType::kLeft;
    case core::JoinType::kLeftSemiFilter:
      return core::JoinType::kRightSemiFilter;
    case core::JoinType::kLeftSemiProject:
      return core::JoinType::kRightSemiProject;
    default:
      return joinType;
  }
}

void Optimization::joinByHashRight(
    const RelationOpPtr& plan,
    const JoinCandidate& candidate,
    PlanState& state,
    std::vector<NextJoin>& toTry) {
  assert(!candidate.tables.empty());
  auto probe = candidate.sideOf(candidate.tables[0]);
  auto build = candidate.sideOf(candidate.tables[0], true);
  PlanStateSaver save(state, candidate);
  PlanObjectSet probeTables;
  PlanObjectSet probeColumns;
  PlanObjectSet probeFilterColumns;
  for (auto& filter : candidate.join->filter()) {
    probeFilterColumns.unionColumns(filter);
  }
  probeFilterColumns.intersect(availableColumns(candidate.tables[0]));

  for (auto probeTable : candidate.tables) {
    probeColumns.unionSet(availableColumns(probeTable));
    state.placed.add(probeTable);
    probeTables.add(probeTable);
  }
  auto downstream = state.downstreamColumns();
  probeColumns.intersect(downstream);
  probeColumns.unionColumns(probe.keys);
  probeColumns.unionSet(probeFilterColumns);
  state.columns.unionSet(probeColumns);
  auto memoKey = MemoKey{
      candidate.tables[0], probeColumns, probeTables, candidate.existences};
  PlanObjectSet empty;
  bool needsShuffle = false;
  auto probePlan = makePlan(
      memoKey,
      Distribution(plan->distribution().distributionType, 0, {}),
      empty,
      candidate.existsFanout,
      state,
      needsShuffle);
  PlanState probeState(state.optimization, state.dt, probePlan);

  RelationOpPtr probeInput = probePlan->op;
  RelationOpPtr buildInput = plan;
  // The build gets shuffled to align with probe. If probe is not partitioned
  // on its keys, shuffle the probe too.
  auto probePart = joinKeyPartition(probeInput, probe.keys);
  if (!isSingle_ && probePart.empty()) {
    Distribution probeDist(
        buildInput->distribution().distributionType,
        probeInput->resultCardinality(),
        probe.keys);
    auto* probeShuffle =
        make<Repartition>(probeInput, probeDist, probeInput->columns());
    probeState.addCost(*probeShuffle);
    probeInput = probeShuffle;
  }
  ExprVector buildPartCols;
  for (size_t i = 0; i < probe.keys.size(); ++i) {
    auto key = probe.keys[i];
    auto nthKey = position(probeInput->distribution().partition, *key);
    if (nthKey != kNotFound) {
      if (buildPartCols.size() <= nthKey) {
        buildPartCols.resize(nthKey + 1);
      }
      assert(isSingle_ || !buildPartCols.empty());
      buildPartCols[nthKey] = build.keys[i];
    }
  }
  Distribution buildDist(
      probeInput->distribution().distributionType,
      buildInput->resultCardinality(),
      std::move(buildPartCols));
  if (!isSingle_) {
    auto* buildShuffle =
        make<Repartition>(plan, std::move(buildDist), plan->columns());
    state.addCost(*buildShuffle);
    buildInput = buildShuffle;
  }
  auto* buildOp =
      make<HashBuild>(buildInput, ++buildCounter_, build.keys, nullptr);
  state.addCost(*buildOp);

  PlanObjectSet buildColumns;
  buildColumns.unionColumns(plan->columns());

  auto leftJoinType = probe.leftJoinType();
  const auto fanout = fanoutJoinTypeLimit(leftJoinType, candidate.fanout);

  // Change the join type to the right join variant.
  const auto rightJoinType = reverseJoinType(leftJoinType);
  VELOX_CHECK(
      leftJoinType != rightJoinType,
      "Join type does not have right hash join variant");

  const bool buildOnly = rightJoinType == core::JoinType::kRightSemiFilter ||
      rightJoinType == core::JoinType::kRightSemiProject;

  ColumnVector columns;
  PlanObjectSet columnSet;
  ColumnCP mark = nullptr;

  downstream = state.downstreamColumns();
  downstream.forEach([&](auto object) {
    auto column = reinterpret_cast<ColumnCP>(object);
    if (column == probe.markColumn) {
      mark = column;
      return;
    }
    if (!buildColumns.contains(column) &&
        !(!buildOnly && probeColumns.contains(column))) {
      return;
    }
    columnSet.add(object);
    columns.push_back(column);
  });

  if (mark) {
    const_cast<Value*>(&mark->value())->trueFraction =
        std::min<float>(1, candidate.fanout);
    columns.push_back(mark);
  }

  state.columns = columnSet;
  auto buildCost = state.cost.unitCost;
  state.cost = probeState.cost;
  state.cost.setupCost += buildCost;

  auto* join = make<Join>(
      JoinMethod::kHash,
      rightJoinType,
      probeInput,
      buildOp,
      probe.keys,
      build.keys,
      candidate.join->filter(),
      fanout,
      std::move(columns));
  state.addCost(*join);

  state.addNextJoin(&candidate, join, {buildOp}, toTry);
}

void Optimization::crossJoin(
    const RelationOpPtr& plan,
    const JoinCandidate& candidate,
    PlanState& state,
    std::vector<NextJoin>& toTry) {
  VELOX_NYI("No cross joins");
}

void Optimization::addJoin(
    const JoinCandidate& candidate,
    const RelationOpPtr& plan,
    PlanState& state,
    std::vector<NextJoin>& result) {
  std::vector<NextJoin> toTry;
  if (!candidate.join) {
    crossJoin(plan, candidate, state, toTry);
    return;
  }
  joinByIndex(plan, candidate, state, toTry);
  auto sizeAfterIndex = toTry.size();
  joinByHash(plan, candidate, state, toTry);
  if (toTry.size() > sizeAfterIndex && candidate.join->isNonCommutative() &&
      candidate.join->hasRightHashVariant()) {
    // There is a hash based candidate with a non-commutative join. Try a right
    // join variant.
    joinByHashRight(plan, candidate, state, toTry);
  }
  // If one is much better do not try the other.
  if (toTry.size() == 2 && candidate.tables.size() == 1) {
    if (toTry[0].isWorse(toTry[1])) {
      toTry.erase(toTry.begin());
    } else if (toTry[1].isWorse(toTry[0])) {
      toTry.erase(toTry.begin() + 1);
    }
  }
  result.insert(result.end(), toTry.begin(), toTry.end());
}

// Sets 'columns' to the columns in 'downstream' that exist
// in 'index' of 'table'.
// If there's no index, returns all columns in 'table' that
// are in 'downstream'.
ColumnVector indexColumns(
    const PlanObjectSet& downstream,
    PlanObjectCP table,
    ColumnGroupP index) {
  ColumnVector result;
  downstream.forEach([&](PlanObjectCP object) {
    auto* column = object->as<Column>();
    if (!column->schemaColumn()) {
      return;
    }
    if (table != column->relation()) {
      return;
    }
    if (!index ||
        position(index->columns(), *object->as<Column>()->schemaColumn()) !=
            kNotFound) {
      result.push_back(object->as<Column>());
    }
  });
  return result;
}

void Optimization::tryNextJoins(
    PlanState& state,
    const std::vector<NextJoin>& nextJoins) {
  for (auto& next : nextJoins) {
    PlanStateSaver save(state);
    state.placed = next.placed;
    state.columns = next.columns;
    state.cost = next.cost;
    state.addBuilds(next.newBuilds);
    makeJoins(next.plan, state);
  }
}

RelationOpPtr Optimization::placeSingleRowDt(
    RelationOpPtr plan,
    const DerivedTable* subq,
    ExprCP filter,
    PlanState& state) {
  auto broadcast = Distribution::broadcast(DistributionType(), 1);
  MemoKey memoKey;
  memoKey.firstTable = subq;
  memoKey.tables.add(subq);
  for (auto& column : subq->columns) {
    memoKey.columns.add(column);
  }
  PlanObjectSet empty;
  bool needsShuffle = false;
  auto rightPlan = makePlan(memoKey, broadcast, empty, 1, state, needsShuffle);
  auto rightOp = rightPlan->op;
  if (needsShuffle) {
    auto* repartition =
        make<Repartition>(rightOp, broadcast, rightOp->columns());
    rightOp = repartition;
  }
  auto resultColumns = plan->columns();
  resultColumns.insert(
      resultColumns.end(),
      rightOp->columns().begin(),
      rightOp->columns().end());
  auto* join = new (queryCtx()->allocate(sizeof(Join))) Join(
      JoinMethod::kCross,
      core::JoinType::kInner,
      plan,
      rightOp,
      {},
      {},
      {filter},
      0.5,
      resultColumns);
  state.addCost(*join);
  return join;
}

void Optimization::placeDerivedTable(
    const DerivedTable* from,
    PlanState& state) {
  PlanStateSaver save(state);

  state.placed.add(from);
  PlanObjectSet columns = state.downstreamColumns();
  PlanObjectSet dtColumns;
  for (auto column : from->columns) {
    dtColumns.add(column);
  }
  columns.intersect(dtColumns);
  state.columns.unionSet(columns);
  MemoKey key;
  key.columns = columns;
  key.firstTable = from;
  key.tables.add(from);
  bool ignore;
  auto plan = makePlan(key, Distribution(), PlanObjectSet(), 1, state, ignore);
  // Make plans based on the dt alone as first.
  makeJoins(plan->op, state);

  // We see if there are reducing joins to import inside the dt.
  PlanObjectSet visited = state.placed;
  visited.add(from);
  visited.unionSet(state.dt->importedExistences);
  visited.unionSet(state.dt->fullyImported);
  PlanObjectSet reducingSet;
  reducingSet.add(from);
  std::vector<PlanObjectCP> path{from};
  float reduction = 1;
  reducingJoinsRecursive(
      state, from, 1, 1.2, path, visited, reducingSet, reduction);
  if (reduction < 0.9) {
    key.tables = reducingSet;
    auto savedPlaced = state.placed;
    key.columns = state.downstreamColumns();
    plan = makePlan(key, Distribution(), PlanObjectSet(), 1, state, ignore);
    // Not all reducing joins are necessarily retained in the plan. Only mark
    // the ones fully imported as placed.
    state.placed.unionSet(plan->fullyImported);
    makeJoins(plan->op, state);
  }
}

bool Optimization::placeConjuncts(
    RelationOpPtr plan,
    PlanState& state,
    bool allowNondeterministic) {
  PlanStateSaver save(state);
  ExprVector filters;
  PlanObjectSet columnsAndSingles = state.columns;
  state.dt->singleRowDts.forEach([&](PlanObjectCP object) {
    columnsAndSingles.unionColumns(object->as<DerivedTable>()->columns);
  });
  for (auto& conjunct : state.dt->conjuncts) {
    if (!allowNondeterministic && conjunct->containsNonDeterministic()) {
      continue;
    }
    if (state.placed.contains(conjunct)) {
      continue;
    }
    if (conjunct->columns().isSubset(state.columns)) {
      state.columns.add(conjunct);
      filters.push_back(conjunct);
      continue;
    }
    if (conjunct->columns().isSubset(columnsAndSingles)) {
      // The filter depends on placed tables and non-correlated single row
      // subqueries.
      std::vector<const DerivedTable*> placeable;
      auto subqColumns = conjunct->columns();
      subqColumns.except(state.columns);
      subqColumns.forEach([&](PlanObjectCP object) {
        state.dt->singleRowDts.forEach([&](PlanObjectCP dtObject) {
          auto subq = dtObject->as<DerivedTable>();
          // If the subq provides columns for the filter, place it.
          auto conjunctColumns = conjunct->columns();
          for (auto subqColumn : subq->columns) {
            if (conjunctColumns.contains(subqColumn)) {
              placeable.push_back(subq);
              break;
            }
          }
        });
      });
      for (auto i = 0; i < placeable.size(); ++i) {
        state.placed.add(conjunct);
        plan = placeSingleRowDt(
            plan,
            placeable[i],
            (i == placeable.size() - 1 ? conjunct : nullptr),
            state);
        makeJoins(plan, state);
        return true;
      }
    }
  }
  if (!filters.empty()) {
    for (auto& filter : filters) {
      state.placed.add(filter);
    }
    auto* filter = make<Filter>(plan, std::move(filters));
    state.addCost(*filter);
    makeJoins(filter, state);
    return true;
  }
  return false;
}

void Optimization::makeJoins(RelationOpPtr plan, PlanState& state) {
  auto& dt = state.dt;
  if (!plan) {
    std::vector<PlanObjectCP> firstTables;
    dt->startTables.forEach([&](auto table) { firstTables.push_back(table); });
    std::vector<float> scores(firstTables.size());
    for (auto i = 0; i < firstTables.size(); ++i) {
      auto table = firstTables[i];
      state.setFirstTable(table->id());
      scores.at(i) = startingScore(table);
    }
    std::vector<int32_t> ids(firstTables.size());
    std::iota(ids.begin(), ids.end(), 0);
    std::sort(ids.begin(), ids.end(), [&](int32_t left, int32_t right) {
      return scores[left] > scores[right];
    });
    for (auto i : ids) {
      auto from = firstTables.at(i);
      if (from->type() == PlanType::kTable) {
        auto table = from->as<BaseTable>();
        auto indices = chooseLeafIndex(table->as<BaseTable>());
        // Make plan starting with each relevant index of the table.
        auto downstream = state.downstreamColumns();
        for (auto index : indices) {
          PlanStateSaver save(state);
          state.placed.add(table);
          auto columns = indexColumns(downstream, table, index);

          auto* scan = make<TableScan>(
              nullptr,
              TableScan::outputDistribution(table, index, columns),
              table,
              index,
              index->distribution().cardinality * table->filterSelectivity,
              columns);

          state.columns.unionObjects(columns);
          state.addCost(*scan);
          makeJoins(scan, state);
        }
      } else if (from->type() == PlanType::kValuesTable) {
        const auto* valuesTable = from->as<ValuesTable>();
        auto downstream = state.downstreamColumns();
        PlanStateSaver save{state};
        state.placed.add(valuesTable);
        auto columns = indexColumns(downstream, valuesTable, nullptr);

        auto* scan = make<Values>(
            Distribution{DistributionType{}, valuesTable->cardinality(), {}},
            *valuesTable,
            std::move(columns));

        state.columns.unionObjects(columns);
        state.addCost(*scan);
        makeJoins(scan, state);
      } else {
        // Start with a derived table.
        placeDerivedTable(from->as<const DerivedTable>(), state);
      }
    }
  } else {
    if (state.isOverBest()) {
      trace(kExceededBest, dt->id(), state.cost, *plan);
      return;
    }
    // Add multitable filters not associated to a non-inner join.
    if (placeConjuncts(plan, state, false)) {
      return;
    }
    auto candidates = nextJoins(state);
    if (candidates.empty()) {
      if (placeConjuncts(plan, state, true)) {
        return;
      }
      addPostprocess(dt, plan, state);
      auto kept = state.plans.addPlan(plan, state);
      trace(kept ? kRetained : kExceededBest, dt->id(), state.cost, *plan);

      return;
    }
    std::vector<NextJoin> nextJoins;
    nextJoins.reserve(candidates.size());
    for (auto& candidate : candidates) {
      addJoin(candidate, plan, state, nextJoins);
    }
    tryNextJoins(state, nextJoins);
  }
}

namespace {
RelationOpPtr makeDistinct(const RelationOpPtr& input) {
  ExprVector exprs;
  for (auto& c : input->columns()) {
    exprs.push_back(c);
  }
  auto agg = make<Aggregation>(input, exprs);
  agg->mutableColumns() = input->columns();
  agg->intermediateColumns = input->columns();
  return agg;
}

Distribution somePartition(const RelationOpPtrVector& inputs) {
  float card = 1;

  // A simple type and many values is a good partitioning key.
  auto score = [&](ColumnCP column) {
    const auto& value = column->value();
    const auto card = value.cardinality;
    return value.type->kind() >= TypeKind::ARRAY ? card / 10000 : card;
  };

  const auto& firstInput = inputs[0];
  auto inputColumns = firstInput->columns();
  std::sort(
      inputColumns.begin(),
      inputColumns.end(),
      [&](ColumnCP left, ColumnCP right) {
        return score(left) > score(right);
      });

  ExprVector columns;
  for (const auto* column : inputColumns) {
    card *= column->value().cardinality;
    columns.push_back(column);
    if (card > 100'000) {
      break;
    }
  }

  DistributionType distributionType;
  distributionType.numPartitions =
      queryCtx()->optimization()->options().numWorkers;
  distributionType.locus = firstInput->distribution().distributionType.locus;

  Distribution result;
  result.partition = columns;
  result.distributionType = distributionType;
  return result;
}

// Adds the costs in the input states to the first state and if 'distinct' is
// not null adds the cost of that to the first state.
PlanPtr unionPlan(
    std::vector<PlanState>& states,
    const std::vector<PlanPtr>& inputPlans,
    const RelationOpPtr& result,
    Aggregation* distinct) {
  auto& firstState = states[0];

  PlanObjectSet fullyImported = inputPlans[0]->fullyImported;
  for (auto i = 1; i < states.size(); ++i) {
    const auto& otherCost = states[i].cost;
    fullyImported.intersect(inputPlans[i]->fullyImported);
    firstState.cost.add(otherCost);
    // The input cardinality is not additive, the fanout and other metrics are.
    firstState.cost.inputCardinality -= otherCost.inputCardinality;
  }
  if (distinct) {
    firstState.addCost(*distinct);
  }
  auto plan = make<Plan>(result, states[0]);
  plan->fullyImported = fullyImported;
  return plan;
}
} // namespace

PlanPtr Optimization::makePlan(
    const MemoKey& key,
    const Distribution& distribution,
    const PlanObjectSet& boundColumns,
    float existsFanout,
    PlanState& state,
    bool& needsShuffle) {
  if (key.firstTable->type() == PlanType::kDerivedTable &&
      key.firstTable->as<DerivedTable>()->setOp.has_value()) {
    const auto* setDt = key.firstTable->as<DerivedTable>();

    RelationOpPtrVector inputs;
    std::vector<PlanPtr> inputPlans;
    std::vector<PlanState> inputStates;
    std::vector<bool> inputNeedsShuffle;

    for (auto* inputDt : setDt->children) {
      MemoKey inputKey = key;
      inputKey.firstTable = inputDt;
      inputKey.tables.erase(key.firstTable);
      inputKey.tables.add(inputDt);

      bool inputShuffle = false;
      auto inputPlan = makePlan(
          inputKey,
          distribution,
          boundColumns,
          existsFanout,
          state,
          inputShuffle);
      inputPlans.push_back(inputPlan);
      inputStates.emplace_back(*this, setDt, inputPlans.back());
      inputs.push_back(inputPlan->op);
      inputNeedsShuffle.push_back(inputShuffle);
    }

    const bool isDistinct =
        setDt->setOp.value() == logical_plan::SetOperation::kUnion;
    if (isSingle_) {
      RelationOpPtr result = make<UnionAll>(inputs);
      Aggregation* distinct = nullptr;
      if (isDistinct) {
        result = makeDistinct(result);
        distinct = result->as<Aggregation>();
      }
      return unionPlan(inputStates, inputPlans, result, distinct);
    }

    if (distribution.partition.empty()) {
      if (isDistinct) {
        // Pick some partitioning key and shuffle on that and make distinct.
        Distribution someDistribution = somePartition(inputs);
        for (auto i = 0; i < inputs.size(); ++i) {
          inputs[i] = make<Repartition>(
              inputs[i], someDistribution, inputs[i]->columns());
          inputStates[i].addCost(*inputs[i]);
        }
      }
    } else {
      // Some need a shuffle. Add the shuffles, add an optional distinct and
      // return with no shuffle needed.
      for (auto i = 0; i < inputs.size(); ++i) {
        if (inputNeedsShuffle[i]) {
          inputs[i] =
              make<Repartition>(inputs[i], distribution, inputs[i]->columns());
          inputStates[i].addCost(*inputs[i]);
        }
      }
    }
    needsShuffle = false;

    RelationOpPtr result = make<UnionAll>(inputs);
    Aggregation* distinct = nullptr;
    if (isDistinct) {
      result = makeDistinct(result);
      distinct = result->as<Aggregation>();
    }
    return unionPlan(inputStates, inputPlans, result, distinct);
  } else {
    return makeDtPlan(
        key, distribution, boundColumns, existsFanout, state, needsShuffle);
  }
}

PlanPtr Optimization::makeDtPlan(
    const MemoKey& key,
    const Distribution& distribution,
    const PlanObjectSet& /*boundColumns*/,
    float existsFanout,
    PlanState& state,
    bool& needsShuffle) {
  auto it = memo_.find(key);
  PlanSet* plans;
  if (it == memo_.end()) {
    DerivedTable dt;
    dt.import(
        *state.dt, key.firstTable, key.tables, key.existences, existsFanout);
    PlanState inner(*this, &dt);
    if (key.firstTable->type() == PlanType::kDerivedTable) {
      inner.setTargetColumnsForDt(key.columns);
    } else {
      inner.targetColumns = key.columns;
    }
    makeJoins(nullptr, inner);
    memo_[key] = std::move(inner.plans);
    plans = &memo_[key];
  } else {
    plans = &it->second;
  }
  return plans->best(distribution, needsShuffle);
}

} // namespace facebook::velox::optimizer
