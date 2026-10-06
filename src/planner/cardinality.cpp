#include "planner/cardinality.h"

#include "planner/expr_util.h"
#include "storage/table_statistics.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace cdb {

namespace {

constexpr double kDefaultEquality = 0.1;
constexpr double kDefaultRange = 1.0 / 3.0;
constexpr double kDefaultOther = 0.5;
constexpr double kDefaultNullFraction = 0.05;

double Clamp01(double x) {
    return std::min(1.0, std::max(0.0, x));
}

// The constant under casts, or null.
const Value* AsConstant(const BoundExpr& e) {
    const BoundExpr* x = &e;
    while (x->kind == BoundKind::Cast) {
        x = x->children[0].get();
    }
    return x->kind == BoundKind::Constant && x->value.has_value() ? &*x->value : nullptr;
}

const ColumnEstimate* ColumnOf(const Estimate& in, const BoundExpr& e) {
    const BoundExpr* c = AsColumnRef(e);
    return c != nullptr && c->ordinal < in.columns.size() ? &in.columns[c->ordinal] : nullptr;
}

enum class Cmp { Eq, Ne, Lt, Le, Gt, Ge };

std::optional<Cmp> ComparisonOf(OperatorKind op) {
    switch (op) {
    case OperatorKind::Eq:
        return Cmp::Eq;
    case OperatorKind::Ne:
        return Cmp::Ne;
    case OperatorKind::Lt:
        return Cmp::Lt;
    case OperatorKind::Le:
        return Cmp::Le;
    case OperatorKind::Gt:
        return Cmp::Gt;
    case OperatorKind::Ge:
        return Cmp::Ge;
    default:
        return std::nullopt;
    }
}

Cmp Flip(Cmp c) { // `constant op column` seen as `column op' constant`
    switch (c) {
    case Cmp::Lt:
        return Cmp::Gt;
    case Cmp::Le:
        return Cmp::Ge;
    case Cmp::Gt:
        return Cmp::Lt;
    case Cmp::Ge:
        return Cmp::Le;
    default:
        return c;
    }
}

// Orders two values of the same family: numbers by value, strings bytewise; false if they are not
// comparable (different families).
bool Compare(const Value& a, const Value& b, int& out) {
    const auto x = NumericValue(a), y = NumericValue(b);
    if (x && y) {
        out = *x < *y ? -1 : (*x > *y ? 1 : 0);
        return true;
    }
    if (a.type().id() == TypeId::Varchar && b.type().id() == TypeId::Varchar) {
        out = Value::Compare(a, b);
        return true;
    }
    return false;
}

double EqualitySelectivity(const ColumnEstimate& c, const Value* k) {
    if (k != nullptr) {
        if (k->IsNull()) {
            return 0;
        }
        int lo = 0, hi = 0;
        if (c.min && c.max && Compare(*k, *c.min, lo) && Compare(*k, *c.max, hi) &&
            (lo < 0 || hi > 0)) {
            return 0; // outside the bounds: no row has it
        }
    }
    return (1.0 - c.null_fraction) / std::max(1.0, c.distinct);
}

// P(column <op> k) for a range comparison.
double RangeSelectivity(const ColumnEstimate& c, Cmp op, const Value& k) {
    const double non_null = 1.0 - c.null_fraction;
    if (k.IsNull()) {
        return 0;
    }
    if (!c.min || !c.max) {
        return non_null * kDefaultRange;
    }
    const auto kn = NumericValue(k), lo = NumericValue(*c.min), hi = NumericValue(*c.max);
    if (kn && lo && hi) {
        // The values are taken to sit on a grid of `points` equally spaced values from the minimum
        // to the maximum (integers and dates in a dense range: every integer in it), each equally
        // frequent. A continuous model would put `qty > 49` over the integers 1..50 at nothing
        // instead of one value in fifty.
        const TypeId id = c.min->type().id();
        const bool whole = id == TypeId::Integer || id == TypeId::BigInt || id == TypeId::Date ||
                           id == TypeId::Boolean;
        double points = c.distinct;
        if (whole && points >= 0.9 * (*hi - *lo + 1)) {
            points = *hi - *lo + 1;
        }
        double at_most, below; // fractions of the values <= k and < k
        if (*hi <= *lo || points < 2) {
            at_most = *kn >= *lo ? 1.0 : 0.0;
            below = *kn > *lo ? 1.0 : 0.0;
        } else {
            const double step = (*hi - *lo) / (points - 1);
            double position = (*kn - *lo) / step;
            const double nearest = std::round(position);
            if (std::fabs(position - nearest) < 1e-9) {
                position = nearest; // a value on the grid: no rounding noise in floor / ceil
            }
            at_most = Clamp01((std::floor(position) + 1) / points);
            below = Clamp01(std::ceil(position) / points);
        }
        switch (op) {
        case Cmp::Lt:
            return non_null * below;
        case Cmp::Le:
            return non_null * at_most;
        case Cmp::Gt:
            return non_null * (1.0 - at_most);
        default:
            return non_null * (1.0 - below);
        }
    }
    int vs_lo = 0, vs_hi = 0; // strings: only the extremes tell
    if (!Compare(k, *c.min, vs_lo) || !Compare(k, *c.max, vs_hi)) {
        return non_null * kDefaultRange;
    }
    switch (op) {
    case Cmp::Lt:
        return non_null * (vs_lo <= 0 ? 0.0 : (vs_hi > 0 ? 1.0 : kDefaultRange));
    case Cmp::Le:
        return non_null * (vs_lo < 0 ? 0.0 : (vs_hi >= 0 ? 1.0 : kDefaultRange));
    case Cmp::Gt:
        return non_null * (vs_hi >= 0 ? 0.0 : (vs_lo < 0 ? 1.0 : kDefaultRange));
    default:
        return non_null * (vs_hi > 0 ? 0.0 : (vs_lo <= 0 ? 1.0 : kDefaultRange));
    }
}

double ComparisonSelectivity(const BoundExpr& e, Cmp op, const Estimate& in) {
    const BoundExpr &a = *e.children[0], &b = *e.children[1];
    const ColumnEstimate* ca = ColumnOf(in, a);
    const ColumnEstimate* cb = ColumnOf(in, b);
    const Value* ka = AsConstant(a);
    const Value* kb = AsConstant(b);
    if (ca != nullptr && kb != nullptr) {
        switch (op) {
        case Cmp::Eq:
            return EqualitySelectivity(*ca, kb);
        case Cmp::Ne:
            return kb->IsNull() ? 0
                                : Clamp01(1.0 - ca->null_fraction - EqualitySelectivity(*ca, kb));
        default:
            return RangeSelectivity(*ca, op, *kb);
        }
    }
    if (cb != nullptr && ka != nullptr) {
        switch (op) {
        case Cmp::Eq:
            return EqualitySelectivity(*cb, ka);
        case Cmp::Ne:
            return ka->IsNull() ? 0
                                : Clamp01(1.0 - cb->null_fraction - EqualitySelectivity(*cb, ka));
        default:
            return RangeSelectivity(*cb, Flip(op), *ka);
        }
    }
    if (ca != nullptr && cb != nullptr) { // two columns of the same input
        const double non_null = (1.0 - ca->null_fraction) * (1.0 - cb->null_fraction);
        const double equal = non_null / std::max({1.0, ca->distinct, cb->distinct});
        switch (op) {
        case Cmp::Eq:
            return equal;
        case Cmp::Ne:
            return Clamp01(non_null - equal);
        default:
            return non_null * kDefaultRange;
        }
    }
    switch (op) {
    case Cmp::Eq:
        return kDefaultEquality;
    case Cmp::Ne:
        return 1.0 - kDefaultEquality;
    default:
        return kDefaultRange;
    }
}

double LikeSelectivity(const BoundExpr& e, const Estimate& in) {
    const ColumnEstimate* c = ColumnOf(in, *e.children[0]);
    const double non_null = c != nullptr ? 1.0 - c->null_fraction : 1.0;
    const Value* pattern = e.children.size() > 1 ? AsConstant(*e.children[1]) : nullptr;
    if (pattern == nullptr || pattern->type().id() != TypeId::Varchar) {
        return non_null * 0.2;
    }
    const std::string& p = pattern->GetVarchar();
    if (p.find_first_of("%_") == std::string::npos) {
        return c != nullptr ? EqualitySelectivity(*c, pattern) : kDefaultEquality;
    }
    const bool prefix = p.find_first_of("%_") == p.size() - 1 && p.back() == '%';
    return non_null * (prefix ? 0.1 : 0.2);
}

double InListSelectivity(const BoundExpr& e, const Estimate& in) {
    const ColumnEstimate* c = ColumnOf(in, *e.children[0]);
    const size_t items = e.children.size() - 1;
    double hit;
    if (c != nullptr) {
        hit = 0;
        for (size_t i = 1; i < e.children.size(); i++) {
            const Value* k = AsConstant(*e.children[i]);
            hit += k != nullptr ? EqualitySelectivity(*c, k)
                                : (1.0 - c->null_fraction) / std::max(1.0, c->distinct);
        }
        hit = std::min(hit, 1.0 - c->null_fraction);
        return e.flag ? Clamp01(1.0 - c->null_fraction - hit) : hit;
    }
    hit = std::min(0.5, kDefaultEquality * static_cast<double>(items));
    return e.flag ? 1.0 - hit : hit;
}

} // namespace

std::optional<double> NumericValue(const Value& v) {
    if (v.IsNull()) {
        return std::nullopt;
    }
    switch (v.type().id()) {
    case TypeId::Integer:
        return static_cast<double>(v.GetInteger());
    case TypeId::BigInt:
        return static_cast<double>(v.GetBigInt());
    case TypeId::Date:
        return static_cast<double>(v.GetDate().days);
    case TypeId::Boolean:
        return v.GetBoolean() ? 1.0 : 0.0;
    case TypeId::Double: {
        const double d = v.GetDouble();
        return std::isnan(d) ? std::nullopt : std::optional<double>(d);
    }
    default:
        return std::nullopt;
    }
}

const BoundExpr* AsColumnRef(const BoundExpr& e) {
    const BoundExpr* x = &e;
    while (x->kind == BoundKind::Cast) {
        x = x->children[0].get();
    }
    return x->kind == BoundKind::ColumnRef ? x : nullptr;
}

double Selectivity(const BoundExpr& e, const Estimate& in) {
    switch (e.kind) {
    case BoundKind::Constant:
        if (!e.value.has_value() || e.value->IsNull()) {
            return 0;
        }
        return e.value->type().id() == TypeId::Boolean ? (e.value->GetBoolean() ? 1.0 : 0.0)
                                                       : kDefaultOther;
    case BoundKind::Operator: {
        if (e.op == OperatorKind::And) {
            return Selectivity(*e.children[0], in) * Selectivity(*e.children[1], in);
        }
        if (e.op == OperatorKind::Or) {
            const double a = Selectivity(*e.children[0], in), b = Selectivity(*e.children[1], in);
            return Clamp01(a + b - a * b);
        }
        if (e.op == OperatorKind::Not) {
            return Clamp01(1.0 - Selectivity(*e.children[0], in));
        }
        if (const auto cmp = ComparisonOf(e.op)) {
            return Clamp01(ComparisonSelectivity(e, *cmp, in));
        }
        return kDefaultOther;
    }
    case BoundKind::IsNull: {
        const ColumnEstimate* c = ColumnOf(in, *e.children[0]);
        const double nulls = c != nullptr ? c->null_fraction : kDefaultNullFraction;
        return e.flag ? 1.0 - nulls : nulls;
    }
    case BoundKind::InList:
        return Clamp01(InListSelectivity(e, in));
    case BoundKind::Function:
        return e.function == FunctionId::Like ? Clamp01(LikeSelectivity(e, in)) : kDefaultOther;
    default:
        return kDefaultOther;
    }
}

namespace {

// How much of a numeric column's range a predicate leaves, to scale its distinct count.
void Narrow(ColumnEstimate& c, Cmp op, const Value& k) {
    c.null_fraction = 0;
    if (op == Cmp::Eq) {
        c.distinct = 1;
        if (k.type().id() == (c.min ? c.min->type().id() : k.type().id())) {
            c.min = k;
            c.max = k;
        }
        return;
    }
    if (!c.min || !c.max || k.type().id() != c.min->type().id()) {
        return;
    }
    const auto kn = NumericValue(k), lo = NumericValue(*c.min), hi = NumericValue(*c.max);
    if (!kn || !lo || !hi || *hi <= *lo) {
        return;
    }
    double new_lo = *lo, new_hi = *hi;
    if (op == Cmp::Lt || op == Cmp::Le) {
        new_hi = std::min(new_hi, *kn);
        if (new_hi < *hi) {
            c.max = k;
        }
    } else if (op == Cmp::Gt || op == Cmp::Ge) {
        new_lo = std::max(new_lo, *kn);
        if (new_lo > *lo) {
            c.min = k;
        }
    } else {
        return;
    }
    c.distinct = std::max(1.0, c.distinct * Clamp01((new_hi - new_lo) / (*hi - *lo)));
}

} // namespace

Estimate ApplyFilter(const Estimate& input, const BoundExpr& predicate) {
    Estimate out;
    out.rows = input.rows * Clamp01(Selectivity(predicate, input));
    out.columns = input.columns;
    std::vector<BoundExprPtr> parts;
    SplitConjuncts(predicate.Clone(), parts);
    for (const auto& p : parts) {
        if (p->kind == BoundKind::Operator) {
            if (const auto cmp = ComparisonOf(p->op)) {
                const BoundExpr *a = p->children[0].get(), *b = p->children[1].get();
                const Value* kb = AsConstant(*b);
                const Value* ka = AsConstant(*a);
                const BoundExpr* col = kb != nullptr ? AsColumnRef(*a) : AsColumnRef(*b);
                if (col != nullptr && col->ordinal < out.columns.size() && (kb || ka) &&
                    *cmp != Cmp::Ne) {
                    Narrow(out.columns[col->ordinal], kb != nullptr ? *cmp : Flip(*cmp),
                           kb != nullptr ? *kb : *ka);
                }
            }
        } else if (p->kind == BoundKind::IsNull && p->flag) {
            if (const BoundExpr* col = AsColumnRef(*p->children[0]);
                col != nullptr && col->ordinal < out.columns.size()) {
                out.columns[col->ordinal].null_fraction = 0;
            }
        } else if (p->kind == BoundKind::InList && !p->flag) {
            if (const BoundExpr* col = AsColumnRef(*p->children[0]);
                col != nullptr && col->ordinal < out.columns.size()) {
                ColumnEstimate& c = out.columns[col->ordinal];
                c.distinct = std::min(c.distinct, static_cast<double>(p->children.size() - 1));
                c.null_fraction = 0;
            }
        }
    }
    const double room = std::max(1.0, out.rows);
    for (ColumnEstimate& c : out.columns) {
        c.distinct = std::max(1.0, std::min(c.distinct, room));
    }
    return out;
}

namespace {

struct KeyPair {
    idx_t left;  // ordinal in the left input
    idx_t right; // ordinal in the right input
};

// The equality conjuncts `left column = right column` of a join condition (over left ++ right),
// and what is left of it.
void SplitJoinCondition(const BoundExpr* condition, idx_t left_width, std::vector<KeyPair>& keys,
                        std::vector<BoundExprPtr>& rest) {
    if (condition == nullptr) {
        return;
    }
    std::vector<BoundExprPtr> parts;
    SplitConjuncts(condition->Clone(), parts);
    for (auto& p : parts) {
        if (p->kind == BoundKind::Operator && p->op == OperatorKind::Eq) {
            const BoundExpr* a = AsColumnRef(*p->children[0]);
            const BoundExpr* b = AsColumnRef(*p->children[1]);
            if (a != nullptr && b != nullptr) {
                if (a->ordinal < left_width && b->ordinal >= left_width) {
                    keys.push_back({a->ordinal, b->ordinal - left_width});
                    continue;
                }
                if (b->ordinal < left_width && a->ordinal >= left_width) {
                    keys.push_back({b->ordinal, a->ordinal - left_width});
                    continue;
                }
            }
        }
        rest.push_back(std::move(p));
    }
}

Estimate Concatenate(const Estimate& left, const Estimate& right, double rows) {
    Estimate out;
    out.rows = rows;
    out.columns = left.columns;
    out.columns.insert(out.columns.end(), right.columns.begin(), right.columns.end());
    return out;
}

void CapDistinct(Estimate& e) {
    const double room = std::max(1.0, e.rows);
    for (ColumnEstimate& c : e.columns) {
        c.distinct = std::max(1.0, std::min(c.distinct, room));
    }
}

} // namespace

Estimate EstimateJoin(JoinType type, const Estimate& left, const Estimate& right,
                      const BoundExpr* condition) {
    const idx_t lw = left.columns.size();
    std::vector<KeyPair> keys;
    std::vector<BoundExprPtr> rest;
    SplitJoinCondition(condition, lw, keys, rest);

    // fraction of the pairs of rows that agree on every key (containment: a key's values on the
    // smaller side are among the larger side's)
    double key_selectivity = 1.0;
    if (!keys.empty()) {
        double combinations = 1.0;
        double non_null = 1.0;
        for (const KeyPair& k : keys) {
            const ColumnEstimate &l = left.columns[k.left], &r = right.columns[k.right];
            combinations *= std::max({1.0, l.distinct, r.distinct});
            non_null *= (1.0 - l.null_fraction) * (1.0 - r.null_fraction);
        }
        // a composite key cannot have more combinations than the larger input has rows
        combinations = std::min(combinations, std::max({1.0, left.rows, right.rows}));
        key_selectivity = non_null / combinations;
    }
    const Estimate both = Concatenate(left, right, left.rows * right.rows);
    double other_selectivity = 1.0;
    for (const auto& p : rest) {
        other_selectivity *= Clamp01(Selectivity(*p, both));
    }
    const double inner_rows = left.rows * right.rows * key_selectivity * other_selectivity;

    if (type == JoinType::Semi || type == JoinType::Anti || type == JoinType::AntiNullAware) {
        double match = 1.0; // fraction of the left rows with at least one partner
        double partners_per_key = std::max(1.0, right.rows);
        if (!keys.empty()) {
            match = 1.0;
            for (const KeyPair& k : keys) {
                const ColumnEstimate &l = left.columns[k.left], &r = right.columns[k.right];
                match = std::min(match, Clamp01(r.distinct / std::max(1.0, l.distinct)) *
                                            (1.0 - l.null_fraction));
                partners_per_key = std::max(1.0, right.rows / std::max(1.0, r.distinct));
            }
        } else if (right.rows < 0.5) {
            match = 0;
        }
        if (!rest.empty() && match > 0) {
            // with several candidate partners per left row, one of them is likely to satisfy the
            // rest
            match *= 1.0 - std::pow(1.0 - other_selectivity, std::min(partners_per_key, 1e6));
        }
        Estimate out;
        out.columns = left.columns;
        out.rows = left.rows * (type == JoinType::Semi ? match : 1.0 - match);
        CapDistinct(out);
        return out;
    }

    double rows = inner_rows;
    if (type == JoinType::Left) {
        rows = std::max(inner_rows, left.rows);
    } else if (type == JoinType::Right) {
        rows = std::max(inner_rows, right.rows);
    }
    Estimate out = Concatenate(left, right, rows);
    for (const KeyPair& k : keys) { // joined keys: the smaller domain, never NULL in an inner join
        ColumnEstimate& l = out.columns[k.left];
        ColumnEstimate& r = out.columns[lw + k.right];
        l.distinct = r.distinct = std::min(l.distinct, r.distinct);
        if (type == JoinType::Inner) {
            l.null_fraction = r.null_fraction = 0;
        }
    }
    if (type == JoinType::Left && left.rows > 0) { // unmatched left rows pad the right columns
        const double unmatched = Clamp01(1.0 - std::min(inner_rows, left.rows) / left.rows);
        for (idx_t c = lw; c < out.columns.size(); c++) {
            out.columns[c].null_fraction = std::max(out.columns[c].null_fraction, unmatched);
        }
    }
    CapDistinct(out);
    return out;
}

// ---------------------------------------------------------------------------------- operators

namespace {

// How many distinct values an expression over `input` takes: those of the single column it is,
// of the columns it combines (capped by the rows), 1 for a constant.
double ExpressionDistinct(const BoundExpr& e, const Estimate& input) {
    if (const BoundExpr* c = AsColumnRef(e)) {
        return c->ordinal < input.columns.size() ? input.columns[c->ordinal].distinct
                                                 : std::max(1.0, input.rows);
    }
    std::set<idx_t> refs;
    CollectColumnRefs(e, refs);
    if (refs.empty()) {
        return 1;
    }
    double product = 1;
    for (const idx_t r : refs) {
        product *= r < input.columns.size() ? input.columns[r].distinct : std::max(1.0, input.rows);
        product = std::min(product, std::max(1.0, input.rows));
    }
    return std::max(1.0, product);
}

ColumnEstimate ProjectedColumn(const BoundExpr& e, const Estimate& input) {
    if (const BoundExpr* c = AsColumnRef(e); c != nullptr && c->ordinal < input.columns.size()) {
        return input.columns[c->ordinal];
    }
    ColumnEstimate out;
    if (const Value* k = AsConstant(e)) {
        out.distinct = 1;
        out.null_fraction = k->IsNull() ? 1.0 : 0.0;
        if (!k->IsNull()) {
            out.min = out.max = *k;
        }
        return out;
    }
    out.distinct = ExpressionDistinct(e, input);
    std::set<idx_t> refs;
    CollectColumnRefs(e, refs);
    for (const idx_t r : refs) {
        if (r < input.columns.size()) {
            out.null_fraction = std::max(out.null_fraction, input.columns[r].null_fraction);
        }
    }
    return out;
}

} // namespace

const Estimate& CardinalityEstimator::Of(const LogicalOperator& op) {
    const auto it = memo_.find(&op);
    if (it != memo_.end()) {
        return it->second;
    }
    Estimate e = Compute(op);
    return memo_.emplace(&op, std::move(e)).first->second;
}

Estimate CardinalityEstimator::Compute(const LogicalOperator& op) {
    switch (op.kind) {
    case LogicalKind::Get: {
        const auto& get = static_cast<const LogicalGet&>(op);
        const std::shared_ptr<const TableStatistics> stats = get.table->Statistics();
        Estimate e;
        e.rows = static_cast<double>(stats->row_count);
        for (const idx_t column : get.column_ids) {
            const ColumnStatistics& s = stats->columns.at(column);
            ColumnEstimate c;
            c.distinct = std::max(1.0, s.distinct);
            c.null_fraction = stats->NullFraction(column);
            c.min = s.min;
            c.max = s.max;
            e.columns.push_back(std::move(c));
        }
        return e;
    }
    case LogicalKind::Filter:
        return ApplyFilter(Of(*op.children[0]), *static_cast<const LogicalFilter&>(op).predicate);
    case LogicalKind::Projection: {
        const Estimate& in = Of(*op.children[0]);
        Estimate e;
        e.rows = in.rows;
        for (const auto& expr : static_cast<const LogicalProjection&>(op).exprs) {
            e.columns.push_back(ProjectedColumn(*expr, in));
        }
        CapDistinct(e);
        return e;
    }
    case LogicalKind::Aggregate: {
        const auto& agg = static_cast<const LogicalAggregate&>(op);
        const Estimate& in = Of(*op.children[0]);
        Estimate e;
        double groups = 1;
        for (const auto& g : agg.groups) {
            groups *= ExpressionDistinct(*g, in);
            groups = std::min(groups, std::max(1.0, in.rows));
        }
        e.rows = agg.groups.empty() ? 1.0 : (in.rows <= 0 ? 0.0 : std::max(1.0, groups));
        for (const auto& g : agg.groups) {
            e.columns.push_back(ProjectedColumn(*g, in));
        }
        for (size_t a = 0; a < agg.aggregates.size(); a++) {
            ColumnEstimate c;
            c.distinct = std::max(1.0, e.rows);
            e.columns.push_back(c);
        }
        CapDistinct(e);
        return e;
    }
    case LogicalKind::Distinct: {
        const Estimate& in = Of(*op.children[0]);
        Estimate e = in;
        double groups = 1;
        for (const ColumnEstimate& c : in.columns) {
            groups = std::min(groups * c.distinct, std::max(1.0, in.rows));
        }
        e.rows = in.rows <= 0 ? 0.0 : std::max(1.0, groups);
        CapDistinct(e);
        return e;
    }
    case LogicalKind::Join: {
        const auto& j = static_cast<const LogicalJoin&>(op);
        return EstimateJoin(j.join_type, Of(*op.children[0]), Of(*op.children[1]),
                            j.condition.get());
    }
    case LogicalKind::Limit: {
        const auto& lim = static_cast<const LogicalLimit&>(op);
        Estimate e = Of(*op.children[0]);
        e.rows = std::max(0.0, e.rows - static_cast<double>(lim.offset));
        if (lim.limit) {
            e.rows = std::min(e.rows, static_cast<double>(*lim.limit));
        }
        CapDistinct(e);
        return e;
    }
    case LogicalKind::ScalarGuard: {
        Estimate e = Of(*op.children[0]);
        e.rows = 1;
        CapDistinct(e);
        return e;
    }
    case LogicalKind::Values: {
        const auto& v = static_cast<const LogicalValues&>(op);
        Estimate e;
        e.rows = static_cast<double>(v.rows.size());
        for (idx_t c = 0; c < op.ColumnCount(); c++) {
            ColumnEstimate col;
            col.distinct = std::max(1.0, e.rows);
            e.columns.push_back(col);
        }
        return e;
    }
    case LogicalKind::Order:
    case LogicalKind::Insert:
    case LogicalKind::Copy:
    case LogicalKind::Explain:
        return op.children.empty() ? Estimate{} : Of(*op.children[0]);
    default:
        return Estimate{};
    }
}

} // namespace cdb
