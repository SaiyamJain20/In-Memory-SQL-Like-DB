#include "planner/logical_plan.h"

namespace cdb {

namespace {

std::string JoinStrings(const std::vector<std::string>& parts, const char* sep = ", ") {
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i > 0)
            out += sep;
        out += parts[i];
    }
    return out;
}

} // namespace

std::string LogicalOperator::ToString(int indent) const {
    std::string out = std::string(static_cast<size_t>(indent) * 2, ' ') + Describe() + "\n";
    for (const auto& c : children)
        out += c->ToString(indent + 1);
    return out;
}

std::string LogicalGet::Describe() const {
    std::string cols;
    for (idx_t i = 0; i < column_ids.size(); i++) {
        if (i > 0)
            cols += ", ";
        cols += table->schema()[column_ids[i]].name;
    }
    std::string out = "SCAN " + table->name() + " [" + cols + "]";
    for (const TableFilter& f : filters) {
        out += " prune(" + table->schema()[f.column_index].name + " " + CompareOpName(f.op) + " " +
               f.constant.ToString() + ")";
    }
    return out;
}

std::string LogicalFilter::Describe() const {
    return "FILTER " + predicate->ToString();
}

std::string LogicalProjection::Describe() const {
    std::vector<std::string> parts;
    for (idx_t i = 0; i < exprs.size(); i++) {
        std::string e = exprs[i]->ToString();
        parts.push_back(e == names[i] ? e : e + " AS " + names[i]);
    }
    return "PROJECT [" + JoinStrings(parts) + "]";
}

std::string LogicalAggregate::Describe() const {
    std::vector<std::string> g, a;
    for (const auto& e : groups)
        g.push_back(e->ToString());
    for (const auto& e : aggregates)
        a.push_back(e->ToString());
    return "AGGREGATE groups=[" + JoinStrings(g) + "] aggregates=[" + JoinStrings(a) + "]";
}

std::string LogicalJoin::Describe() const {
    static const char* const kNames[] = {"INNER", "LEFT", "RIGHT", "FULL", "CROSS"};
    std::string out = std::string("JOIN ") + kNames[static_cast<int>(join_type)];
    if (condition)
        out += " ON " + condition->ToString();
    return out;
}

std::string LogicalOrder::Describe() const {
    std::vector<std::string> parts;
    for (const SortKey& k : keys) {
        parts.push_back(k.expr->ToString() + (k.descending ? " DESC" : " ASC") +
                        (k.nulls_first ? " NULLS FIRST" : " NULLS LAST"));
    }
    return "ORDER BY " + JoinStrings(parts);
}

std::string LogicalLimit::Describe() const {
    std::string out = "LIMIT " + (limit ? std::to_string(*limit) : std::string("ALL"));
    if (offset != 0)
        out += " OFFSET " + std::to_string(offset);
    return out;
}

std::string LogicalDistinct::Describe() const {
    return "DISTINCT";
}

std::string LogicalValues::Describe() const {
    return "VALUES (" + std::to_string(rows.size()) + " row" + (rows.size() == 1 ? "" : "s") + ")";
}

std::string LogicalCreateTable::Describe() const {
    std::vector<std::string> cols;
    for (const auto& c : schema) {
        cols.push_back(c.name + " " + c.type.ToString() + (c.not_null ? " NOT NULL" : ""));
    }
    return std::string("CREATE TABLE ") + (if_not_exists ? "IF NOT EXISTS " : "") + table_name +
           " (" + JoinStrings(cols) + ")";
}

std::string LogicalDropTable::Describe() const {
    return std::string("DROP TABLE ") + (if_exists ? "IF EXISTS " : "") + table_name;
}

std::string LogicalInsert::Describe() const {
    return "INSERT INTO " + table->name();
}

std::string LogicalCopy::Describe() const {
    return "COPY " + table->name() + " FROM '" + path + "' (delimiter '" + delimiter +
           "', header " + (header ? "true" : "false") + ")";
}

std::string LogicalCheckpoint::Describe() const {
    return "CHECKPOINT";
}

std::string LogicalExplain::Describe() const {
    return analyze ? "EXPLAIN ANALYZE" : "EXPLAIN";
}

} // namespace cdb
