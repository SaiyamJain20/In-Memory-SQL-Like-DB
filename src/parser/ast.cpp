#include "parser/ast.h"

#include "parser/token.h"

#include <cctype>
#include <sstream>

namespace cdb {

std::string QuoteIdentifier(const std::string& name) {
    bool plain =
        !name.empty() && (std::islower(static_cast<unsigned char>(name[0])) || name[0] == '_');
    for (char ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if (!(std::islower(c) || std::isdigit(c) || c == '_' || c == '$')) {
            plain = false;
        }
    }
    if (plain) {
        // Reserved words must be quoted to survive a round trip.
        std::string upper = name;
        for (char& ch : upper)
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        for (const Token& t : Tokenize(upper)) {
            if (t.type == TokenType::Keyword) {
                plain = false;
            }
            break;
        }
    }
    if (plain) {
        return name;
    }
    std::string out = "\"";
    for (char ch : name) {
        if (ch == '"')
            out += '"';
        out += ch;
    }
    out += '"';
    return out;
}

std::string QuoteString(const std::string& text) {
    std::string out = "'";
    for (char ch : text) {
        if (ch == '\'')
            out += '\'';
        out += ch;
    }
    out += '\'';
    return out;
}

const char* BinaryOpText(BinaryOp op) noexcept {
    switch (op) {
    case BinaryOp::Add:
        return "+";
    case BinaryOp::Sub:
        return "-";
    case BinaryOp::Mul:
        return "*";
    case BinaryOp::Div:
        return "/";
    case BinaryOp::Mod:
        return "%";
    case BinaryOp::Concat:
        return "||";
    case BinaryOp::Eq:
        return "=";
    case BinaryOp::Ne:
        return "<>";
    case BinaryOp::Lt:
        return "<";
    case BinaryOp::Le:
        return "<=";
    case BinaryOp::Gt:
        return ">";
    case BinaryOp::Ge:
        return ">=";
    case BinaryOp::And:
        return "AND";
    case BinaryOp::Or:
        return "OR";
    }
    return "?";
}

// ------------------------------------------------------------------------------ expressions

std::string ColumnRefExpr::ToString() const {
    return table.empty() ? QuoteIdentifier(column)
                         : QuoteIdentifier(table) + "." + QuoteIdentifier(column);
}

std::string StarExpr::ToString() const {
    return table.empty() ? "*" : QuoteIdentifier(table) + ".*";
}

std::string ConstantExpr::ToString() const {
    switch (literal) {
    case LiteralKind::Null:
        return "NULL";
    case LiteralKind::Boolean:
        return value.GetBoolean() ? "TRUE" : "FALSE";
    case LiteralKind::Integer:
    case LiteralKind::Double:
        return value.ToString();
    case LiteralKind::String:
        return QuoteString(value.GetVarchar());
    case LiteralKind::Date:
        return "DATE " + QuoteString(value.ToString());
    }
    return "?";
}

std::string IntervalExpr::ToString() const {
    return "INTERVAL " + QuoteString(std::to_string(amount)) + " " + unit;
}

std::string UnaryExpr::ToString() const {
    return op == UnaryOp::Negate ? "(-" + child->ToString() + ")"
                                 : "(NOT " + child->ToString() + ")";
}

std::string BinaryExpr::ToString() const {
    return "(" + left->ToString() + " " + BinaryOpText(op) + " " + right->ToString() + ")";
}

std::string IsNullExpr::ToString() const {
    return "(" + child->ToString() + (negated ? " IS NOT NULL)" : " IS NULL)");
}

std::string BetweenExpr::ToString() const {
    return "(" + child->ToString() + (negated ? " NOT BETWEEN " : " BETWEEN ") + lower->ToString() +
           " AND " + upper->ToString() + ")";
}

std::string InListExpr::ToString() const {
    std::string out = "(" + child->ToString() + (negated ? " NOT IN (" : " IN (");
    for (size_t i = 0; i < list.size(); i++) {
        if (i > 0)
            out += ", ";
        out += list[i]->ToString();
    }
    return out + "))";
}

std::string InSubqueryExpr::ToString() const {
    return "(" + child->ToString() + (negated ? " NOT IN (" : " IN (") + subquery->ToString() +
           "))";
}

std::string LikeExpr::ToString() const {
    return "(" + child->ToString() + (negated ? " NOT LIKE " : " LIKE ") + pattern->ToString() +
           ")";
}

std::string CaseExpr::ToString() const {
    std::string out = "CASE";
    if (operand)
        out += " " + operand->ToString();
    for (const When& w : whens) {
        out += " WHEN " + w.condition->ToString() + " THEN " + w.result->ToString();
    }
    if (else_result)
        out += " ELSE " + else_result->ToString();
    return out + " END";
}

std::string CastExpr::ToString() const {
    return "CAST(" + child->ToString() + " AS " + target.ToString() + ")";
}

std::string FunctionExpr::ToString() const {
    // left()/right() are reserved (join keywords) but the parser accepts them bare as functions.
    const bool bare = name == "left" || name == "right";
    std::string out = (bare ? name : QuoteIdentifier(name)) + "(";
    if (star) {
        out += "*";
    } else {
        if (distinct)
            out += "DISTINCT ";
        for (size_t i = 0; i < args.size(); i++) {
            if (i > 0)
                out += ", ";
            out += args[i]->ToString();
        }
    }
    return out + ")";
}

std::string ExtractExpr::ToString() const {
    return "EXTRACT(" + field + " FROM " + child->ToString() + ")";
}

std::string ExistsExpr::ToString() const {
    return "EXISTS (" + subquery->ToString() + ")";
}

std::string ScalarSubqueryExpr::ToString() const {
    return "(" + subquery->ToString() + ")";
}

// ------------------------------------------------------------------------------ table refs

namespace {
std::string ColumnAliasList(const std::vector<std::string>& names) {
    if (names.empty())
        return "";
    std::string out = " (";
    for (size_t i = 0; i < names.size(); i++) {
        if (i > 0)
            out += ", ";
        out += QuoteIdentifier(names[i]);
    }
    return out + ")";
}
} // namespace

std::string BaseTableRef::ToString() const {
    return alias.empty() ? QuoteIdentifier(name)
                         : QuoteIdentifier(name) + " AS " + QuoteIdentifier(alias) +
                               ColumnAliasList(column_aliases);
}

std::string JoinRef::ToString() const {
    static const char* const kNames[] = {"INNER JOIN", "LEFT JOIN", "RIGHT JOIN", "FULL JOIN",
                                         "CROSS JOIN"};
    // A join on the right-hand side came from explicit parentheses: keep them.
    const std::string rhs =
        right->kind == TableRefKind::Join ? "(" + right->ToString() + ")" : right->ToString();
    std::string out = left->ToString() + " " + kNames[static_cast<int>(type)] + " " + rhs;
    if (condition)
        out += " ON " + condition->ToString();
    if (!using_columns.empty()) {
        out += " USING (";
        for (size_t i = 0; i < using_columns.size(); i++) {
            if (i > 0)
                out += ", ";
            out += QuoteIdentifier(using_columns[i]);
        }
        out += ")";
    }
    return out;
}

std::string SubqueryRef::ToString() const {
    // The parser accepts a derived table without an alias; print it back the same way.
    if (alias.empty())
        return "(" + select->ToString() + ")";
    return "(" + select->ToString() + ") AS " + QuoteIdentifier(alias) +
           ColumnAliasList(column_aliases);
}

// ------------------------------------------------------------------------------ statements

std::string SelectStatement::ToString() const {
    std::string out = "SELECT ";
    if (distinct)
        out += "DISTINCT ";
    for (size_t i = 0; i < items.size(); i++) {
        if (i > 0)
            out += ", ";
        out += items[i].expr->ToString();
        if (!items[i].alias.empty())
            out += " AS " + QuoteIdentifier(items[i].alias);
    }
    if (from)
        out += " FROM " + from->ToString();
    if (where)
        out += " WHERE " + where->ToString();
    if (!group_by.empty()) {
        out += " GROUP BY ";
        for (size_t i = 0; i < group_by.size(); i++) {
            if (i > 0)
                out += ", ";
            out += group_by[i]->ToString();
        }
    }
    if (having)
        out += " HAVING " + having->ToString();
    if (!order_by.empty()) {
        out += " ORDER BY ";
        for (size_t i = 0; i < order_by.size(); i++) {
            if (i > 0)
                out += ", ";
            out += order_by[i].expr->ToString();
            if (order_by[i].descending)
                out += " DESC";
            if (order_by[i].nulls == NullOrder::First)
                out += " NULLS FIRST";
            if (order_by[i].nulls == NullOrder::Last)
                out += " NULLS LAST";
        }
    }
    if (limit)
        out += " LIMIT " + limit->ToString();
    if (offset)
        out += " OFFSET " + offset->ToString();
    return out;
}

std::string CreateTableStatement::ToString() const {
    std::string out = "CREATE TABLE ";
    if (if_not_exists)
        out += "IF NOT EXISTS ";
    out += QuoteIdentifier(name) + " (";
    for (size_t i = 0; i < columns.size(); i++) {
        if (i > 0)
            out += ", ";
        out += QuoteIdentifier(columns[i].name) + " " + columns[i].type.ToString();
        if (columns[i].not_null)
            out += " NOT NULL";
    }
    return out + ")";
}

std::string DropTableStatement::ToString() const {
    return std::string("DROP TABLE ") + (if_exists ? "IF EXISTS " : "") + QuoteIdentifier(name);
}

std::string InsertStatement::ToString() const {
    std::string out = "INSERT INTO " + QuoteIdentifier(table);
    if (!columns.empty()) {
        out += " (";
        for (size_t i = 0; i < columns.size(); i++) {
            if (i > 0)
                out += ", ";
            out += QuoteIdentifier(columns[i]);
        }
        out += ")";
    }
    if (select) {
        return out + " " + select->ToString();
    }
    out += " VALUES ";
    for (size_t r = 0; r < rows.size(); r++) {
        if (r > 0)
            out += ", ";
        out += "(";
        for (size_t c = 0; c < rows[r].size(); c++) {
            if (c > 0)
                out += ", ";
            out += rows[r][c]->ToString();
        }
        out += ")";
    }
    return out;
}

std::string CopyStatement::ToString() const {
    return "COPY " + QuoteIdentifier(table) + " FROM " + QuoteString(path) + " (DELIMITER " +
           QuoteString(delimiter) + ", HEADER " + (header ? "TRUE" : "FALSE") + ")";
}

std::string ExplainStatement::ToString() const {
    return std::string("EXPLAIN ") + (analyze ? "ANALYZE " : "") + inner->ToString();
}

} // namespace cdb
