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
#include "axiom/optimizer/DerivedTable.h"
#include "axiom/optimizer/Plan.h"
#include "axiom/optimizer/PlanUtils.h"

namespace facebook::velox::optimizer {

namespace {
/// If 'object' is an Expr, returns Expr::singleTable, else nullptr.
PlanObjectCP singleTable(PlanObjectCP object) {
  if (isExprType(object->type())) {
    return object->as<Expr>()->singleTable();
  }
  return nullptr;
}
} // namespace

void DerivedTable::addJoinEquality(
    ExprCP left,
    ExprCP right,
    const ExprVector& filter,
    bool leftOptional,
    bool rightOptional,
    bool rightExists,
    bool rightNotExists) {
  auto leftTable = singleTable(left);
  auto rightTable = singleTable(right);
  for (auto& join : joins) {
    if (join->leftTable() == leftTable && join->rightTable() == rightTable) {
      join->addEquality(left, right);
      return;
    } else if (
        join->rightTable() == leftTable && join->leftTable() == rightTable) {
      join->addEquality(right, left);
      return;
    }
  }
  auto* join = make<JoinEdge>(
      leftTable,
      rightTable,
      filter,
      leftOptional,
      rightOptional,
      rightExists,
      rightNotExists);
  join->addEquality(left, right);
  joins.push_back(join);
}

namespace {

using EdgeSet = std::unordered_set<std::pair<int32_t, int32_t>>;

void addEdge(EdgeSet& edges, int32_t id1, int32_t id2) {
  if (id1 > id2) {
    edges.insert(std::pair<int32_t, int32_t>(id2, id1));
  } else {
    edges.insert(std::pair<int32_t, int32_t>(id1, id2));
  }
}

bool hasEdge(const EdgeSet& edges, int32_t id1, int32_t id2) {
  if (id1 == id2) {
    return true;
  }
  auto it = edges.find(
      id1 > id2 ? std::pair<int32_t, int32_t>(id2, id1)
                : std::pair<int32_t, int32_t>(id1, id2));
  return it != edges.end();
}

void fillJoins(
    PlanObjectCP column,
    const Equivalence& equivalence,
    EdgeSet& edges,
    DerivedTableP dt) {
  for (auto& other : equivalence.columns) {
    if (!hasEdge(edges, column->id(), other->id())) {
      addEdge(edges, column->id(), other->id());
      dt->addJoinEquality(
          column->as<Column>(),
          other->as<Column>(),
          {},
          false,
          false,
          false,
          false);
    }
  }
}
} // namespace

void DerivedTable::addImpliedJoins() {
  EdgeSet edges;
  for (auto& join : joins) {
    if (join->isInner()) {
      for (auto i = 0; i < join->leftKeys().size(); ++i) {
        const auto* leftKey = join->leftKeys()[i];
        const auto* rightKey = join->rightKeys()[i];
        if (leftKey->isColumn() && rightKey->isColumn()) {
          addEdge(edges, leftKey->id(), rightKey->id());
        }
      }
    }
  }

  // The loop appends to 'joins', so loop over a copy.
  JoinEdgeVector joinsCopy = joins;
  for (auto& join : joinsCopy) {
    if (join->isInner()) {
      for (auto i = 0; i < join->leftKeys().size(); ++i) {
        const auto* leftKey = join->leftKeys()[i];
        const auto* rightKey = join->rightKeys()[i];
        if (leftKey->isColumn() && rightKey->isColumn()) {
          auto leftEq = leftKey->as<Column>()->equivalence();
          auto rightEq = rightKey->as<Column>()->equivalence();
          if (rightEq && leftEq) {
            for (auto& left : leftEq->columns) {
              fillJoins(left, *rightEq, edges, this);
            }
          } else if (leftEq) {
            fillJoins(rightKey, *leftEq, edges, this);
          } else if (rightEq) {
            fillJoins(leftKey, *rightEq, edges, this);
          }
        }
      }
    }
  }
}

void DerivedTable::setStartTables() {
  findSingleRowDts();
  startTables = tableSet;
  startTables.except(singleRowDts);
  for (auto join : joins) {
    if (join->isNonCommutative()) {
      startTables.erase(join->rightTable());
    }
  }
}

namespace {
// Returns a right exists (semijoin) with 'table' on the left and one of
// 'tables' on the right.
JoinEdgeP makeExists(PlanObjectCP table, const PlanObjectSet& tables) {
  for (auto join : joinedBy(table)) {
    if (join->leftTable() == table) {
      if (!tables.contains(join->rightTable())) {
        continue;
      }
      auto* exists = make<JoinEdge>(
          table, join->rightTable(), ExprVector{}, false, false, true, false);
      for (auto i = 0; i < join->leftKeys().size(); ++i) {
        exists->addEquality(join->leftKeys()[i], join->rightKeys()[i]);
      }
      return exists;
    }

    if (join->rightTable() == table) {
      if (!join->leftTable() || !tables.contains(join->leftTable())) {
        continue;
      }

      auto* exists = make<JoinEdge>(
          table, join->leftTable(), ExprVector{}, false, false, true, false);
      for (auto i = 0; i < join->leftKeys().size(); ++i) {
        exists->addEquality(join->rightKeys()[i], join->leftKeys()[i]);
      }
      return exists;
    }
  }
  VELOX_UNREACHABLE("No join to make an exists build side restriction");
}

bool isSingleRowDt(PlanObjectCP object) {
  if (object->type() == PlanType::kDerivedTable) {
    auto dt = object->as<DerivedTable>();
    return dt->limit == 1 ||
        (dt->aggregation && dt->aggregation->aggregation->grouping.empty());
  }
  return false;
}

} // namespace

void DerivedTable::findSingleRowDts() {
  auto tablesCopy = tableSet;
  int32_t numSingle = 0;
  for (auto& join : joins) {
    tablesCopy.erase(join->rightTable());
    for (auto& key : join->leftKeys()) {
      tablesCopy.except(key->allTables());
    }
    for (auto& filter : join->filter()) {
      tablesCopy.except(filter->allTables());
    }
  }
  tablesCopy.forEach([&](PlanObjectCP object) {
    if (isSingleRowDt(object)) {
      ++numSingle;
      singleRowDts.add(object);
    }
  });
  // if everything is a single row dt, then process tese as cross products and
  // not as placed with filters.
  if (numSingle == tables.size()) {
    singleRowDts = PlanObjectSet();
  }
}

void DerivedTable::linkTablesToJoins() {
  setStartTables();

  // All tables directly mentioned by a join link to the join. A non-inner
  // that depends on multiple left tables has no leftTable but is still linked
  // from all the tables it depends on.
  for (auto join : joins) {
    PlanObjectSet tables;
    for (auto key : join->leftKeys()) {
      tables.unionSet(key->allTables());
    }
    for (auto key : join->rightKeys()) {
      tables.unionSet(key->allTables());
    }
    if (!join->filter().empty()) {
      for (auto& conjunct : join->filter()) {
        tables.unionSet(conjunct->allTables());
      }
    }
    tables.forEachMutable([&](PlanObjectP table) {
      if (table->type() == PlanType::kTable) {
        table->as<BaseTable>()->addJoinedBy(join);
      } else if (table->type() == PlanType::kValuesTable) {
        table->as<ValuesTable>()->addJoinedBy(join);
      } else {
        VELOX_CHECK_EQ(table->type(), PlanType::kDerivedTable);
        table->as<DerivedTable>()->addJoinedBy(join);
      }
    });
  }
}

namespace {
std::pair<DerivedTableP, JoinEdgeP> makeExistsDtAndJoin(
    const DerivedTable& super,
    PlanObjectCP firstTable,
    float existsFanout,
    PlanObjectVector& existsTables,
    JoinEdgeP existsJoin) {
  auto firstExistsTable = existsJoin->rightKeys()[0]->singleTable();
  VELOX_CHECK(firstExistsTable);
  MemoKey existsDtKey;
  existsDtKey.firstTable = firstExistsTable;
  for (auto& column : existsJoin->rightKeys()) {
    existsDtKey.columns.unionColumns(column);
  }
  auto optimization = queryCtx()->optimization();
  existsDtKey.tables.unionObjects(existsTables);
  auto it = optimization->existenceDts().find(existsDtKey);
  DerivedTableP existsDt;
  if (it == optimization->existenceDts().end()) {
    auto* newDt = make<DerivedTable>();
    existsDt = newDt;
    existsDt->cname = queryCtx()->optimization()->newCName("edt");
    existsDt->import(super, firstExistsTable, existsDtKey.tables, {});
    for (auto& k : existsJoin->rightKeys()) {
      auto* existsColumn = make<Column>(
          toName(fmt::format("{}.{}", existsDt->cname, k->toString())),
          existsDt,
          k->value());
      existsDt->columns.push_back(existsColumn);
      existsDt->exprs.push_back(k);
    }
    existsDt->noImportOfExists = true;
    existsDt->makeInitialPlan();
    optimization->existenceDts()[existsDtKey] = existsDt;
  } else {
    existsDt = it->second;
  }
  auto* joinWithDt = make<JoinEdge>(
      firstTable, existsDt, ExprVector{}, false, false, true, false);
  joinWithDt->setFanouts(existsFanout, 1);
  for (auto i = 0; i < existsJoin->leftKeys().size(); ++i) {
    joinWithDt->addEquality(existsJoin->leftKeys()[i], existsDt->columns[i]);
  }
  return std::make_pair(existsDt, joinWithDt);
}
} // namespace

void DerivedTable::import(
    const DerivedTable& super,
    PlanObjectCP firstTable,
    const PlanObjectSet& _tables,
    const std::vector<PlanObjectSet>& existences,
    float existsFanout) {
  tableSet = _tables;
  _tables.forEach([&](auto table) { tables.push_back(table); });
  for (auto join : super.joins) {
    if (_tables.contains(join->rightTable()) && join->leftTable() &&
        _tables.contains(join->leftTable())) {
      joins.push_back(join);
    }
  }
  for (auto& exists : existences) {
    // We filter the derived table by importing reducing semijoins.
    // These are based on joins on the outer query but become
    // existences so as not to change cardinality. The reducing join
    // is against one or more tables. If more than one table, the join
    // of these tables goes into its own derived table which is joined
    // with exists to the main table(s) in the 'this'.
    importedExistences.unionSet(exists);
    PlanObjectVector existsTables;
    exists.forEach([&](auto object) { existsTables.push_back(object); });
    auto existsJoin = makeExists(firstTable, exists);
    if (existsTables.size() > 1) {
      // There is a join on the right of exists. Needs its own dt.
      auto [existsDt, joinWithDt] = makeExistsDtAndJoin(
          super, firstTable, existsFanout, existsTables, existsJoin);
      joins.push_back(joinWithDt);
      tables.push_back(existsDt);
      tableSet.add(existsDt);
      noImportOfExists = true;
    } else {
      joins.push_back(existsJoin);
      assert(!existsTables.empty());
      tables.push_back(existsTables[0]);
      tableSet.add(existsTables[0]);
      noImportOfExists = true;
    }
  }
  if (firstTable->type() == PlanType::kDerivedTable) {
    importJoinsIntoFirstDt(firstTable->as<DerivedTable>());
  } else {
    fullyImported = _tables;
  }
  linkTablesToJoins();
}

namespace {
template <typename V, typename E>
void eraseFirst(V& set, E element) {
  auto it = std::find(set.begin(), set.end(), element);
  if (it != set.end()) {
    set.erase(it);
  } else {
    LOG(INFO) << "suspect erase";
  }
}

JoinEdgeP importedDtJoin(
    JoinEdgeP join,
    DerivedTableP dt,
    ExprCP innerKey,
    bool fullyImported) {
  auto left = singleTable(innerKey);
  VELOX_CHECK(left);
  auto otherKey = dt->columns[0];
  auto* newJoin = make<JoinEdge>(
      left, dt, ExprVector{}, false, false, !fullyImported, false);
  newJoin->addEquality(innerKey, otherKey);
  return newJoin;
}

bool isProjected(PlanObjectCP table, const PlanObjectSet& columns) {
  bool projected = false;
  columns.forEach([&](PlanObjectCP column) {
    projected |= column->as<Column>()->relation() == table;
  });
  return projected;
}

// True if 'join'  has max 1 match for a row of 'side'.
bool isUnique(JoinEdgeP join, PlanObjectCP side) {
  return join->sideOf(side, true).isUnique;
}

// Returns a join partner of 'startin 'joins' ' where the partner is
// not in 'visited' Sets 'isFullyImported' to false if the partner is
// not guaranteed n:1 reducing or has columns that are projected out.
PlanObjectCP nextJoin(
    PlanObjectCP start,
    const JoinEdgeVector& joins,
    const PlanObjectSet& columns,
    const PlanObjectSet& visited,
    bool& fullyImported) {
  for (auto& join : joins) {
    auto other = join->otherSide(start);
    if (!other) {
      continue;
    }
    if (visited.contains(other)) {
      continue;
    }
    if (!isUnique(join, other) || isProjected(other, columns)) {
      fullyImported = false;
    }
    return other;
  }
  return nullptr;
}

void joinChain(
    PlanObjectCP start,
    const JoinEdgeVector& joins,
    const PlanObjectSet& columns,
    PlanObjectSet visited,
    bool& fullyImported,
    std::vector<PlanObjectCP>& path) {
  auto next = nextJoin(start, joins, columns, visited, fullyImported);
  if (!next) {
    return;
  }
  visited.add(next);
  path.push_back(next);
  joinChain(next, joins, columns, visited, fullyImported, path);
}

JoinEdgeP importedJoin(
    JoinEdgeP join,
    PlanObjectCP other,
    ExprCP innerKey,
    bool fullyImported) {
  auto left = singleTable(innerKey);
  VELOX_CHECK(left);
  auto otherKey = join->sideOf(other).keys[0];
  auto* newJoin = make<JoinEdge>(
      left, other, ExprVector{}, false, false, !fullyImported, false);
  newJoin->addEquality(innerKey, otherKey);
  return newJoin;
}
} // namespace

void DerivedTable::importJoinsIntoFirstDt(const DerivedTable* firstDt) {
  if (tables.size() == 1 && tables[0]->type() == PlanType::kDerivedTable) {
    flattenDt(tables[0]->as<DerivedTable>());
    return;
  }
  auto initialTables = tables;
  if (firstDt->limit != -1 || firstDt->orderBy) {
    // tables can't be imported but are marked as used so not tried again.
    for (auto i = 1; i < tables.size(); ++i) {
      importedExistences.add(tables[i]);
    }
    return;
  }
  auto& outer = firstDt->columns;
  auto& inner = firstDt->exprs;
  PlanObjectSet projected;
  for (auto& expr : exprs) {
    projected.unionColumns(expr);
  }

  auto* newFirst = make<DerivedTable>(*firstDt->as<DerivedTable>());
  newFirst->cname = firstDt->as<DerivedTable>()->cname;
  for (auto& join : joins) {
    auto other = join->otherSide(firstDt);
    if (!other) {
      continue;
    }
    if (!tableSet.contains(other)) {
      // Already placed in some previous join chain.
      continue;
    }
    auto side = join->sideOf(firstDt);
    if (side.keys.size() > 1 || !join->filter().empty()) {
      continue;
    }
    auto innerKey = importExpr(side.keys[0], outer, inner);
    assert(innerKey);
    if (innerKey->containsFunction(FunctionSet::kAggregate)) {
      // If the join key is an aggregate, the join can't be moved below the agg.
      continue;
    }
    auto otherSide = join->sideOf(firstDt, true);
    PlanObjectSet visited;
    visited.add(firstDt);
    visited.add(other);
    std::vector<PlanObjectCP> path;
    bool fullyImported = otherSide.isUnique;
    joinChain(other, joins, projected, visited, fullyImported, path);
    if (path.empty()) {
      if (other->type() == PlanType::kDerivedTable) {
        const_cast<PlanObject*>(other)->as<DerivedTable>()->makeInitialPlan();
      }

      newFirst->tables.push_back(other);
      newFirst->tableSet.add(other);
      newFirst->joins.push_back(
          importedJoin(join, other, innerKey, fullyImported));
      if (fullyImported) {
        newFirst->fullyImported.add(other);
      }
    } else {
      auto* chainDt = make<DerivedTable>();
      PlanObjectSet chainSet;
      chainSet.add(other);
      if (fullyImported) {
        newFirst->fullyImported.add(other);
      }
      for (auto& object : path) {
        chainSet.add(object);
        if (fullyImported) {
          newFirst->fullyImported.add(object);
        }
      }
      chainDt->makeProjection(otherSide.keys);
      chainDt->import(*this, other, chainSet, {}, 1);
      chainDt->makeInitialPlan();
      newFirst->tables.push_back(chainDt);
      newFirst->tableSet.add(chainDt);
      newFirst->joins.push_back(
          importedDtJoin(join, chainDt, innerKey, fullyImported));
    }
    eraseFirst(tables, other);
    tableSet.erase(other);
    for (auto& table : path) {
      eraseFirst(tables, table);
      tableSet.erase(table);
    }
  }

  VELOX_CHECK_EQ(tables.size(), 1);
  for (auto i = 0; i < initialTables.size(); ++i) {
    if (!newFirst->fullyImported.contains(initialTables[i])) {
      newFirst->importedExistences.add(initialTables[i]);
    }
  }
  tables[0] = newFirst;
  flattenDt(newFirst);
}

void DerivedTable::flattenDt(const DerivedTable* dt) {
  tables = dt->tables;
  cname = dt->cname;
  tableSet = dt->tableSet;
  joins = dt->joins;
  columns = dt->columns;
  exprs = dt->exprs;
  fullyImported = dt->fullyImported;
  importedExistences.unionSet(dt->importedExistences);
  aggregation = dt->aggregation;
  having = dt->having;
}

void DerivedTable::makeProjection(const ExprVector& exprs) {
  auto optimization = queryCtx()->optimization();
  for (auto& expr : exprs) {
    auto* column =
        make<Column>(optimization->newCName("ec"), this, expr->value());
    columns.push_back(column);
    this->exprs.push_back(expr);
  }
}

namespace {
// True if 'expr' is of the form a = b where a depends on one of ''tables' and
// b on the other. If true, returns the side depending on tables[0] in 'left'
// and the other in 'right'.
bool isJoinEquality(
    ExprCP expr,
    std::vector<PlanObjectP>& tables,
    ExprCP& left,
    ExprCP& right) {
  if (expr->type() == PlanType::kCall) {
    auto call = expr->as<Call>();
    if (call->name() == toName("eq")) {
      left = call->argAt(0);
      right = call->argAt(1);
      auto leftTable = singleTable(left);
      auto rightTable = singleTable(right);
      if (!leftTable || !rightTable) {
        return false;
      }
      if (leftTable == tables[1]) {
        std::swap(left, right);
      }
      return true;
    }
  }
  return false;
}

// Finds a JoinEdge between tables[0] and tables[1]. Sets tables[0] to the
// left and [1] to the right table of the found join. Returns the JoinEdge. If
// 'create' is true and no edge is found, makes a new edge with tables[0] as
// left and [1] as right.
JoinEdgeP
findJoin(DerivedTableP dt, std::vector<PlanObjectP>& tables, bool create) {
  for (auto& join : dt->joins) {
    if (join->leftTable() == tables[0] && join->rightTable() == tables[1]) {
      return join;
    }
    if (join->leftTable() == tables[1] && join->rightTable() == tables[0]) {
      std::swap(tables[0], tables[1]);
      return join;
    }
  }
  if (create) {
    auto* join = make<JoinEdge>(
        tables[0], tables[1], ExprVector{}, false, false, false, false);
    dt->joins.push_back(join);
    return join;
  }
  return nullptr;
}
} // namespace

void DerivedTable::distributeConjuncts() {
  std::vector<DerivedTableP> changedDts;
  if (!having.empty()) {
    VELOX_CHECK_NOT_NULL(aggregation);
    VELOX_CHECK_NOT_NULL(aggregation->aggregation);

    // Push HAVING clause that uses only grouping keys below the aggregation.
    //
    // SELECT a, sum(b) FROM t GROUP BY a HAVING a > 0
    //   =>
    //     SELECT a, sum(b) FROM t WHERE a > 0 GROUP BY a

    const auto* op = aggregation->aggregation;

    // Gather the columns of grouping expressions. If a having depends
    // on these alone it can move below the aggregation and gets
    // translated from the aggregation output columns to the columns
    // inside the agg. Consider both the grouping expr nd its rename
    // after the aggregation.
    PlanObjectSet grouping;
    for (auto i = 0; i < op->grouping.size(); ++i) {
      grouping.unionSet(op->columns()[i]->columns());
      grouping.unionSet(op->grouping[i]->columns());
    }

    for (auto i = 0; i < having.size(); ++i) {
      // No pushdown of non-deterministic.
      if (having[i]->containsNonDeterministic()) {
        continue;
      }
      // having that refers to no aggregates goes below the
      // aggregation. Translate from names after agg to pre-agg
      // names. Pre/post agg names may differ for dts in set
      // operations. If already in pre-agg names, no-op.
      if (having[i]->columns().isSubset(grouping)) {
        conjuncts.push_back(importExpr(having[i], op->columns(), op->grouping));
        having.erase(having.begin() + i);
        --i;
      }
    }
  }

  expandConjuncts();

  for (auto i = 0; i < conjuncts.size(); ++i) {
    // No pushdown of non-deterministic.
    if (conjuncts[i]->containsNonDeterministic()) {
      continue;
    }
    PlanObjectSet tableSet = conjuncts[i]->allTables();
    std::vector<PlanObjectP> tables;
    tableSet.forEachMutable([&](auto table) { tables.push_back(table); });
    if (tables.size() == 1) {
      if (tables[0] == this) {
        continue; // the conjunct depends on containing dt, like grouping or
                  // existence flags. Leave in place.
      } else if (tables[0]->type() == PlanType::kDerivedTable) {
        // Translate the column names and add the condition to the conjuncts in
        // the dt. If the inner is a set operation, add the filter to children.
        auto innerDt = tables[0]->as<DerivedTable>();
        auto numChildren =
            innerDt->children.empty() ? 1 : innerDt->children.size();
        for (auto childIdx = 0; childIdx < numChildren; ++childIdx) {
          auto childDt =
              numChildren == 1 ? innerDt : innerDt->children[childIdx];
          auto imported =
              importExpr(conjuncts[i], childDt->columns, childDt->exprs);
          if (childDt->aggregation) {
            childDt->having.push_back(imported);
          } else {
            childDt->conjuncts.push_back(imported);
          }
          if (std::find(changedDts.begin(), changedDts.end(), childDt) ==
              changedDts.end()) {
            changedDts.push_back(childDt);
          }
        }
      } else if (tables[0]->type() == PlanType::kValuesTable) {
        continue; // ValuesTable does not have filter push-down.
      } else {
        VELOX_CHECK_EQ(tables[0]->type(), PlanType::kTable);
        tables[0]->as<BaseTable>()->addFilter(conjuncts[i]);
      }
      conjuncts.erase(conjuncts.begin() + i);
      --numCanonicalConjuncts;
      --i;
      continue;
    }
    if (tables.size() == 2) {
      ExprCP left = nullptr;
      ExprCP right = nullptr;
      // expr depends on 2 tables. If it is left = right or right = left and
      // there is no edge or the edge is inner, add the equality. For other
      // cases, leave the conjunct in place, to be evaluated when its
      // dependences are known.
      if (isJoinEquality(conjuncts[i], tables, left, right)) {
        auto join = findJoin(this, tables, true);
        if (join->isInner()) {
          if (left->type() == PlanType::kColumn &&
              right->type() == PlanType::kColumn) {
            left->as<Column>()->equals(right->as<Column>());
          }
          if (join->leftTable() == tables[0]) {
            join->addEquality(left, right);
          } else {
            join->addEquality(right, left);
          }
          conjuncts.erase(conjuncts.begin() + i);
          --numCanonicalConjuncts;
          --i;
        }
      }
    }
  }
  // Remake initial plan for changedDTs. Calls distributeConjuncts
  // recursively for further pushdown of pushed down items. Replans
  // on returning edge of recursion, so everybody's initial plan is
  // up to date after all pushdowns.
  for (auto* changed : changedDts) {
    changed->makeInitialPlan();
  }
}

namespace {
void flattenAll(ExprCP expr, Name func, ExprVector& flat) {
  if (expr->type() != PlanType::kCall || expr->as<Call>()->name() != func) {
    flat.push_back(expr);
    return;
  }
  for (auto arg : expr->as<Call>()->args()) {
    flattenAll(arg, func, flat);
  }
}

// 'disjuncts' is an OR of ANDs. If each disjunct depends on the same tables
// and if each conjunct inside the ANDs in the OR depends on a single table,
// then return for each distinct table an OR of ANDs. The disjuncts are the
// top vector the conjuncts are the inner vector.
ExprVector extractPerTable(
    const ExprVector& disjuncts,
    std::vector<ExprVector>& orOfAnds) {
  PlanObjectSet tables;
  auto optimization = queryCtx()->optimization();
  auto& names = optimization->builtinNames();
  tables = disjuncts[0]->allTables();
  if (tables.size() <= 1) {
    // All must depend on the same set of more than 1 table.
    return {};
  }
  std::unordered_map<int32_t, std::vector<ExprVector>> perTable;

  for (auto i = 0; i < disjuncts.size(); ++i) {
    if (i > 0 && disjuncts[i]->allTables() != tables) {
      // Does not  depend on the same tables as the other disjuncts.
      return {};
    }
    std::unordered_map<int32_t, ExprVector> perTableAnd;
    ExprVector& inner = orOfAnds[i];
    // do the inner conjuncts each depend on a single table?
    for (auto j = 0; j < inner.size(); ++j) {
      auto single = singleTable(inner[j]);
      if (!single) {
        return {};
      }
      perTableAnd[single->id()].push_back(inner[j]);
    }
    for (auto& pair : perTableAnd) {
      perTable[pair.first].push_back(pair.second);
    }
  }
  ExprVector conjuncts;
  for (auto& pair : perTable) {
    ExprVector tableAnds;
    for (auto& tableAnd : pair.second) {
      tableAnds.push_back(
          optimization->combineLeftDeep(names._and, tableAnd, {}));
    }
    conjuncts.push_back(
        optimization->combineLeftDeep(names._or, tableAnds, {}));
  }
  return conjuncts;
}

/// Analyzes an OR. Returns top level conjuncts that this has
/// inferred from the disjuncts. For example if all have an AND
/// inside and each AND has the same condition then this condition
/// is returned and removed from the disjuncts. 'disjuncts' is
/// changed in place. If 'replacement' is set, then this replaces
/// the whole OR from which 'disjuncts' was flattened.
ExprVector extractCommon(ExprVector& disjuncts, ExprCP* replacement) {
  std::unordered_set<ExprCP> distinct;
  PlanObjectSet tableSet;
  auto optimization = queryCtx()->optimization();
  auto& names = optimization->builtinNames();
  ExprVector result;
  // Remove duplicates.
  bool changeOriginal = false;
  for (auto i = 0; i < disjuncts.size(); ++i) {
    auto disjunct = disjuncts[i];
    if (distinct.find(disjunct) != distinct.end()) {
      disjuncts.erase(disjuncts.begin() + i);
      --i;
      changeOriginal = true;
      continue;
    }
    distinct.insert(disjunct);
  }
  if (disjuncts.size() == 1) {
    *replacement = disjuncts[0];
    return {};
  }

  // The conjuncts  in each of the disjuncts.
  std::vector<ExprVector> flat;
  for (auto i = 0; i < disjuncts.size(); ++i) {
    flat.emplace_back();
    flattenAll(disjuncts[i], names._and, flat.back());
  }
  // Check if the flat conjuncts lists have any element that occurs in all.
  // Remove all the elememts that are in all.
  for (auto j = 0; j < flat[0].size(); ++j) {
    auto item = flat[0][j];
    bool inAll = true;
    for (auto i = 1; i < flat.size(); ++i) {
      if (std::find(flat[i].begin(), flat[i].end(), item) == flat[i].end()) {
        inAll = false;
        break;
      }
    }
    if (inAll) {
      changeOriginal = true;
      result.push_back(item);
      flat[0].erase(flat[0].begin() + j);
      --j;
      for (auto i = 1; i < flat.size(); ++i) {
        flat[i].erase(std::find(flat[i].begin(), flat[i].end(), item));
      }
    }
  }
  auto perTable = extractPerTable(disjuncts, flat);
  if (!perTable.empty()) {
    // The per-table extraction does not alter the original but can surface
    // things to push down.
    result.insert(result.end(), perTable.begin(), perTable.end());
  }
  if (changeOriginal) {
    ExprVector ands;
    for (auto inner : flat) {
      ands.push_back(optimization->combineLeftDeep(names._and, inner, {}));
    }
    *replacement = optimization->combineLeftDeep(names._or, ands, {});
  }

  return result;
}

} // namespace

void DerivedTable::expandConjuncts() {
  const auto& names = queryCtx()->optimization()->builtinNames();
  bool any;
  std::unordered_set<int32_t> processed;
  auto firstUnprocessed = numCanonicalConjuncts;
  do {
    any = false;
    const int32_t numProcessed = conjuncts.size();
    const auto end = conjuncts.size();
    for (auto i = firstUnprocessed; i < end; ++i) {
      const auto& conjunct = conjuncts[i];
      if (isCallExpr(conjunct, names._or) &&
          !conjunct->containsNonDeterministic()) {
        ExprVector flat;
        flattenAll(conjunct, names._or, flat);
        ExprCP replace = nullptr;
        ExprVector common = extractCommon(flat, &replace);
        if (replace) {
          any = true;
          conjuncts[i] = replace;
        }
        if (!common.empty()) {
          any = true;
          conjuncts.insert(conjuncts.end(), common.begin(), common.end());
        }
      }
    }
    firstUnprocessed = numProcessed;
    numCanonicalConjuncts = numProcessed;
  } while (any);
}

void DerivedTable::makeInitialPlan() {
  auto optimization = queryCtx()->optimization();
  MemoKey key;
  key.firstTable = this;
  key.tables.add(this);
  for (auto& column : columns) {
    key.columns.add(column);
  }
  distributeConjuncts();
  addImpliedJoins();
  linkTablesToJoins();
  for (auto& join : joins) {
    join->guessFanout();
  }
  setStartTables();
  PlanState state(*optimization, this);
  for (auto expr : exprs) {
    state.targetColumns.unionColumns(expr);
  }

  optimization->makeJoins(nullptr, state);
  Distribution emptyDistribution;
  bool needsShuffle;
  auto plan = state.plans.best(emptyDistribution, needsShuffle)->op;
  auto& distribution = plan->distribution();
  ExprVector partition = distribution.partition;
  ExprVector order = distribution.order;
  auto orderType = distribution.orderType;
  replace(partition, exprs, columns.data());
  replace(order, exprs, columns.data());
  auto* dtDist = make<Distribution>(
      distribution.distributionType,
      distribution.cardinality,
      partition,
      order,
      orderType);
  this->distribution = dtDist;
  optimization->memo()[key] = std::move(state.plans);
}

std::string DerivedTable::toString() const {
  std::stringstream out;
  out << "{dt " << cname << " from ";
  for (auto& table : tables) {
    out << table->toString() << " ";
  }
  out << " where ";
  for (auto& join : joins) {
    out << join->toString();
  }
  if (!conjuncts.empty()) {
    out << " where " << conjunctsToString(conjuncts);
  }
  out << "}";
  return out.str();
}

void DerivedTable::addJoinedBy(JoinEdgeP join) {
  pushBackUnique(joinedBy, join);
}

} // namespace facebook::velox::optimizer
