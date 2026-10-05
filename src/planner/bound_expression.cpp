#include "planner/bound_expression.h"

#include "common/assert.h"

namespace cdb {

const char* OperatorText(OperatorKind op) noexcept {
    switch (op) {
    case OperatorKind::Add:
        return "+";
    case OperatorKind::Sub:
        return "-";
    case OperatorKind::Mul:
        return "*";
    case OperatorKind::Div:
        return "/";
    case OperatorKind::Mod:
        return "%";
    case OperatorKind::Negate:
        return "-";
    case OperatorKind::Concat:
        return "||";
    case OperatorKind::Eq:
        return "=";
    case OperatorKind::Ne:
        return "<>";
    case OperatorKind::Lt:
        return "<";
    case OperatorKind::Le:
        return "<=";
    case OperatorKind::Gt:
        return ">";
    case OperatorKind::Ge:
        return ">=";
    case OperatorKind::And:
        return "AND";
    case OperatorKind::Or:
        return "OR";
    case OperatorKind::Not:
        return "NOT";
    }
    return "?";
}

const char* FunctionName(FunctionId id) noexcept {
    switch (id) {
    case FunctionId::Like:
        return "like";
    case FunctionId::Year:
        return "year";
    case FunctionId::Month:
        return "month";
    case FunctionId::Day:
        return "day";
    case FunctionId::DayOfWeek:
        return "dayofweek";
    case FunctionId::DayOfYear:
        return "dayofyear";
    case FunctionId::Quarter:
        return "quarter";
    case FunctionId::Substring:
        return "substring";
    case FunctionId::Length:
        return "length";
    case FunctionId::Upper:
        return "upper";
    case FunctionId::Lower:
        return "lower";
    case FunctionId::Abs:
        return "abs";
    case FunctionId::Round:
        return "round";
    case FunctionId::Floor:
        return "floor";
    case FunctionId::Ceil:
        return "ceil";
    case FunctionId::Coalesce:
        return "coalesce";
    case FunctionId::NullIf:
        return "nullif";
    }
    return "?";
}

const char* AggregateName(AggregateKind kind) noexcept {
    switch (kind) {
    case AggregateKind::Count:
        return "count";
    case AggregateKind::CountStar:
        return "count_star";
    case AggregateKind::Sum:
        return "sum";
    case AggregateKind::Avg:
        return "avg";
    case AggregateKind::Min:
        return "min";
    case AggregateKind::Max:
        return "max";
    }
    return "?";
}

BoundExprPtr BoundExpr::ColumnRef(idx_t ordinal, LogicalType type, std::string name) {
    auto e = std::make_unique<BoundExpr>(BoundKind::ColumnRef, type);
    e->ordinal = ordinal;
    e->name = std::move(name);
    return e;
}

BoundExprPtr BoundExpr::Constant(Value value) {
    auto e = std::make_unique<BoundExpr>(BoundKind::Constant, value.type());
    e->value = std::move(value);
    return e;
}

BoundExprPtr BoundExpr::Cast(BoundExprPtr child, LogicalType target) {
    auto e = std::make_unique<BoundExpr>(BoundKind::Cast, target);
    e->children.push_back(std::move(child));
    return e;
}

BoundExprPtr BoundExpr::Unary(OperatorKind op, BoundExprPtr child, LogicalType type) {
    auto e = std::make_unique<BoundExpr>(BoundKind::Operator, type);
    e->op = op;
    e->children.push_back(std::move(child));
    return e;
}

BoundExprPtr BoundExpr::Binary(OperatorKind op, BoundExprPtr l, BoundExprPtr r, LogicalType type) {
    auto e = std::make_unique<BoundExpr>(BoundKind::Operator, type);
    e->op = op;
    e->children.push_back(std::move(l));
    e->children.push_back(std::move(r));
    return e;
}

BoundExprPtr BoundExpr::Call(FunctionId id, std::vector<BoundExprPtr> args, LogicalType type) {
    auto e = std::make_unique<BoundExpr>(BoundKind::Function, type);
    e->function = id;
    e->children = std::move(args);
    return e;
}

BoundExprPtr BoundExpr::Case(std::vector<BoundExprPtr> whens_thens_else, LogicalType type) {
    CDB_CHECK(whens_thens_else.size() >= 3 && whens_thens_else.size() % 2 == 1);
    auto e = std::make_unique<BoundExpr>(BoundKind::Case, type);
    e->children = std::move(whens_thens_else);
    return e;
}

BoundExprPtr BoundExpr::InList(BoundExprPtr value, std::vector<BoundExprPtr> items, bool negated) {
    auto e = std::make_unique<BoundExpr>(BoundKind::InList, LogicalType::Boolean());
    e->children.push_back(std::move(value));
    for (auto& i : items)
        e->children.push_back(std::move(i));
    e->flag = negated;
    return e;
}

BoundExprPtr BoundExpr::IsNull(BoundExprPtr child, bool negated) {
    auto e = std::make_unique<BoundExpr>(BoundKind::IsNull, LogicalType::Boolean());
    e->children.push_back(std::move(child));
    e->flag = negated;
    return e;
}

BoundExprPtr BoundExpr::Aggregate(AggregateKind kind, std::vector<BoundExprPtr> args, bool distinct,
                                  LogicalType type) {
    auto e = std::make_unique<BoundExpr>(BoundKind::Aggregate, type);
    e->aggregate = kind;
    e->children = std::move(args);
    e->flag = distinct;
    return e;
}

BoundExprPtr BoundExpr::Clone() const {
    auto e = std::make_unique<BoundExpr>(kind, type);
    e->ordinal = ordinal;
    e->name = name;
    if (value)
        e->value = value;
    e->op = op;
    e->function = function;
    e->aggregate = aggregate;
    e->flag = flag;
    e->children.reserve(children.size());
    for (const auto& c : children)
        e->children.push_back(c->Clone());
    return e;
}

bool BoundExpr::Equals(const BoundExpr& o) const {
    if (kind != o.kind || type != o.type || children.size() != o.children.size())
        return false;
    switch (kind) {
    case BoundKind::ColumnRef:
        if (ordinal != o.ordinal)
            return false;
        break;
    case BoundKind::Constant:
        if (!(*value == *o.value))
            return false;
        break;
    case BoundKind::Operator:
        if (op != o.op)
            return false;
        break;
    case BoundKind::Function:
        if (function != o.function)
            return false;
        break;
    case BoundKind::Aggregate:
        if (aggregate != o.aggregate || flag != o.flag)
            return false;
        break;
    case BoundKind::InList:
    case BoundKind::IsNull:
        if (flag != o.flag)
            return false;
        break;
    case BoundKind::Cast:
    case BoundKind::Case:
        break;
    }
    for (size_t i = 0; i < children.size(); i++) {
        if (!children[i]->Equals(*o.children[i]))
            return false;
    }
    return true;
}

namespace {

std::string Quote(const Value& v) {
    if (v.IsNull())
        return "NULL";
    switch (v.type().id()) {
    case TypeId::Varchar: {
        std::string out = "'";
        for (char c : v.GetVarchar()) {
            if (c == '\'')
                out += '\'';
            out += c;
        }
        return out + "'";
    }
    case TypeId::Date:
        return "DATE '" + v.ToString() + "'";
    case TypeId::BigInt:
        return v.ToString() + "::BIGINT";
    default:
        return v.ToString();
    }
}

} // namespace

std::string BoundExpr::ToString() const {
    switch (kind) {
    case BoundKind::ColumnRef:
        return name.empty() ? "#" + std::to_string(ordinal) : name;
    case BoundKind::Constant:
        return Quote(*value);
    case BoundKind::Cast:
        return "CAST(" + children[0]->ToString() + " AS " + type.ToString() + ")";
    case BoundKind::Operator:
        if (children.size() == 1) {
            return op == OperatorKind::Not ? "(NOT " + children[0]->ToString() + ")"
                                           : "(-" + children[0]->ToString() + ")";
        }
        return "(" + children[0]->ToString() + " " + OperatorText(op) + " " +
               children[1]->ToString() + ")";
    case BoundKind::Function: {
        std::string out = std::string(FunctionName(function)) + "(";
        for (size_t i = 0; i < children.size(); i++) {
            if (i > 0)
                out += ", ";
            out += children[i]->ToString();
        }
        return out + ")";
    }
    case BoundKind::Case: {
        std::string out = "CASE";
        for (size_t i = 0; i + 1 < children.size(); i += 2) {
            out += " WHEN " + children[i]->ToString() + " THEN " + children[i + 1]->ToString();
        }
        return out + " ELSE " + children.back()->ToString() + " END";
    }
    case BoundKind::InList: {
        std::string out = "(" + children[0]->ToString() + (flag ? " NOT IN (" : " IN (");
        for (size_t i = 1; i < children.size(); i++) {
            if (i > 1)
                out += ", ";
            out += children[i]->ToString();
        }
        return out + "))";
    }
    case BoundKind::IsNull:
        return "(" + children[0]->ToString() + (flag ? " IS NOT NULL)" : " IS NULL)");
    case BoundKind::Aggregate: {
        if (aggregate == AggregateKind::CountStar)
            return "count(*)";
        std::string out = std::string(AggregateName(aggregate)) + "(" + (flag ? "DISTINCT " : "");
        for (size_t i = 0; i < children.size(); i++) {
            if (i > 0)
                out += ", ";
            out += children[i]->ToString();
        }
        return out + ")";
    }
    }
    return "?";
}

bool BoundExpr::IsConstantTree() const {
    if (kind == BoundKind::ColumnRef || kind == BoundKind::Aggregate)
        return false;
    for (const auto& c : children) {
        if (!c->IsConstantTree())
            return false;
    }
    return true;
}

bool BoundExpr::ContainsAggregate() const {
    if (kind == BoundKind::Aggregate)
        return true;
    for (const auto& c : children) {
        if (c->ContainsAggregate())
            return true;
    }
    return false;
}

void BoundExpr::ForEach(const std::function<void(const BoundExpr&)>& fn) const {
    fn(*this);
    for (const auto& c : children)
        c->ForEach(fn);
}

} // namespace cdb
