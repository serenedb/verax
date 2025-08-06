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

#include "axiom/optimizer/QueryGraph.h"
#include "axiom/optimizer/Plan.h"
#include "axiom/optimizer/PlanUtils.h"
#include "velox/expression/ScopedVarSetter.h"

namespace facebook::velox::optimizer {

void Column::equals(ColumnCP other) const {
  if (!equivalence_ && !other->equivalence_) {
    auto* equiv = make<Equivalence>();
    equiv->columns.push_back(this);
    equiv->columns.push_back(other);
    equivalence_ = equiv;
    other->equivalence_ = equiv;
    return;
  }
  if (!other->equivalence_) {
    other->equivalence_ = equivalence_;
    equivalence_->columns.push_back(other);
    return;
  }
  if (!equivalence_) {
    other->equals(this);
    return;
  }
  for (auto& column : other->equivalence_->columns) {
    equivalence_->columns.push_back(column);
    column->equivalence_ = equivalence_;
  }
}

std::string Column::toString() const {
  auto* opt = queryCtx()->optimization();
  if (!-opt->cnamesInExpr()) {
    return name_;
  }
  Name cname = !relation_ ? ""
      : relation_->type() == PlanType::kTable
      ? relation_->as<BaseTable>()->cname
      : relation_->type() == PlanType::kDerivedTable
      ? relation_->as<DerivedTable>()->cname
      : "--";

  // Map corre;correlation names to canonical if making keys for history pieces.
  auto canonical = opt->canonicalCnames();
  if (canonical) {
    auto it = canonical->find(cname);
    if (it != canonical->end()) {
      cname = it->second;
    }
  }
  return fmt::format("{}.{}", cname, name_);
}

std::string Literal::toString() const {
  std::stringstream out;
  if (vector_) {
    out << vector_->toString(0);
  } else {
    out << *literal_;
  }
  return out.str();
}

std::string Call::toString() const {
  std::stringstream out;
  out << name_ << "(";
  for (auto i = 0; i < args_.size(); ++i) {
    out << args_[i]->toString() << (i == args_.size() - 1 ? ")" : ", ");
  }
  return out.str();
}

std::string Field::toString() const {
  std::stringstream out;
  out << base_->toString() << ".";
  if (field_) {
    out << field_;
  } else {
    out << fmt::format("{}", index_);
  }
  return out.str();
}

std::optional<BitSet> SubfieldSet::findSubfields(int32_t id) const {
  for (auto i = 0; i < ids.size(); ++i) {
    if (ids[i] == id) {
      return subfields[i];
    }
  }
  return std::nullopt;
}

void BaseTable::addJoinedBy(JoinEdgeP join) {
  pushBackUnique(joinedBy, join);
}

std::optional<int32_t> BaseTable::columnId(Name column) const {
  for (auto i = 0; i < columns.size(); ++i) {
    if (columns[i]->name() == column) {
      return columns[i]->id();
    }
  }
  return std::nullopt;
}

BitSet BaseTable::columnSubfields(
    int32_t id,
    bool controlOnly,
    bool payloadOnly) const {
  BitSet subfields;
  if (!controlOnly) {
    if (auto maybe = payloadSubfields.findSubfields(id)) {
      subfields = maybe.value();
    }
  }
  if (!payloadOnly) {
    if (auto maybe = controlSubfields.findSubfields(id)) {
      subfields.unionSet(maybe.value());
    }
  }
  Path::subfieldSkyline(subfields);
  return subfields;
}

std::string BaseTable::toString() const {
  std::stringstream out;
  out << "{" << PlanObject::toString();
  out << schemaTable->name << " " << cname << "}";
  return out.str();
}

void ValuesTable::addJoinedBy(JoinEdgeP join) {
  pushBackUnique(joinedBy, join);
}

std::string ValuesTable::toString() const {
  std::stringstream out;
  out << "{" << PlanObject::toString();
  out << values.id() << " " << cname << "}";
  return out.str();
}

const JoinSide JoinEdge::sideOf(PlanObjectCP side, bool other) const {
  if ((side == rightTable_ && !other) || (side == leftTable_ && other)) {
    return {
        rightTable_,
        rightKeys_,
        lrFanout_,
        rightOptional_,
        leftOptional_,
        rightExists_,
        rightNotExists_,
        markColumn_,
        rightUnique_};
  }
  return {
      leftTable_,
      leftKeys_,
      rlFanout_,
      leftOptional_,
      rightOptional_,
      false,
      false,
      nullptr,
      leftUnique_};
}

bool JoinEdge::isBroadcastableType() const {
  return !leftOptional_;
}

void JoinEdge::addEquality(ExprCP left, ExprCP right, bool update) {
  leftKeys_.push_back(left);
  rightKeys_.push_back(right);
  if (update) {
    guessFanout();
  }
}

std::pair<std::string, bool> JoinEdge::sampleKey() const {
  std::stringstream out;
  if (!leftTable_ || leftTable_->type() != PlanType::kTable ||
      rightTable_->type() != PlanType::kTable) {
    return std::make_pair("", false);
  }
  auto* opt = queryCtx()->optimization();
  ScopedVarSetter pref(&opt->cnamesInExpr(), false);
  std::vector<int32_t> indices(leftKeys_.size());
  std::iota(indices.begin(), indices.end(), 0);
  std::vector<std::string> leftString;
  for (auto& k : leftKeys_) {
    leftString.push_back(k->toString());
  }
  std::sort(indices.begin(), indices.end(), [&](int32_t l, int32_t r) {
    return leftString[l] < leftString[r];
  });
  auto left =
      fmt::format("{} ", leftTable_->as<BaseTable>()->schemaTable->name);
  auto right =
      fmt::format("{} ", rightTable_->as<BaseTable>()->schemaTable->name);
  for (auto i : indices) {
    left += leftKeys_[i]->toString() + " ";
    right += rightKeys_[i]->toString() + " ";
  }
  if (left < right) {
    return std::make_pair(left + " " + right, false);
  }
  return std::make_pair(right + " " + left, true);
}

std::string JoinEdge::toString() const {
  std::stringstream out;
  out << "<join "
      << (leftTable_ ? leftTable_->toString() : " multiple tables ");
  if (leftOptional_ && rightOptional_) {
    out << " full outr ";
  } else if (markColumn_) {
    out << " exists project ";
  } else if (rightOptional_) {
    out << " left";
  } else if (rightExists_) {
    out << " exists ";
  } else if (rightNotExists_) {
    out << " not exists ";
  } else if (leftOptional_) {
    out << "right";
  } else {
    out << " inner ";
  }
  out << rightTable_->toString();
  out << " on ";
  for (auto i = 0; i < leftKeys_.size(); ++i) {
    out << leftKeys_[i]->toString() << " = " << rightKeys_[i]->toString()
        << (i < leftKeys_.size() - 1 ? " and " : "");
  }
  if (!filter_.empty()) {
    out << " filter " << conjunctsToString(filter_);
  }
  out << ">";
  return out.str();
}

const FunctionSet& Expr::functions() const {
  static FunctionSet empty;
  return empty;
}

bool Expr::sameOrEqual(const Expr& other) const {
  if (this == &other) {
    return true;
  }
  if (type() != other.type()) {
    return false;
  }
  switch (type()) {
    case PlanType::kColumn:
      return as<Column>()->equivalence() &&
          as<Column>()->equivalence() == other.as<Column>()->equivalence();
    case PlanType::kAggregate: {
      auto a = reinterpret_cast<const Aggregate*>(this);
      auto b = reinterpret_cast<const Aggregate*>(&other);
      if (a->isDistinct() != b->isDistinct() ||
          a->isAccumulator() != b->isAccumulator() ||
          !(a->condition() == b->condition() ||
            (a->condition() && b->condition() &&
             a->condition()->sameOrEqual(*b->condition())))) {
        return false;
      }
    }
      [[fallthrough]];
    case PlanType::kCall: {
      if (as<Call>()->name() != other.as<Call>()->name()) {
        return false;
      }
      auto numArgs = as<Call>()->args().size();
      if (numArgs != other.as<Call>()->args().size()) {
        return false;
      }
      for (auto i = 0; i < numArgs; ++i) {
        if (as<Call>()->argAt(i)->sameOrEqual(*other.as<Call>()->argAt(i))) {
          return false;
        }
      }
      return true;
    }
    default:
      return false;
  }
}

PlanObjectCP Expr::singleTable() const {
  if (type() == PlanType::kColumn) {
    return as<Column>()->relation();
  }
  PlanObjectCP table = nullptr;
  bool multiple = false;
  columns_.forEach([&](PlanObjectCP object) {
    VELOX_CHECK(object->type() == PlanType::kColumn);
    if (!table) {
      table = object->template as<Column>()->relation();
    } else if (table != object->as<Column>()->relation()) {
      multiple = true;
    }
  });
  return multiple ? nullptr : table;
}

PlanObjectSet Expr::allTables() const {
  PlanObjectSet set;
  columns_.forEach(
      [&](PlanObjectCP object) { set.add(object->as<Column>()->relation()); });
  return set;
}

PlanObjectSet allTables(CPSpan<Expr> exprs) {
  PlanObjectSet all;
  for (auto expr : exprs) {
    auto set = expr->allTables();
    all.unionSet(set);
  }
  return all;
}

Column::Column(
    Name name,
    PlanObjectP relation,
    const Value& value,
    Name nameInTable,
    ColumnCP top,
    PathCP path)
    : Expr(PlanType::kColumn, value),
      name_(name),
      relation_(relation),
      topColumn_(top),
      path_(path) {
  columns_.add(this);
  subexpressions_.add(this);
  if (relation_ && relation_->type() == PlanType::kTable) {
    if (topColumn_) {
      schemaColumn_ = topColumn_->schemaColumn_;
    } else {
      schemaColumn_ = relation->as<BaseTable>()->schemaTable->findColumn(
          nameInTable ? nameInTable : name_);
      VELOX_CHECK(schemaColumn_);
    }
  }
}

// Returns a copy of 'expr', replacing instances of columns in 'outer' with
// the corresponding expression from 'inner'
ExprCP
importExpr(ExprCP expr, const ColumnVector& outer, const ExprVector& inner) {
  if (!expr) {
    return nullptr;
  }
  switch (expr->type()) {
    case PlanType::kColumn:
      for (auto i = 0; i < inner.size(); ++i) {
        if (outer[i] == expr) {
          return inner[i];
        }
      }
      return expr;
    case PlanType::kLiteral:
      return expr;
    case PlanType::kCall:
    case PlanType::kAggregate: {
      auto children = expr->children();
      std::vector<ExprCP> newChildren(children.size());
      FunctionSet functions;
      bool anyChange = false;
      for (auto i = 0; i < children.size(); ++i) {
        newChildren[i] = importExpr(children[i]->as<Expr>(), outer, inner);
        anyChange |= newChildren[i] != children[i];
        if (newChildren[i]->isFunction()) {
          functions = functions | newChildren[i]->as<Call>()->functions();
        }
      }
      ExprCP newCondition = nullptr;
      if (expr->type() == PlanType::kAggregate) {
        newCondition =
            importExpr(expr->as<Aggregate>()->condition(), outer, inner);
        anyChange |= newCondition != expr->as<Aggregate>()->condition();

        if (newCondition && newCondition->isFunction()) {
          functions = functions | newCondition->as<Call>()->functions();
        }
      }
      if (!anyChange) {
        return expr;
      }
      ExprVector childVector;
      childVector.insert(
          childVector.begin(), newChildren.begin(), newChildren.end());
      if (expr->type() == PlanType::kCall) {
        auto call = expr->as<Call>();
        auto* copy = make<Call>(
            call->name(), call->value(), std::move(childVector), functions);
        return copy;
      } else if (expr->type() == PlanType::kAggregate) {
        auto aggregate = expr->as<Aggregate>();
        auto* copy = make<Aggregate>(
            aggregate->name(),
            aggregate->value(),
            std::move(childVector),
            functions,
            aggregate->isDistinct(),
            newCondition,
            aggregate->isAccumulator(),
            aggregate->intermediateType());
        return copy;
      }
    }
      [[fallthrough]];
    default:
      VELOX_UNREACHABLE();
  }
}

void BaseTable::addFilter(ExprCP expr) {
  const auto& columns = expr->columns();
  bool isMultiColumn = false;
  bool isSingleColumn = false;
  columns.forEach([&](PlanObjectCP object) {
    if (!isMultiColumn) {
      if (isSingleColumn) {
        isMultiColumn = true;
      } else {
        isSingleColumn = true;
      }
    };
  });
  if (isSingleColumn) {
    columnFilters.push_back(expr);
    filterUpdated(this);
    return;
  }
  filter.push_back(expr);
  filterUpdated(this);
}

void extractNonInnerJoinEqualities(
    ExprVector& conjuncts,
    PlanObjectCP right,
    ExprVector& leftKeys,
    ExprVector& rightKeys,
    PlanObjectSet& allLeft) {
  const auto* eq = toName("eq");

  for (auto i = 0; i < conjuncts.size(); ++i) {
    const auto* conjunct = conjuncts[i];
    if (isCallExpr(conjunct, eq)) {
      auto eq = conjunct->as<Call>();
      auto leftTables = eq->argAt(0)->allTables();
      auto rightTables = eq->argAt(1)->allTables();
      if (rightTables.size() == 1 && rightTables.contains(right) &&
          !leftTables.contains(right)) {
        allLeft.unionSet(leftTables);
        leftKeys.push_back(eq->argAt(0));
        rightKeys.push_back(eq->argAt(1));
        conjuncts.erase(conjuncts.begin() + i);
        --i;
      } else if (
          leftTables.size() == 1 && leftTables.contains(right) &&
          !rightTables.contains(right)) {
        allLeft.unionSet(rightTables);
        leftKeys.push_back(eq->argAt(1));
        rightKeys.push_back(eq->argAt(0));
        conjuncts.erase(conjuncts.begin() + i);
        --i;
      }
    }
  }
}

namespace {
template <typename U>
inline CPSpan<Column> toRangeCast(const ExprVector& exprs) {
  return CPSpan<Column>(
      reinterpret_cast<const Column* const*>(exprs.data()), exprs.size());
}
} // namespace

float tableCardinality(PlanObjectCP table) {
  if (table->type() == PlanType::kTable) {
    return table->as<BaseTable>()
        ->schemaTable->columnGroups[0]
        ->distribution()
        .cardinality;
  }
  if (table->type() == PlanType::kValuesTable) {
    return table->as<ValuesTable>()->cardinality;
  }
  VELOX_CHECK_EQ(table->type(), PlanType::kDerivedTable);
  return table->as<DerivedTable>()->distribution->cardinality;
}

void JoinEdge::guessFanout() {
  if (fanoutsFixed_) {
    return;
  }
  auto* opt = queryCtx()->optimization();
  auto samplePair = opt->history().sampleJoin(this);
  auto left = joinCardinality(leftTable_, toRangeCast<Column>(leftKeys_));
  auto right = joinCardinality(rightTable_, toRangeCast<Column>(rightKeys_));
  leftUnique_ = left.unique;
  rightUnique_ = right.unique;
  if (samplePair.first == 0 && samplePair.second == 0) {
    lrFanout_ = right.joinCardinality * baseSelectivity(rightTable_);
    rlFanout_ = left.joinCardinality * baseSelectivity(leftTable_);
  } else {
    lrFanout_ = samplePair.second * baseSelectivity(rightTable_);
    rlFanout_ = samplePair.first * baseSelectivity(leftTable_);
  }
  // If one side is unique, the other side is a pk to fk join, with fanout =
  // fk-table-card / pk-table-card.
  if (rightUnique_) {
    lrFanout_ = baseSelectivity(rightTable_);
    rlFanout_ = tableCardinality(leftTable_) / tableCardinality(rightTable_) *
        baseSelectivity(leftTable_);
  }
  if (leftUnique_) {
    rlFanout_ = baseSelectivity(leftTable_);
    lrFanout_ = tableCardinality(rightTable_) / tableCardinality(leftTable_) *
        baseSelectivity(rightTable_);
  }
}

void exprsToString(const ExprVector& exprs, std::stringstream& out) {
  int32_t size = exprs.size();
  for (auto i = 0; i < size; ++i) {
    out << exprs[i]->toString() << (i < size - 1 ? ", " : "");
  }
}

} // namespace facebook::velox::optimizer
