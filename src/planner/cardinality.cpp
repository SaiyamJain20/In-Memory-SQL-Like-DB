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

namespace {

void FlattenAnd(const BoundExpr& e, std::vector<const BoundExpr*>& out) {
    if (e.kind == BoundKind::Operator && e.op == OperatorKind::And) {
        FlattenAnd(*e.children[0], out);
        FlattenAnd(*e.children[1], out);
    } else {
        out.push_back(&e);
    }
}

// A conjunct that bounds one column from one side by a constant: `column > 5`, `date <= x`, ...
struct OneSidedRange {
    idx_t column = 0;
    Cmp op = Cmp::Lt;
    const Value* constant = nullptr;
    bool lower() const { return op == Cmp::Gt || op == Cmp::Ge; }
};

std::optional<OneSidedRange> AsOneSidedRange(const BoundExpr& e, const Estimate& in) {
    if (e.kind != BoundKind::Operator) {
        return std::nullopt;
    }
    const auto cmp = ComparisonOf(e.op);
    if (!cmp || *cmp == Cmp::Eq || *cmp == Cmp::Ne) {
        return std::nullopt;
    }
    const BoundExpr* column = AsColumnRef(*e.children[0]);
    const Value* k = AsConstant(*e.children[1]);
    Cmp op = *cmp;
    if (column == nullptr || k == nullptr) {
        column = AsColumnRef(*e.children[1]);
        k = AsConstant(*e.children[0]);
        op = Flip(op);
    }
    if (column == nullptr || k == nullptr || k->IsNull() || column->ordinal >= in.columns.size() ||
        !NumericValue(*k)) {
        return std::nullopt;
    }
    return OneSidedRange{column->ordinal, op, k};
}

} // namespace

// The conjuncts of an AND are taken as independent, except that a lower and an upper bound on the
// same column describe an interval: `d >= 1994-01-01 AND d < 1995-01-01` keeps the year between
// them (P(d < b) - P(d < a)), not the product of two shares that both include most of the range.
double AndSelectivity(const BoundExpr& e, const Estimate& in) {
    std::vector<const BoundExpr*> parts;
    FlattenAnd(e, parts);
    std::vector<bool> used(parts.size(), false);
    double selectivity = 1.0;
    for (size_t i = 0; i < parts.size(); i++) {
        const auto lower = AsOneSidedRange(*parts[i], in);
        if (used[i] || !lower || !lower->lower()) {
            continue;
        }
        for (size_t j = 0; j < parts.size(); j++) {
            const auto upper = AsOneSidedRange(*parts[j], in);
            if (j == i || used[j] || !upper || upper->lower() || upper->column != lower->column) {
                continue;
            }
            const ColumnEstimate& c = in.columns[lower->column];
            const double non_null = 1.0 - c.null_fraction;
            // P(low bound) + P(high bound) - P(either) = P(both), and P(either) = non-NULL rows
            selectivity *= Clamp01(RangeSelectivity(c, lower->op, *lower->constant) +
                                   RangeSelectivity(c, upper->op, *upper->constant) - non_null);
            used[i] = used[j] = true;
            break;
        }
    }
    for (size_t i = 0; i < parts.size(); i++) {
        if (!used[i]) {
            selectivity *= Selectivity(*parts[i], in);
        }
    }
    return selectivity;
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
            return AndSelectivity(e, in);
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
                // the share of the left keys' domain that the right keys cover: not of the distinct
                // values the left rows hold, which every predicate that thins them out reduces
                match = std::min(match, Clamp01(r.distinct / std::max(1.0, l.Domain())) *
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

// The range an arithmetic expression over `input`'s columns can take (interval arithmetic on the
// columns' bounds), or nothing if some leaf has no bounds.
std::optional<std::pair<double, double>> NumericRange(const BoundExpr& e, const Estimate& input) {
    if (const BoundExpr* c = AsColumnRef(e)) {
        if (c->ordinal >= input.columns.size() || !input.columns[c->ordinal].min ||
            !input.columns[c->ordinal].max) {
            return std::nullopt;
        }
        const auto lo = NumericValue(*input.columns[c->ordinal].min);
        const auto hi = NumericValue(*input.columns[c->ordinal].max);
        if (!lo || !hi) {
            return std::nullopt;
        }
        return std::make_pair(*lo, *hi);
    }
    if (const Value* k = AsConstant(e)) {
        const auto v = NumericValue(*k);
        return v ? std::optional(std::make_pair(*v, *v)) : std::nullopt;
    }
    if (e.kind == BoundKind::Cast) {
        return NumericRange(*e.children[0], input);
    }
    if (e.kind != BoundKind::Operator || e.children.empty()) {
        return std::nullopt;
    }
    if (e.op == OperatorKind::Negate) {
        const auto r = NumericRange(*e.children[0], input);
        return r ? std::optional(std::make_pair(-r->second, -r->first)) : std::nullopt;
    }
    if (e.children.size() != 2 ||
        (e.op != OperatorKind::Add && e.op != OperatorKind::Sub && e.op != OperatorKind::Mul)) {
        return std::nullopt;
    }
    const auto a = NumericRange(*e.children[0], input), b = NumericRange(*e.children[1], input);
    if (!a || !b) {
        return std::nullopt;
    }
    if (e.op == OperatorKind::Add) {
        return std::make_pair(a->first + b->first, a->second + b->second);
    }
    if (e.op == OperatorKind::Sub) {
        return std::make_pair(a->first - b->second, a->second - b->first);
    }
    const double products[] = {a->first * b->first, a->first * b->second, a->second * b->first,
                               a->second * b->second};
    return std::make_pair(*std::min_element(products, products + 4),
                          *std::max_element(products, products + 4));
}

// How many distinct values an expression over `input` takes. A column has its own count; a constant
// one; what an expression of a column can take is bounded by what the function can return (a year
// of a date column: the years between its bounds, not one per date; a comparison: two values;
// a CASE of constants: those constants), else by the product of the distinct counts of the columns
// it combines (capped by the rows).
double ExpressionDistinct(const BoundExpr& e, const Estimate& input) {
    const double rows = std::max(1.0, input.rows);
    if (const BoundExpr* c = AsColumnRef(e)) {
        return c->ordinal < input.columns.size() ? input.columns[c->ordinal].distinct : rows;
    }
    std::set<idx_t> refs;
    CollectColumnRefs(e, refs);
    if (refs.empty()) {
        return 1;
    }
    const auto argument = [&](size_t i) { return ExpressionDistinct(*e.children.at(i), input); };
    switch (e.kind) {
    case BoundKind::Cast:
        return argument(0);
    case BoundKind::IsNull:
    case BoundKind::InList:
        return 2;
    case BoundKind::Operator:
        switch (e.op) {
        case OperatorKind::Eq:
        case OperatorKind::Ne:
        case OperatorKind::Lt:
        case OperatorKind::Le:
        case OperatorKind::Gt:
        case OperatorKind::Ge:
        case OperatorKind::And:
        case OperatorKind::Or:
        case OperatorKind::Not:
            return 3; // TRUE, FALSE, NULL
        default:
            break;
        }
        break;
    case BoundKind::Function: {
        const auto bounded = [&](double most) {
            return std::max(1.0, std::min(argument(0), most));
        };
        switch (e.function) {
        case FunctionId::Like:
            return 3;
        case FunctionId::Month:
            return bounded(12);
        case FunctionId::Day:
            return bounded(31);
        case FunctionId::Quarter:
            return bounded(4);
        case FunctionId::DayOfWeek:
            return bounded(7);
        case FunctionId::DayOfYear:
            return bounded(366);
        case FunctionId::Year: {
            const BoundExpr* column = AsColumnRef(*e.children.at(0));
            if (column != nullptr && column->ordinal < input.columns.size()) {
                const ColumnEstimate& c = input.columns[column->ordinal];
                const auto lo = c.min ? NumericValue(*c.min) : std::nullopt;
                const auto hi = c.max ? NumericValue(*c.max) : std::nullopt;
                if (lo && hi) { // days since 1970: the years the bounds span
                    return bounded(std::floor((*hi - *lo) / 365.2425) + 2);
                }
            }
            return argument(0);
        }
        case FunctionId::Upper:
        case FunctionId::Lower:
        case FunctionId::Substring:
        case FunctionId::Length:
        case FunctionId::Abs:
        case FunctionId::Floor:
        case FunctionId::Ceil:
        case FunctionId::Round:
            return argument(0); // never more values than the argument has
        default:
            break;
        }
        break;
    }
    case BoundKind::Case: {
        // children = [when1, then1, ..., whenN, thenN, else]: with constant results the CASE can
        // return no more values than there are distinct constants
        std::vector<const BoundExpr*> results;
        for (size_t i = 1; i + 1 < e.children.size(); i += 2) {
            results.push_back(e.children[i].get());
        }
        results.push_back(e.children.back().get());
        std::vector<Value> constants;
        bool all_constant = true;
        for (const BoundExpr* r : results) {
            const Value* k = AsConstant(*r);
            if (k == nullptr) {
                all_constant = false;
                break;
            }
            const bool seen = std::any_of(constants.begin(), constants.end(), [&](const Value& v) {
                return v.IsNull() == k->IsNull() && (v.IsNull() || Value::Compare(v, *k) == 0);
            });
            if (!seen) {
                constants.push_back(*k);
            }
        }
        if (all_constant) {
            return static_cast<double>(constants.size());
        }
        break;
    }
    default:
        break;
    }
    double product = 1;
    for (const idx_t r : refs) {
        product *= r < input.columns.size() ? input.columns[r].distinct : rows;
        product = std::min(product, rows);
    }
    // an integer-valued expression takes no more values than its range holds integers
    const TypeId id = e.type.id();
    if (id == TypeId::Integer || id == TypeId::BigInt || id == TypeId::Date) {
        if (const auto range = NumericRange(e, input)) {
            product = std::min(product, std::floor(range->second - range->first) + 1);
        }
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
            c.domain = c.distinct;
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
