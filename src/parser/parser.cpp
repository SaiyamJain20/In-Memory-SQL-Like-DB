#include "parser/parser.h"

#include "parser/token.h"
#include "types/date.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace cdb {

namespace {

class Parser {
  public:
    explicit Parser(std::string_view sql) : tokens_(Tokenize(sql)) {}

    std::vector<StatementPtr> ParseAll() {
        std::vector<StatementPtr> out;
        while (true) {
            while (AcceptSymbol(";")) {
            }
            if (Peek().type == TokenType::End) {
                break;
            }
            out.push_back(ParseStatementBody());
            if (Peek().type != TokenType::End && !Peek().IsSymbol(";")) {
                Fail("expected end of statement");
            }
        }
        return out;
    }

    ExprPtr ParseStandaloneExpression() {
        ExprPtr e = ParseExpr();
        if (Peek().type != TokenType::End) {
            Fail("expected end of expression");
        }
        return e;
    }

  private:
    // ------------------------------------------------------------------ token helpers

    const Token& Peek(size_t ahead = 0) const {
        const size_t i = std::min(pos_ + ahead, tokens_.size() - 1);
        return tokens_[i];
    }
    const Token& Advance() {
        const Token& t = tokens_[pos_];
        if (pos_ + 1 < tokens_.size())
            pos_++;
        return t;
    }
    bool AcceptKeyword(Keyword k) {
        if (Peek().IsKeyword(k)) {
            Advance();
            return true;
        }
        return false;
    }
    bool AcceptSymbol(std::string_view s) {
        if (Peek().IsSymbol(s)) {
            Advance();
            return true;
        }
        return false;
    }
    // A contextual (non-reserved) word: an unquoted identifier with this exact lower-case text.
    bool IsWord(std::string_view w, size_t ahead = 0) const {
        return Peek(ahead).type == TokenType::Identifier && Peek(ahead).text == w;
    }
    bool AcceptWord(std::string_view w) {
        if (IsWord(w)) {
            Advance();
            return true;
        }
        return false;
    }

    [[noreturn]] void Fail(const std::string& expected) const { FailAt(Peek(), expected); }

    [[noreturn]] void FailAt(const Token& t, const std::string& expected) const {
        std::string msg =
            t.type == TokenType::End
                ? "syntax error at end of input"
                : "syntax error at or near \"" +
                      std::string(t.type == TokenType::String ? "'" + t.text + "'" : t.text) + "\"";
        if (!expected.empty())
            msg += " (" + expected + ")";
        throw Error(ErrorCode::Syntax, msg, t.pos);
    }

    [[noreturn]] void NotImplemented(const std::string& what, const Token& t) const {
        throw Error(ErrorCode::NotImplemented, what + " is not supported yet", t.pos);
    }

    void ExpectKeyword(Keyword k) {
        if (!AcceptKeyword(k))
            Fail(std::string("expected ") + KeywordName(k));
    }
    void ExpectSymbol(std::string_view s) {
        if (!AcceptSymbol(s))
            Fail("expected \"" + std::string(s) + "\"");
    }

    std::string ParseIdentifier(const char* what) {
        const Token& t = Peek();
        if (t.type == TokenType::Identifier || t.type == TokenType::QuotedIdentifier) {
            return Advance().text;
        }
        Fail(std::string("expected ") + what);
    }

    // Bounds recursion (nested parentheses, subqueries, NOT/unary chains).
    struct NestingGuard {
        Parser& p;
        explicit NestingGuard(Parser& parser) : p(parser) {
            if (++p.nesting_ > kMaxParseNesting) {
                throw Error(ErrorCode::Syntax, "expression is nested too deeply", p.Peek().pos);
            }
        }
        ~NestingGuard() { p.nesting_--; }
    };

    // ------------------------------------------------------------------ statements

    StatementPtr ParseStatementBody() {
        const Token& t = Peek();
        if (t.IsKeyword(Keyword::SELECT))
            return ParseSelect();
        if (t.IsKeyword(Keyword::CREATE))
            return ParseCreate();
        if (t.IsKeyword(Keyword::DROP))
            return ParseDrop();
        if (t.IsKeyword(Keyword::INSERT))
            return ParseInsert();
        if (t.IsKeyword(Keyword::COPY))
            return ParseCopy();
        if (t.IsKeyword(Keyword::EXPLAIN))
            return ParseExplain();
        if (IsWord("checkpoint")) { // a word, not a keyword: a table may still be called checkpoint
            Advance();
            return std::make_unique<CheckpointStatement>();
        }
        if (t.IsKeyword(Keyword::WITH))
            NotImplemented("WITH (common table expressions)", t);
        Fail("expected a statement (SELECT, CREATE, DROP, INSERT, COPY, CHECKPOINT or EXPLAIN)");
    }

    StatementPtr ParseExplain() {
        ExpectKeyword(Keyword::EXPLAIN);
        auto stmt = std::make_unique<ExplainStatement>();
        stmt->analyze = AcceptWord("analyze");
        if (Peek().IsKeyword(Keyword::EXPLAIN))
            Fail("EXPLAIN cannot be nested");
        stmt->inner = ParseStatementBody();
        return stmt;
    }

    StatementPtr ParseCreate() {
        ExpectKeyword(Keyword::CREATE);
        if (!Peek().IsKeyword(Keyword::TABLE))
            NotImplemented("CREATE " + Peek().text, Peek());
        ExpectKeyword(Keyword::TABLE);
        auto stmt = std::make_unique<CreateTableStatement>();
        if (AcceptKeyword(Keyword::IF)) {
            ExpectKeyword(Keyword::NOT);
            ExpectKeyword(Keyword::EXISTS);
            stmt->if_not_exists = true;
        }
        stmt->name = ParseIdentifier("a table name");
        if (Peek().IsKeyword(Keyword::AS))
            NotImplemented("CREATE TABLE ... AS", Peek());
        ExpectSymbol("(");
        do {
            const Token& first = Peek();
            if (first.IsKeyword(Keyword::PRIMARY) || first.IsKeyword(Keyword::UNIQUE)) {
                NotImplemented("table constraints", first);
            }
            std::string col_name = ParseIdentifier("a column name");
            ColumnSpec col{std::move(col_name), ParseType(), false};
            while (true) {
                if (Peek().IsKeyword(Keyword::NOT) && Peek(1).IsKeyword(Keyword::NULL_)) {
                    Advance();
                    Advance();
                    col.not_null = true;
                } else if (AcceptKeyword(Keyword::NULL_)) {
                    col.not_null = false;
                } else if (Peek().IsKeyword(Keyword::PRIMARY) ||
                           Peek().IsKeyword(Keyword::UNIQUE)) {
                    NotImplemented("column constraints (PRIMARY KEY / UNIQUE)", Peek());
                } else {
                    break;
                }
            }
            stmt->columns.push_back(std::move(col));
        } while (AcceptSymbol(","));
        ExpectSymbol(")");
        return stmt;
    }

    StatementPtr ParseDrop() {
        ExpectKeyword(Keyword::DROP);
        if (!Peek().IsKeyword(Keyword::TABLE))
            NotImplemented("DROP " + Peek().text, Peek());
        ExpectKeyword(Keyword::TABLE);
        auto stmt = std::make_unique<DropTableStatement>();
        if (AcceptKeyword(Keyword::IF)) {
            ExpectKeyword(Keyword::EXISTS);
            stmt->if_exists = true;
        }
        stmt->name = ParseIdentifier("a table name");
        return stmt;
    }

    StatementPtr ParseInsert() {
        ExpectKeyword(Keyword::INSERT);
        ExpectKeyword(Keyword::INTO);
        auto stmt = std::make_unique<InsertStatement>();
        stmt->table = ParseIdentifier("a table name");
        if (Peek().IsSymbol("(")) {
            Advance();
            do {
                stmt->columns.push_back(ParseIdentifier("a column name"));
            } while (AcceptSymbol(","));
            ExpectSymbol(")");
        }
        if (Peek().IsKeyword(Keyword::SELECT)) {
            stmt->select = std::shared_ptr<SelectStatement>(ParseSelect().release());
            return stmt;
        }
        ExpectKeyword(Keyword::VALUES);
        do {
            ExpectSymbol("(");
            std::vector<ExprPtr> row;
            do {
                row.push_back(ParseExpr());
            } while (AcceptSymbol(","));
            ExpectSymbol(")");
            stmt->rows.push_back(std::move(row));
        } while (AcceptSymbol(","));
        return stmt;
    }

    StatementPtr ParseCopy() {
        ExpectKeyword(Keyword::COPY);
        auto stmt = std::make_unique<CopyStatement>();
        stmt->table = ParseIdentifier("a table name");
        if (!Peek().IsKeyword(Keyword::FROM))
            NotImplemented("COPY ... TO / COPY (query)", Peek());
        ExpectKeyword(Keyword::FROM);
        if (Peek().type != TokenType::String)
            Fail("expected a quoted file path");
        stmt->path = Advance().text;
        const bool with = AcceptKeyword(Keyword::WITH);
        if (Peek().IsSymbol("(")) {
            Advance();
            do {
                if (AcceptWord("delimiter") || AcceptWord("delim")) {
                    if (Peek().type != TokenType::String)
                        Fail("expected a quoted delimiter");
                    stmt->delimiter = Advance().text;
                    if (stmt->delimiter.size() != 1) {
                        throw Error(ErrorCode::Syntax, "COPY delimiter must be a single character",
                                    tokens_[pos_ - 1].pos);
                    }
                } else if (AcceptWord("header")) {
                    stmt->header = true;
                    if (Peek().IsKeyword(Keyword::TRUE)) {
                        Advance();
                    } else if (Peek().IsKeyword(Keyword::FALSE)) {
                        Advance();
                        stmt->header = false;
                    }
                } else if (AcceptWord("format")) {
                    const Token& f = Peek();
                    if (!((f.type == TokenType::Identifier || f.type == TokenType::String) &&
                          f.text == "csv")) {
                        NotImplemented("COPY formats other than CSV", f);
                    }
                    Advance();
                } else {
                    Fail("expected a COPY option (DELIMITER, HEADER or FORMAT)");
                }
            } while (AcceptSymbol(","));
            ExpectSymbol(")");
        } else if (with) {
            Fail("expected \"(\"");
        }
        return stmt;
    }

    // ------------------------------------------------------------------ SELECT

    std::unique_ptr<SelectStatement> ParseSelect() {
        NestingGuard guard(*this);
        ExpectKeyword(Keyword::SELECT);
        auto sel = std::make_unique<SelectStatement>();
        if (AcceptKeyword(Keyword::DISTINCT)) {
            sel->distinct = true;
        } else {
            AcceptKeyword(Keyword::ALL);
        }
        do {
            sel->items.push_back(ParseSelectItem());
        } while (AcceptSymbol(","));

        if (AcceptKeyword(Keyword::FROM)) {
            sel->from = ParseFrom();
        }
        if (AcceptKeyword(Keyword::WHERE)) {
            sel->where = ParseExpr();
        }
        if (Peek().IsKeyword(Keyword::GROUP)) {
            Advance();
            ExpectKeyword(Keyword::BY);
            do {
                sel->group_by.push_back(ParseExpr());
            } while (AcceptSymbol(","));
        }
        if (AcceptKeyword(Keyword::HAVING)) {
            sel->having = ParseExpr();
        }
        if (Peek().IsKeyword(Keyword::ORDER)) {
            Advance();
            ExpectKeyword(Keyword::BY);
            do {
                OrderItem item;
                item.expr = ParseExpr();
                if (AcceptKeyword(Keyword::DESC)) {
                    item.descending = true;
                } else {
                    AcceptKeyword(Keyword::ASC);
                }
                if (AcceptWord("nulls")) {
                    if (AcceptWord("first")) {
                        item.nulls = NullOrder::First;
                    } else if (AcceptWord("last")) {
                        item.nulls = NullOrder::Last;
                    } else {
                        Fail("expected FIRST or LAST");
                    }
                }
                sel->order_by.push_back(std::move(item));
            } while (AcceptSymbol(","));
        }
        if (AcceptKeyword(Keyword::LIMIT)) {
            sel->limit = ParseExpr();
        }
        if (AcceptKeyword(Keyword::OFFSET)) {
            sel->offset = ParseExpr();
        }
        const Token& next = Peek();
        if (next.IsKeyword(Keyword::UNION) || next.IsKeyword(Keyword::INTERSECT) ||
            next.IsKeyword(Keyword::EXCEPT)) {
            NotImplemented("set operations (UNION / INTERSECT / EXCEPT)", next);
        }
        return sel;
    }

    SelectItem ParseSelectItem() {
        SelectItem item;
        const Token& t = Peek();
        if (t.IsSymbol("*")) {
            Advance();
            item.expr = std::make_unique<StarExpr>(t.pos, "");
            return item;
        }
        if ((t.type == TokenType::Identifier || t.type == TokenType::QuotedIdentifier) &&
            Peek(1).IsSymbol(".") && Peek(2).IsSymbol("*")) {
            std::string table = Advance().text;
            Advance();
            Advance();
            item.expr = std::make_unique<StarExpr>(t.pos, std::move(table));
            return item;
        }
        item.expr = ParseExpr();
        if (AcceptKeyword(Keyword::AS)) {
            item.alias = ParseIdentifier("an alias");
        } else if (Peek().type == TokenType::Identifier ||
                   Peek().type == TokenType::QuotedIdentifier) {
            item.alias = Advance().text;
        }
        return item;
    }

    TableRefPtr ParseFrom() {
        TableRefPtr left = ParseJoinChain();
        while (Peek().IsSymbol(",")) {
            const size_t pos = Advance().pos;
            TableRefPtr right = ParseJoinChain();
            left =
                std::make_unique<JoinRef>(pos, JoinType::Cross, std::move(left), std::move(right));
        }
        return left;
    }

    TableRefPtr ParseJoinChain() {
        TableRefPtr left = ParsePrimaryRef();
        while (true) {
            const Token& t = Peek();
            JoinType type;
            if (t.IsKeyword(Keyword::JOIN) || t.IsKeyword(Keyword::INNER)) {
                if (AcceptKeyword(Keyword::INNER)) {
                }
                type = JoinType::Inner;
            } else if (t.IsKeyword(Keyword::LEFT)) {
                Advance();
                AcceptKeyword(Keyword::OUTER);
                type = JoinType::Left;
            } else if (t.IsKeyword(Keyword::RIGHT)) {
                Advance();
                AcceptKeyword(Keyword::OUTER);
                type = JoinType::Right;
            } else if (t.IsKeyword(Keyword::FULL)) {
                Advance();
                AcceptKeyword(Keyword::OUTER);
                type = JoinType::Full;
            } else if (t.IsKeyword(Keyword::CROSS)) {
                Advance();
                type = JoinType::Cross;
            } else {
                break;
            }
            const size_t pos = Peek().pos;
            ExpectKeyword(Keyword::JOIN);
            TableRefPtr right = ParsePrimaryRef();
            auto join = std::make_unique<JoinRef>(pos, type, std::move(left), std::move(right));
            if (type != JoinType::Cross) {
                if (AcceptKeyword(Keyword::ON)) {
                    join->condition = ParseExpr();
                } else if (AcceptKeyword(Keyword::USING)) {
                    ExpectSymbol("(");
                    do {
                        join->using_columns.push_back(ParseIdentifier("a column name"));
                    } while (AcceptSymbol(","));
                    ExpectSymbol(")");
                } else {
                    Fail("expected ON or USING");
                }
            }
            left = std::move(join);
        }
        return left;
    }

    TableRefPtr ParsePrimaryRef() {
        const Token& t = Peek();
        if (t.IsSymbol("(")) {
            NestingGuard guard(*this);
            Advance();
            if (Peek().IsKeyword(Keyword::SELECT)) {
                auto select = std::shared_ptr<SelectStatement>(ParseSelect().release());
                ExpectSymbol(")");
                std::string alias = ParseOptionalAlias();
                auto ref =
                    std::make_unique<SubqueryRef>(t.pos, std::move(select), std::move(alias));
                ref->column_aliases = ParseOptionalColumnAliases(ref->alias);
                return ref;
            }
            TableRefPtr inner = ParseFrom();
            ExpectSymbol(")");
            return inner;
        }
        std::string name = ParseIdentifier("a table name or subquery");
        if (Peek().IsSymbol("."))
            NotImplemented("schema-qualified table names", Peek());
        auto ref = std::make_unique<BaseTableRef>(t.pos, std::move(name), ParseOptionalAlias());
        ref->column_aliases = ParseOptionalColumnAliases(ref->alias);
        return ref;
    }

    // `AS x (a, b)`: only meaningful when the relation has an alias.
    std::vector<std::string> ParseOptionalColumnAliases(const std::string& alias) {
        std::vector<std::string> names;
        if (!alias.empty() && Peek().IsSymbol("(")) {
            Advance();
            do {
                names.push_back(ParseIdentifier("a column alias"));
            } while (AcceptSymbol(","));
            ExpectSymbol(")");
        }
        return names;
    }

    std::string ParseOptionalAlias() {
        if (AcceptKeyword(Keyword::AS)) {
            return ParseIdentifier("an alias");
        }
        if (Peek().type == TokenType::Identifier || Peek().type == TokenType::QuotedIdentifier) {
            return Advance().text;
        }
        return "";
    }

    // ------------------------------------------------------------------ types

    LogicalType ParseType() {
        const Token& t = Peek();
        if (t.type != TokenType::Identifier && t.type != TokenType::QuotedIdentifier) {
            Fail("expected a type name");
        }
        std::string name = Advance().text;
        if (name == "double" && AcceptWord("precision")) {
            name = "double";
        }
        auto type = LogicalType::FromName(name);
        if (!type) {
            throw Error(ErrorCode::Syntax, "unknown type \"" + name + "\"", t.pos);
        }
        // Length / precision / scale arguments are accepted and ignored: VARCHAR(25),
        // DECIMAL(15,2).
        if (Peek().IsSymbol("(")) {
            Advance();
            do {
                if (Peek().type != TokenType::Integer)
                    Fail("expected an integer type parameter");
                Advance();
            } while (AcceptSymbol(","));
            ExpectSymbol(")");
        }
        return *type;
    }

    // ------------------------------------------------------------------ expressions
    //
    // Precedence, loosest to tightest:  OR  <  AND  <  NOT  <  predicates (comparison, IS,
    // BETWEEN, IN, LIKE)  <  + - ||  <  * / %  <  unary + -  <  ::cast  <  primary

    static uint32_t DepthOf(const ParsedExpr& e) { return e.depth; }

    template <class T> std::unique_ptr<T> Finish(std::unique_ptr<T> node, uint32_t child_depth) {
        node->depth = child_depth + 1;
        if (node->depth > kMaxExprDepth) {
            throw Error(ErrorCode::Syntax, "expression is nested too deeply", node->pos);
        }
        return node;
    }

    ExprPtr MakeBinary(size_t pos, BinaryOp op, ExprPtr l, ExprPtr r) {
        const uint32_t d = std::max(DepthOf(*l), DepthOf(*r));
        return Finish(std::make_unique<BinaryExpr>(pos, op, std::move(l), std::move(r)), d);
    }

    ExprPtr ParseExpr() { return ParseOr(); }

    ExprPtr ParseOr() {
        NestingGuard guard(*this);
        ExprPtr left = ParseAnd();
        while (Peek().IsKeyword(Keyword::OR)) {
            const size_t pos = Advance().pos;
            left = MakeBinary(pos, BinaryOp::Or, std::move(left), ParseAnd());
        }
        return left;
    }

    ExprPtr ParseAnd() {
        ExprPtr left = ParseNot();
        while (Peek().IsKeyword(Keyword::AND)) {
            const size_t pos = Advance().pos;
            left = MakeBinary(pos, BinaryOp::And, std::move(left), ParseNot());
        }
        return left;
    }

    ExprPtr ParseNot() {
        if (Peek().IsKeyword(Keyword::NOT)) {
            NestingGuard guard(*this);
            const size_t pos = Advance().pos;
            ExprPtr child = ParseNot();
            const uint32_t d = DepthOf(*child);
            return Finish(std::make_unique<UnaryExpr>(pos, UnaryOp::Not, std::move(child)), d);
        }
        return ParsePredicate();
    }

    ExprPtr ParsePredicate() {
        ExprPtr left = ParseAdditive();
        const Token& t = Peek();
        if (t.type == TokenType::Symbol) {
            BinaryOp op;
            bool is_cmp = true;
            if (t.text == "=")
                op = BinaryOp::Eq;
            else if (t.text == "<>")
                op = BinaryOp::Ne;
            else if (t.text == "<")
                op = BinaryOp::Lt;
            else if (t.text == "<=")
                op = BinaryOp::Le;
            else if (t.text == ">")
                op = BinaryOp::Gt;
            else if (t.text == ">=")
                op = BinaryOp::Ge;
            else
                is_cmp = false;
            if (is_cmp) {
                Advance();
                return MakeBinary(t.pos, op, std::move(left), ParseAdditive());
            }
            return left;
        }
        if (t.IsKeyword(Keyword::IS)) {
            Advance();
            const bool negated = AcceptKeyword(Keyword::NOT);
            if (!AcceptKeyword(Keyword::NULL_))
                Fail("expected NULL");
            const uint32_t d = DepthOf(*left);
            return Finish(std::make_unique<IsNullExpr>(t.pos, std::move(left), negated), d);
        }
        bool negated = false;
        if (t.IsKeyword(Keyword::NOT)) {
            const Token& after = Peek(1);
            if (!(after.IsKeyword(Keyword::BETWEEN) || after.IsKeyword(Keyword::IN) ||
                  after.IsKeyword(Keyword::LIKE))) {
                return left; // not ours; the caller will report the stray NOT
            }
            Advance();
            negated = true;
        }
        if (AcceptKeyword(Keyword::BETWEEN)) {
            ExprPtr lo = ParseAdditive();
            ExpectKeyword(Keyword::AND);
            ExprPtr hi = ParseAdditive();
            const uint32_t d = std::max({DepthOf(*left), DepthOf(*lo), DepthOf(*hi)});
            return Finish(std::make_unique<BetweenExpr>(t.pos, std::move(left), std::move(lo),
                                                        std::move(hi), negated),
                          d);
        }
        if (AcceptKeyword(Keyword::IN)) {
            ExpectSymbol("(");
            if (Peek().IsKeyword(Keyword::SELECT)) {
                auto select = std::shared_ptr<SelectStatement>(ParseSelect().release());
                ExpectSymbol(")");
                const uint32_t d = DepthOf(*left);
                return Finish(std::make_unique<InSubqueryExpr>(t.pos, std::move(left),
                                                               std::move(select), negated),
                              d);
            }
            std::vector<ExprPtr> list;
            uint32_t d = DepthOf(*left);
            do {
                list.push_back(ParseExpr());
                d = std::max(d, DepthOf(*list.back()));
            } while (AcceptSymbol(","));
            ExpectSymbol(")");
            return Finish(
                std::make_unique<InListExpr>(t.pos, std::move(left), std::move(list), negated), d);
        }
        if (AcceptKeyword(Keyword::LIKE)) {
            ExprPtr pattern = ParseAdditive();
            const uint32_t d = std::max(DepthOf(*left), DepthOf(*pattern));
            return Finish(
                std::make_unique<LikeExpr>(t.pos, std::move(left), std::move(pattern), negated), d);
        }
        return left;
    }

    ExprPtr ParseAdditive() {
        ExprPtr left = ParseMultiplicative();
        while (true) {
            const Token& t = Peek();
            BinaryOp op;
            if (t.IsSymbol("+"))
                op = BinaryOp::Add;
            else if (t.IsSymbol("-"))
                op = BinaryOp::Sub;
            else if (t.IsSymbol("||"))
                op = BinaryOp::Concat;
            else
                break;
            Advance();
            left = MakeBinary(t.pos, op, std::move(left), ParseMultiplicative());
        }
        return left;
    }

    ExprPtr ParseMultiplicative() {
        ExprPtr left = ParseUnary();
        while (true) {
            const Token& t = Peek();
            BinaryOp op;
            if (t.IsSymbol("*"))
                op = BinaryOp::Mul;
            else if (t.IsSymbol("/"))
                op = BinaryOp::Div;
            else if (t.IsSymbol("%"))
                op = BinaryOp::Mod;
            else
                break;
            Advance();
            left = MakeBinary(t.pos, op, std::move(left), ParseUnary());
        }
        return left;
    }

    ExprPtr ParseUnary() {
        const Token& t = Peek();
        if (t.IsSymbol("-") || t.IsSymbol("+")) {
            NestingGuard guard(*this);
            Advance();
            ExprPtr child = ParseUnary();
            if (t.IsSymbol("+"))
                return child; // unary plus is the identity
            const uint32_t d = DepthOf(*child);
            return Finish(std::make_unique<UnaryExpr>(t.pos, UnaryOp::Negate, std::move(child)), d);
        }
        return ParsePostfix();
    }

    ExprPtr ParsePostfix() {
        ExprPtr e = ParsePrimary();
        while (Peek().IsSymbol("::")) {
            const size_t pos = Advance().pos;
            LogicalType type = ParseType();
            const uint32_t d = DepthOf(*e);
            e = Finish(std::make_unique<CastExpr>(pos, std::move(e), type), d);
        }
        return e;
    }

    ExprPtr ParsePrimary() {
        const Token& t = Peek();
        switch (t.type) {
        case TokenType::Integer:
            return ParseIntegerLiteral();
        case TokenType::Decimal: {
            Advance();
            errno = 0;
            const double v = std::strtod(t.text.c_str(), nullptr);
            if (!std::isfinite(v)) {
                throw Error(ErrorCode::Syntax, "numeric literal out of range", t.pos);
            }
            return std::make_unique<ConstantExpr>(t.pos, LiteralKind::Double, Value::Double(v));
        }
        case TokenType::String:
            Advance();
            return std::make_unique<ConstantExpr>(t.pos, LiteralKind::String,
                                                  Value::Varchar(t.text));
        case TokenType::Keyword:
            return ParseKeywordPrimary();
        case TokenType::Symbol:
            if (t.IsSymbol("("))
                return ParseParenthesised();
            Fail("expected an expression");
        case TokenType::Identifier:
        case TokenType::QuotedIdentifier:
            return ParseIdentifierPrimary();
        case TokenType::End:
            Fail("expected an expression");
        }
        Fail("expected an expression");
    }

    ExprPtr ParseIntegerLiteral() {
        const Token& t = Advance();
        errno = 0;
        char* end = nullptr;
        const long long v = std::strtoll(t.text.c_str(), &end, 10);
        if (errno == ERANGE) {
            // Too big for BIGINT: becomes a DOUBLE, like most engines' fallback to NUMERIC.
            return std::make_unique<ConstantExpr>(
                t.pos, LiteralKind::Double, Value::Double(std::strtod(t.text.c_str(), nullptr)));
        }
        if (v >= std::numeric_limits<int32_t>::min() && v <= std::numeric_limits<int32_t>::max()) {
            return std::make_unique<ConstantExpr>(t.pos, LiteralKind::Integer,
                                                  Value::Integer(static_cast<int32_t>(v)));
        }
        return std::make_unique<ConstantExpr>(t.pos, LiteralKind::Integer,
                                              Value::BigInt(static_cast<int64_t>(v)));
    }

    ExprPtr ParseKeywordPrimary() {
        const Token& t = Peek();
        switch (t.keyword) {
        case Keyword::TRUE:
            Advance();
            return std::make_unique<ConstantExpr>(t.pos, LiteralKind::Boolean,
                                                  Value::Boolean(true));
        case Keyword::FALSE:
            Advance();
            return std::make_unique<ConstantExpr>(t.pos, LiteralKind::Boolean,
                                                  Value::Boolean(false));
        case Keyword::NULL_:
            Advance();
            return std::make_unique<ConstantExpr>(t.pos, LiteralKind::Null,
                                                  Value::Null(LogicalType::Integer()));
        case Keyword::CASE:
            return ParseCase();
        case Keyword::CAST: {
            NestingGuard guard(*this);
            Advance();
            ExpectSymbol("(");
            ExprPtr child = ParseExpr();
            ExpectKeyword(Keyword::AS);
            LogicalType type = ParseType();
            ExpectSymbol(")");
            const uint32_t d = DepthOf(*child);
            return Finish(std::make_unique<CastExpr>(t.pos, std::move(child), type), d);
        }
        case Keyword::EXISTS: {
            Advance();
            ExpectSymbol("(");
            if (!Peek().IsKeyword(Keyword::SELECT))
                Fail("expected SELECT");
            auto select = std::shared_ptr<SelectStatement>(ParseSelect().release());
            ExpectSymbol(")");
            return std::make_unique<ExistsExpr>(t.pos, std::move(select));
        }
        case Keyword::LEFT:
        case Keyword::RIGHT:
            // left(...) / right(...) string functions share their names with join keywords.
            if (Peek(1).IsSymbol("(")) {
                std::string name = t.keyword == Keyword::LEFT ? "left" : "right";
                Advance();
                return ParseFunctionCall(t.pos, std::move(name));
            }
            Fail("expected an expression");
        default:
            Fail("expected an expression");
        }
    }

    ExprPtr ParseParenthesised() {
        NestingGuard guard(*this);
        const size_t pos = Advance().pos; // (
        if (Peek().IsKeyword(Keyword::SELECT)) {
            auto select = std::shared_ptr<SelectStatement>(ParseSelect().release());
            ExpectSymbol(")");
            return std::make_unique<ScalarSubqueryExpr>(pos, std::move(select));
        }
        ExprPtr inner = ParseExpr();
        if (Peek().IsSymbol(","))
            NotImplemented("row values / tuples", Peek());
        ExpectSymbol(")");
        return inner;
    }

    ExprPtr ParseCase() {
        NestingGuard guard(*this);
        auto c = std::make_unique<CaseExpr>(Advance().pos); // CASE
        uint32_t d = 0;
        if (Peek().IsKeyword(Keyword::END))
            Fail("expected WHEN");
        if (!Peek().IsKeyword(Keyword::WHEN)) {
            c->operand = ParseExpr();
            d = DepthOf(*c->operand);
        }
        if (!Peek().IsKeyword(Keyword::WHEN))
            Fail("expected WHEN");
        while (AcceptKeyword(Keyword::WHEN)) {
            CaseExpr::When w;
            w.condition = ParseExpr();
            ExpectKeyword(Keyword::THEN);
            w.result = ParseExpr();
            d = std::max({d, DepthOf(*w.condition), DepthOf(*w.result)});
            c->whens.push_back(std::move(w));
        }
        if (AcceptKeyword(Keyword::ELSE)) {
            c->else_result = ParseExpr();
            d = std::max(d, DepthOf(*c->else_result));
        }
        ExpectKeyword(Keyword::END);
        return Finish(std::move(c), d);
    }

    ExprPtr ParseIdentifierPrimary() {
        const Token& t = Peek();
        if (t.type == TokenType::Identifier) {
            // typed literals and special forms are contextual: `date`, `interval`, ... stay usable
            // as ordinary identifiers everywhere else
            if (t.text == "date" && Peek(1).type == TokenType::String) {
                Advance();
                const Token& s = Advance();
                auto d = Date::FromString(s.text);
                if (!d) {
                    throw Error(ErrorCode::Syntax, "invalid date literal '" + s.text + "'", s.pos);
                }
                return std::make_unique<ConstantExpr>(t.pos, LiteralKind::Date, Value::Date(*d));
            }
            if (t.text == "interval" && Peek(1).type == TokenType::String) {
                return ParseInterval();
            }
            if (t.text == "extract" && Peek(1).IsSymbol("(")) {
                return ParseExtract();
            }
            if (t.text == "substring" && Peek(1).IsSymbol("(")) {
                Advance();
                return ParseSubstring(t.pos);
            }
        }
        // A quoted name may also be a function ("left"(x)): that is how reserved-word function
        // names are printed, and it keeps print -> parse a fixpoint.
        if (Peek(1).IsSymbol("(")) {
            std::string name = Advance().text;
            return ParseFunctionCall(t.pos, std::move(name));
        }
        std::string first = Advance().text;
        if (Peek().IsSymbol(".")) {
            Advance();
            const Token& col = Peek();
            if (col.type != TokenType::Identifier && col.type != TokenType::QuotedIdentifier) {
                Fail("expected a column name");
            }
            std::string column = Advance().text;
            if (Peek().IsSymbol("."))
                NotImplemented("schema-qualified column names", Peek());
            return std::make_unique<ColumnRefExpr>(t.pos, std::move(first), std::move(column));
        }
        return std::make_unique<ColumnRefExpr>(t.pos, "", std::move(first));
    }

    ExprPtr ParseFunctionCall(size_t pos, std::string name) {
        NestingGuard guard(*this);
        ExpectSymbol("(");
        auto fn = std::make_unique<FunctionExpr>(pos, std::move(name));
        uint32_t d = 0;
        if (AcceptSymbol("*")) {
            fn->star = true;
        } else if (!Peek().IsSymbol(")")) {
            if (AcceptKeyword(Keyword::DISTINCT)) {
                fn->distinct = true;
            } else {
                AcceptKeyword(Keyword::ALL);
            }
            do {
                fn->args.push_back(ParseExpr());
                d = std::max(d, DepthOf(*fn->args.back()));
            } while (AcceptSymbol(","));
        }
        ExpectSymbol(")");
        return Finish(std::move(fn), d);
    }

    ExprPtr ParseInterval() {
        const Token& kw = Advance();  // INTERVAL
        const Token& lit = Advance(); // '90' or '3 month'
        std::string text = lit.text;
        std::string unit;
        auto trim = [](std::string s) {
            const auto b = s.find_first_not_of(" \t");
            const auto e = s.find_last_not_of(" \t");
            return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
        };
        text = trim(text);
        const auto space = text.find_first_of(" \t");
        if (space != std::string::npos) { // '3 month'
            unit = trim(text.substr(space));
            text = text.substr(0, space);
        } else if (Peek().type == TokenType::Identifier) { // '90' day
            unit = Advance().text;
        } else {
            throw Error(ErrorCode::Syntax,
                        "interval literal needs a unit (e.g. INTERVAL '3' month)", lit.pos);
        }
        for (char& ch : unit)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (unit.size() > 1 && unit.back() == 's')
            unit.pop_back(); // days -> day
        static const char* const kUnits[] = {"year", "month",  "week",  "day",
                                             "hour", "minute", "second"};
        if (std::find(std::begin(kUnits), std::end(kUnits), unit) == std::end(kUnits)) {
            throw Error(ErrorCode::Syntax, "unknown interval unit \"" + unit + "\"", lit.pos);
        }
        errno = 0;
        char* end = nullptr;
        const long long amount = std::strtoll(text.c_str(), &end, 10);
        if (text.empty() || *end != '\0' || errno == ERANGE) {
            throw Error(ErrorCode::Syntax, "invalid interval amount '" + text + "'", lit.pos);
        }
        return std::make_unique<IntervalExpr>(kw.pos, static_cast<int64_t>(amount),
                                              std::move(unit));
    }

    ExprPtr ParseExtract() {
        NestingGuard guard(*this);
        const Token& kw = Advance(); // EXTRACT
        ExpectSymbol("(");
        const Token& field = Peek();
        if (field.type != TokenType::Identifier && field.type != TokenType::String) {
            Fail("expected a date part (year, month, day, ...)");
        }
        std::string name = Advance().text;
        for (char& ch : name)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        ExpectKeyword(Keyword::FROM);
        ExprPtr child = ParseExpr();
        ExpectSymbol(")");
        const uint32_t d = DepthOf(*child);
        return Finish(std::make_unique<ExtractExpr>(kw.pos, std::move(name), std::move(child)), d);
    }

    // substring(x FROM a [FOR b])  and  substring(x, a [, b])  both become substring(x, a[, b])
    ExprPtr ParseSubstring(size_t pos) {
        NestingGuard guard(*this);
        ExpectSymbol("(");
        auto fn = std::make_unique<FunctionExpr>(pos, "substring");
        fn->args.push_back(ParseExpr());
        if (AcceptKeyword(Keyword::FROM)) {
            fn->args.push_back(ParseExpr());
            if (AcceptWord("for"))
                fn->args.push_back(ParseExpr());
        } else {
            while (AcceptSymbol(","))
                fn->args.push_back(ParseExpr());
        }
        ExpectSymbol(")");
        uint32_t d = 0;
        for (const ExprPtr& a : fn->args)
            d = std::max(d, DepthOf(*a));
        return Finish(std::move(fn), d);
    }

    std::vector<Token> tokens_;
    size_t pos_ = 0;
    uint32_t nesting_ = 0;
};

} // namespace

std::vector<StatementPtr> ParseStatements(std::string_view sql) {
    return Parser(sql).ParseAll();
}

StatementPtr ParseStatement(std::string_view sql) {
    std::vector<StatementPtr> all = ParseStatements(sql);
    if (all.empty()) {
        throw Error(ErrorCode::Syntax, "empty query", 0);
    }
    if (all.size() > 1) {
        throw Error(ErrorCode::Syntax,
                    "expected a single statement but found " + std::to_string(all.size()));
    }
    return std::move(all[0]);
}

ExprPtr ParseExpression(std::string_view sql) {
    return Parser(sql).ParseStandaloneExpression();
}

std::string FormatErrorWithContext(std::string_view sql, const Error& error) {
    if (!error.position().has_value()) {
        return error.what();
    }
    const size_t pos = std::min(*error.position(), sql.size());
    size_t line_start = sql.rfind('\n', pos == 0 ? 0 : pos - 1);
    line_start = (line_start == std::string_view::npos || pos == 0) ? 0 : line_start + 1;
    if (pos > 0 && sql[pos - 1] == '\n' && line_start > pos)
        line_start = pos;
    size_t line_end = sql.find('\n', pos);
    if (line_end == std::string_view::npos)
        line_end = sql.size();
    size_t line_no = 1 + static_cast<size_t>(std::count(
                             sql.begin(), sql.begin() + static_cast<long>(line_start), '\n'));

    std::string line(sql.substr(line_start, line_end - line_start));
    for (char& c : line) {
        if (c == '\t' || c == '\r')
            c = ' ';
    }
    const std::string prefix = "LINE " + std::to_string(line_no) + ": ";
    return std::string(error.what()) + "\n" + prefix + line + "\n" +
           std::string(prefix.size() + (pos - line_start), ' ') + "^";
}

} // namespace cdb
