#include "pl_syntax.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <system_error>

namespace sdp::sql
{
namespace
{
using namespace std::literals;

constexpr size_t sql_bytes = 64 * 1024;
constexpr size_t token_limit = 4096;
constexpr size_t expression_depth = 64;
constexpr size_t identifier_bytes = 128;
constexpr size_t column_limit = 128;
constexpr size_t insert_row_limit = 512;
constexpr size_t constraint_limit = 128;
constexpr size_t query_depth_limit = 12;
constexpr size_t query_part_limit = 8;
constexpr size_t program_depth_limit = 24;
constexpr size_t routine_variable_limit = 64;
constexpr uint32_t history_page_limit = 256;

enum class TokenKind
{
    word, quoted_name, quoted_text, integer, decimal, lparen, rparen, comma, dot, semicolon,
    colon, assign, range, star, plus, minus, slash, eq, ne, lt, le, gt, ge, other_operator, end
};

struct Token
{
    TokenKind kind;
    Source source;
    std::string text;
};

bool digit(char value)
{
    return value >= '0' && value <= '9';
}

bool identifier_start(char value)
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
}

bool whitespace(char value)
{
    return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}

char ascii_lower(char value)
{
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

constexpr std::array supported_words{
    "add"sv, "all"sv, "alter"sv, "analyze"sv, "and"sv, "as"sv, "bigint"sv,
    "blob"sv, "boolean"sv, "by"sv, "check"sv, "collate"sv, "column"sv, "constraint"sv,
    "create"sv, "cross"sv, "database"sv, "date"sv, "decimal"sv, "default"sv, "delete"sv,
    "distinct"sv, "drop"sv, "exists"sv, "explain"sv, "false"sv, "for"sv,
    "foreign"sv, "from"sv, "group"sv, "having"sv, "identity"sv, "index"sv, "inner"sv,
    "insert"sv, "int"sv, "integer"sv, "into"sv, "is"sv, "join"sv, "left"sv, "not"sv,
    "null"sv, "on"sv, "or"sv, "order"sv, "outer"sv, "primary"sv, "references"sv,
    "reindex"sv, "restrict"sv, "schema"sv, "select"sv, "sequence"sv, "set"sv, "table"sv,
    "text"sv, "time"sv, "timestamp"sv, "true"sv, "truncate"sv, "union"sv, "unique"sv,
    "update"sv, "values"sv, "view"sv, "where"sv, "with"sv, "zone"sv
};
constexpr std::array unsupported_words{
    "asc"sv, "begin"sv, "between"sv, "call"sv, "cascade"sv, "case"sv, "cast"sv, "char"sv, "commit"sv,
    "current_date"sv, "current_time"sv, "current_timestamp"sv, "cursor"sv, "deferrable"sv,
    "desc"sv, "except"sv, "execute"sv, "fetch"sv, "full"sv, "function"sv, "grant"sv, "history"sv, "if"sv,
    "in"sv, "intersect"sv, "like"sv, "limit"sv, "merge"sv, "natural"sv, "number"sv, "numeric"sv,
    "offset"sv, "over"sv, "procedure"sv, "recursive"sv, "replace"sv, "revoke"sv, "right"sv,
    "rollback"sv, "savepoint"sv, "trigger"sv, "using"sv, "varchar"sv, "varchar2"sv, "window"sv
};

constexpr std::array aggregate_words{"avg"sv, "count"sv, "max"sv, "min"sv, "sum"sv};
constexpr std::array permission_words{
    "usage"sv, "select"sv, "insert"sv, "update"sv, "delete"sv, "ddl"sv, "execute"sv,
    "history"sv, "backup"sv, "debug"sv, "monitor"sv, "control"sv
};
constexpr std::array security_kinds{
    "database"sv, "schema"sv, "table"sv, "view"sv, "routine"sv, "model"sv, "global"sv
};

bool contains_word(const auto& words, std::string_view token)
{
    return std::ranges::find(words, token) != words.end();
}

bool reserved(std::string_view token)
{
    return contains_word(supported_words, token) || contains_word(unsupported_words, token);
}

class Lexer
{
public:
    explicit Lexer(std::string_view input) : input_(input)
    {
        if (input.size() > sql_bytes)
            throw Error(Code::limit, position_, "SQL text exceeds 64 KiB");
    }

    std::vector<Token> tokenize()
    {
        std::vector<Token> tokens;
        for (;;)
        {
            skip_ignored();
            if (eof())
            {
                tokens.push_back({TokenKind::end, position_, {}});
                return tokens;
            }
            if (tokens.size() == token_limit)
                throw Error(Code::limit, position_, "SQL token limit exceeded");
            tokens.push_back(next());
        }
    }

private:
    bool eof() const { return position_.offset == input_.size(); }
    char peek() const { return eof() ? '\0' : input_[position_.offset]; }
    bool starts(std::string_view candidate) const { return input_.substr(position_.offset).starts_with(candidate); }

    void advance()
    {
        const auto value = peek();
        const auto width = utf8_width(input_.substr(position_.offset));
        if (!width)
            throw Error(Code::syntax, position_, "Invalid UTF-8 in SQL source");
        if (static_cast<unsigned char>(value) < 0x20 && !whitespace(value))
            throw Error(Code::syntax, position_, "Control character in SQL source");

        const bool continues_crlf = value == '\n' && position_.offset && input_[position_.offset - 1] == '\r';
        position_.offset += width;
        if (value == '\r' || value == '\n')
        {
            if (!continues_crlf) ++position_.line;
            position_.column = 1;
        }
        else
        {
            ++position_.column;
        }
    }

    void skip_ignored()
    {
        for (;;)
        {
            while (whitespace(peek())) advance();
            if (starts("--"))
            {
                while (!eof() && peek() != '\r' && peek() != '\n') advance();
            }
            else if (starts("/*"))
            {
                const auto start = position_;
                advance();
                advance();
                while (!eof() && !starts("*/")) advance();
                if (eof()) throw Error(Code::syntax, start, "Unterminated block comment");
                advance();
                advance();
            }
            else
            {
                return;
            }
        }
    }

    Token word(Source start)
    {
        do { advance(); } while (identifier_start(peek()) || digit(peek()));
        std::string text(input_.substr(start.offset, position_.offset - start.offset));
        std::ranges::transform(text, text.begin(), ascii_lower);
        return {TokenKind::word, start, std::move(text)};
    }

    Token quoted(Source start, char delimiter)
    {
        const auto kind = delimiter == '"' ? TokenKind::quoted_name : TokenKind::quoted_text;
        advance();
        std::string text;
        while (!eof())
        {
            const auto begin = position_.offset;
            if (peek() == delimiter)
            {
                advance();
                if (peek() != delimiter) return {kind, start, std::move(text)};
                text.push_back(delimiter);
                advance();
            }
            else
            {
                advance();
                text.append(input_.substr(begin, position_.offset - begin));
            }
        }
        throw Error(Code::syntax, start, "Unterminated quoted token");
    }

    Token number(Source start)
    {
        do { advance(); } while (digit(peek()));
        auto kind = TokenKind::integer;
        if (peek() == '.' && position_.offset + 1 < input_.size() && digit(input_[position_.offset + 1]))
        {
            kind = TokenKind::decimal;
            advance();
            do { advance(); } while (digit(peek()));
        }
        return {kind, start, std::string(input_.substr(start.offset, position_.offset - start.offset))};
    }

    Token next()
    {
        const auto start = position_;
        const auto value = peek();
        if (identifier_start(value)) return word(start);
        if (digit(value)) return number(start);
        if (value == '"' || value == '\'') return quoted(start, value);

        advance();
        auto kind = TokenKind::end;
        switch (value)
        {
        case '(': kind = TokenKind::lparen; break;
        case ')': kind = TokenKind::rparen; break;
        case ',': kind = TokenKind::comma; break;
        case '.':
            if (peek() == '.') { advance(); kind = TokenKind::range; }
            else kind = TokenKind::dot;
            break;
        case ';': kind = TokenKind::semicolon; break;
        case ':':
            if (peek() == '=') { advance(); kind = TokenKind::assign; }
            else kind = TokenKind::colon;
            break;
        case '*': kind = TokenKind::star; break;
        case '-': kind = TokenKind::minus; break;
        case '=': kind = TokenKind::eq; break;
        case '<':
            if (peek() == '=') { advance(); kind = TokenKind::le; }
            else if (peek() == '>') { advance(); kind = TokenKind::ne; }
            else kind = TokenKind::lt;
            break;
        case '>':
            if (peek() == '=') { advance(); kind = TokenKind::ge; }
            else kind = TokenKind::gt;
            break;
        case '!':
            if (peek() != '=') throw Error(Code::syntax, start, "Expected !=");
            advance();
            kind = TokenKind::ne;
            break;
        case '+': kind = TokenKind::plus; break;
        case '/': kind = TokenKind::slash; break;
        case '%': case '|': kind = TokenKind::other_operator; break;
        default: throw Error(Code::syntax, start, "Unexpected character in SQL source");
        }
        return {kind, start, {}};
    }

    std::string_view input_;
    Source position_;
};

bool word_is(const Token& token, std::string_view keyword)
{
    return token.kind == TokenKind::word && token.text == keyword;
}

bool is_name(const Token& token)
{
    return token.kind == TokenKind::quoted_name || (token.kind == TokenKind::word && !reserved(token.text));
}

using Op = Expr::Op;

struct Expression
{
    Expr node;
    size_t height = 1;
};

struct Operator
{
    Op op;
    unsigned precedence;
    bool null_test = false;
};

std::optional<Operator> infix(const Token& token)
{
    switch (token.kind)
    {
    case TokenKind::star: return Operator{Op::multiply, 5};
    case TokenKind::slash: return Operator{Op::divide, 5};
    case TokenKind::plus: return Operator{Op::add, 4};
    case TokenKind::minus: return Operator{Op::subtract, 4};
    case TokenKind::eq: return Operator{Op::eq, 3};
    case TokenKind::ne: return Operator{Op::ne, 3};
    case TokenKind::lt: return Operator{Op::lt, 3};
    case TokenKind::le: return Operator{Op::le, 3};
    case TokenKind::gt: return Operator{Op::gt, 3};
    case TokenKind::ge: return Operator{Op::ge, 3};
    default: break;
    }
    if (word_is(token, "is")) return Operator{Op::is_null, 3, true};
    if (word_is(token, "and")) return Operator{Op::and_, 2};
    if (word_is(token, "or")) return Operator{Op::or_, 1};
    return {};
}

struct SelectInto
{
    size_t begin{};
    size_t end{};
    std::vector<Name> targets;
};

class Parser
{
public:
    Parser(std::string_view input, std::vector<Token> tokens) : input_(input), tokens_(std::move(tokens)) {}

    Statement parse()
    {
        const auto start = current().source;
        Statement statement{};
        statement.source = start;
        if (word_is(current(), "select") || word_is(current(), "with")) query(statement);
        else if (take_word("explain")) explain(statement);
        else if (take_word("analyze")) analyze(statement);
        else if (take_word("reindex")) reindex(statement);
        else if (take_word("alter")) alter(statement);
        else if (take_word("create")) object(statement, true);
        else if (take_word("drop")) object(statement, false);
        else if (take_word("insert")) insert(statement);
        else if (take_word("update")) update(statement);
        else if (take_word("delete")) remove(statement);
        else if (take_word("truncate")) truncate(statement);
        else if (take_word("grant")) grant(statement, true);
        else if (take_word("revoke")) grant(statement, false);
        else if (take_word("history")) history(statement);
        else if (take_word("decision")) decision_inspect(statement);
        else if (take_word("train")) model_request(statement, ModelCommand::Kind::train, false);
        else if (take_word("evaluate")) model_request(statement, ModelCommand::Kind::evaluate, false);
        else if (take_word("predict")) model_request(statement, ModelCommand::Kind::predict, false);
        else if (take_word("call")) call_routine(statement);
        else if (word_is(current(), "begin") || word_is(current(), "declare")) anonymous_block(statement);
        else fail("Expected a supported SQL statement");

        if (word_is(current(), "returning") && (statement.action == Action::insert ||
            statement.action == Action::update || statement.action == Action::delete_))
            throw Error(Code::unsupported, current().source, "DML RETURNING is unavailable");
        if (take(TokenKind::semicolon) && current().kind != TokenKind::end)
            throw Error(Code::unsupported, current().source, "Multiple SQL statements are unavailable");
        expect(TokenKind::end, "Unexpected token after statement");
        return statement;
    }

    Name parse_history_target()
    {
        auto result = name(3);
        expect(TokenKind::end, "Unexpected token after History target");
        return result;
    }


private:
    const Token& current() const { return tokens_[index_]; }
    bool take(TokenKind kind)
    {
        if (current().kind != kind) return false;
        ++index_;
        return true;
    }
    bool take_word(std::string_view word)
    {
        if (!word_is(current(), word)) return false;
        ++index_;
        return true;
    }
    bool following(TokenKind kind) const
    {
        return index_ + 1 < tokens_.size() && tokens_[index_ + 1].kind == kind;
    }
    bool following_word(std::string_view word) const
    {
        return index_ + 1 < tokens_.size() && word_is(tokens_[index_ + 1], word);
    }
    Token consume()
    {
        return std::move(tokens_[index_++]);
    }

    [[noreturn]] void fail(const char* message) const
    {
        if (current().kind == TokenKind::other_operator ||
            (current().kind == TokenKind::word && contains_word(unsupported_words, current().text)))
            throw Error(Code::unsupported, current().source, "SQL construct is unavailable");
        throw Error(Code::syntax, current().source, message);
    }
    void expect(TokenKind kind, const char* message)
    {
        if (!take(kind)) fail(message);
    }
    void expect_word(std::string_view word, const char* message)
    {
        if (!take_word(word)) fail(message);
    }

    std::string identifier()
    {
        if (!is_name(current())) fail("Expected identifier");
        const auto token = consume();
        if (token.text.empty())
            throw Error(Code::syntax, token.source, "Quoted identifier is empty");
        if (token.text.size() > identifier_bytes)
            throw Error(Code::limit, token.source, "Identifier exceeds 128 UTF-8 bytes");
        return token.text;
    }

    Name name(size_t maximum)
    {
        Name value{{}, current().source};
        value.parts.push_back(identifier());
        while (current().kind == TokenKind::dot)
        {
            if (value.parts.size() == maximum) fail("Too many identifier qualifiers");
            ++index_;
            value.parts.push_back(identifier());
        }
        return value;
    }

    uint8_t size_part()
    {
        if (current().kind != TokenKind::integer) fail("Expected non-negative decimal size");
        const auto token = consume();
        const auto end = token.text.data() + token.text.size();
        unsigned number{};
        const auto parsed = std::from_chars(token.text.data(), end, number);
        if (parsed.ec != std::errc{} || parsed.ptr != end || number > 255)
            throw Error(Code::range, token.source, "Decimal type size is out of range");
        return static_cast<uint8_t>(number);
    }

    Type type(bool procedural = false)
    {
        const auto start = current().source;
        if (procedural && take_word("record")) return {Kind::record};
        if (procedural && take_word("collection")) return {Kind::collection};
        if (take_word("boolean")) return {Kind::boolean};
        if (take_word("integer") || take_word("int") || take_word("bigint")) return {Kind::integer};
        if (take_word("text")) return {Kind::text};
        if (take_word("blob")) return {Kind::blob};
        if (take_word("date")) return {Kind::date};
        if (take_word("time")) return {Kind::time};
        if (take_word("timestamp"))
        {
            expect_word("with", "Expected WITH TIME ZONE for TIMESTAMP");
            expect_word("time", "Expected TIME in TIMESTAMP WITH TIME ZONE");
            expect_word("zone", "Expected ZONE in TIMESTAMP WITH TIME ZONE");
            return {Kind::timestamp_tz};
        }
        if (!take_word("decimal")) fail("Expected a supported data type");
        expect(TokenKind::lparen, "Expected DECIMAL(precision, scale)");
        const auto precision = size_part();
        expect(TokenKind::comma, "Expected comma after DECIMAL precision");
        const auto scale = size_part();
        expect(TokenKind::rparen, "Expected closing parenthesis after DECIMAL scale");
        const Type result{Kind::decimal, precision, scale};
        validate_type(result, start);
        return result;
    }

    std::vector<Name> column_names()
    {
        expect(TokenKind::lparen, "Expected opening parenthesis before column names");
        std::vector<Name> names;
        do
        {
            if (names.size() == column_limit)
                throw Error(Code::limit, current().source, "Column name list limit exceeded");
            names.push_back(name(1));
        } while (take(TokenKind::comma));
        expect(TokenKind::rparen, "Expected closing parenthesis after column names");
        return names;
    }

    bool constraint_starts(bool on_column) const
    {
        return word_is(current(), "constraint") || word_is(current(), "primary") ||
            word_is(current(), "unique") || word_is(current(), "check") ||
            word_is(current(), on_column ? "references" : "foreign");
    }

    void foreign_key_actions()
    {
        bool on_delete = false;
        bool on_update = false;
        while (take_word("on"))
        {
            const auto source = current().source;
            if (take_word("delete"))
            {
                if (std::exchange(on_delete, true))
                    throw Error(Code::syntax, source, "Repeated ON DELETE action");
            }
            else if (take_word("update"))
            {
                if (std::exchange(on_update, true))
                    throw Error(Code::syntax, source, "Repeated ON UPDATE action");
            }
            else fail("Expected DELETE or UPDATE after ON");

            if (take_word("no"))
                expect_word("action", "Expected ACTION after NO");
            else if (!take_word("restrict"))
                throw Error(Code::unsupported, current().source, "Foreign keys accept only NO ACTION or RESTRICT");
        }
    }

    void add_constraint(Statement& statement, const Name* column_name = nullptr)
    {
        if (statement.constraints.size() == constraint_limit)
            throw Error(Code::limit, current().source, "Table constraint limit exceeded");
        using Kind = ConstraintDefinition::Kind;
        ConstraintDefinition constraint{};
        constraint.source = current().source;
        if (take_word("constraint")) constraint.name = identifier();
        if (column_name) constraint.columns = {*column_name};

        if (take_word("primary"))
        {
            expect_word("key", "Expected KEY after PRIMARY");
            constraint.kind = Kind::primary_key;
            if (!column_name) constraint.columns = column_names();
        }
        else if (take_word("unique"))
        {
            constraint.kind = Kind::unique;
            if (!column_name) constraint.columns = column_names();
        }
        else if (take_word("check"))
        {
            constraint.kind = Kind::check;
            expect(TokenKind::lparen, "Expected opening parenthesis before CHECK expression");
            constraint.predicate = expression(1, 1).node;
            expect(TokenKind::rparen, "Expected closing parenthesis after CHECK expression");
        }
        else
        {
            if (!column_name)
            {
                expect_word("foreign", "Expected PRIMARY KEY, UNIQUE, CHECK or FOREIGN KEY");
                expect_word("key", "Expected KEY after FOREIGN");
                constraint.columns = column_names();
            }
            expect_word("references", "Expected REFERENCES");
            constraint.kind = Kind::foreign_key;
            constraint.referenced_table = name(3);
            if (current().kind != TokenKind::lparen)
                throw Error(Code::unsupported, current().source, "Foreign keys require explicit referenced columns");
            constraint.referenced_columns = column_names();
            foreign_key_actions();
        }

        if (word_is(current(), "deferrable") || word_is(current(), "initially") ||
            (word_is(current(), "not") && following_word("deferrable")))
            throw Error(Code::unsupported, current().source, "Constraint deferrability options are unavailable");
        statement.constraints.push_back(std::move(constraint));
    }

    ColumnDefinition column(Statement& statement)
    {
        ColumnDefinition result{name(1), type(), true, {}, false};
        bool null_option = false;
        const bool creating = statement.action == Action::create_table;
        for (;;)
        {
            const auto source = current().source;
            if (word_is(current(), "null") || word_is(current(), "not"))
            {
                if (std::exchange(null_option, true))
                    throw Error(Code::syntax, source, "Repeated column nullability clause");
                result.nullable = !take_word("not");
                expect_word("null", "Expected NULL after NOT in column declaration");
            }
            else if (take_word("default"))
            {
                if (!creating)
                    throw Error(Code::unsupported, source, "ADD COLUMN with DEFAULT is unavailable");
                if (result.default_value || result.identity)
                    throw Error(Code::syntax, source, "Multiple column defaults are unavailable");
                result.default_value = expression(1, 1).node;
            }
            else if (take_word("generated"))
            {
                if (!creating)
                    throw Error(Code::unsupported, source, "ADD COLUMN with IDENTITY is unavailable");
                if (result.default_value || result.identity)
                    throw Error(Code::syntax, source, "Multiple column defaults are unavailable");
                if (word_is(current(), "always"))
                    throw Error(Code::unsupported, current().source, "GENERATED ALWAYS AS IDENTITY is unavailable");
                expect_word("by", "Expected BY after GENERATED");
                expect_word("default", "Expected DEFAULT after GENERATED BY");
                expect_word("as", "Expected AS in IDENTITY declaration");
                expect_word("identity", "Expected IDENTITY after GENERATED BY DEFAULT AS");
                result.identity = true;
            }
            else if (constraint_starts(true))
            {
                if (!creating)
                    throw Error(Code::unsupported, source, "ADD COLUMN with a key or CHECK is unavailable");
                add_constraint(statement, &result.name);
            }
            else
            {
                return result;
            }
        }
    }

    void create_table_columns(Statement& statement)
    {
        expect(TokenKind::lparen, "Expected opening parenthesis for table columns");
        do
        {
            if (constraint_starts(false))
            {
                add_constraint(statement);
                continue;
            }
            if (statement.columns.size() == column_limit)
                throw Error(Code::limit, current().source, "Table column limit exceeded");
            statement.columns.push_back(column(statement));
        } while (take(TokenKind::comma));
        if (statement.columns.empty()) fail("A table requires at least one column");
        expect(TokenKind::rparen, "Expected closing parenthesis after table definition");
    }

    void index(Statement& statement, bool create, bool unique)
    {
        statement.action = create ? Action::create_index : Action::drop_index;
        statement.target = name(3);
        if (!create) return;

        statement.unique_index = unique;
        expect_word("on", "Expected ON after index name");
        statement.related = name(3);
        statement.key_columns = column_names();
        if (take_word("collate")) statement.collation = identifier();
    }

    void view(Statement& statement, bool create)
    {
        statement.action = create ? Action::create_view : Action::drop_view;
        statement.target = name(3);
        if (!create) return;

        expect_word("as", "Expected AS after view name");
        const auto start = current().source;
        Statement definition{};
        definition.source = start;
        query(definition);
        statement.view_sql.assign(input_.substr(start.offset, current().source.offset - start.offset));
    }

    void object(Statement& statement, bool create)
    {
        if (create && take_word("model"))
        {
            model_request(statement, ModelCommand::Kind::create, true);
            return;
        }
        bool replace = false;
        if (create && take_word("or"))
        {
            expect_word("replace", "Expected REPLACE after CREATE OR");
            replace = true;
        }
        if (create && (word_is(current(), "function") || word_is(current(), "procedure")))
        {
            create_routine(statement, replace);
            return;
        }
        if (replace)
            throw Error(Code::unsupported, current().source, "CREATE OR REPLACE is only available for routines");
        if (create && take_word("decision"))
        {
            decision_rule(statement, true);
            return;
        }
        if (take_word("trigger"))
        {
            trigger(statement, create);
            return;
        }
        if (create && (word_is(current(), "user") || word_is(current(), "role")))
        {
            const auto role = take_word("role");
            if (!role) expect_word("user", "Expected USER");
            statement.action = role ? Action::create_role : Action::create_user;
            statement.target = name(1);
            statement.security_subject = statement.target.parts.front();
            statement.security_role = role;
            if (current().kind != TokenKind::end && current().kind != TokenKind::semicolon)
                throw Error(Code::unsupported, current().source,
                    "CREATE USER and ROLE accept only a name. Passwords require an authenticated operation");
        }
        else if (create && take_word("unique"))
        {
            expect_word("index", "Expected INDEX after CREATE UNIQUE");
            index(statement, true, true);
        }
        else if (take_word("index")) index(statement, create, false);
        else if (take_word("view")) view(statement, create);
        else if (take_word("sequence"))
        {
            statement.action = create ? Action::create_sequence : Action::drop_sequence;
            statement.target = name(3);
            if (create && current().kind == TokenKind::word)
                throw Error(Code::unsupported, current().source, "CREATE SEQUENCE options are unavailable");
        }
        else if (take_word("database"))
        {
            statement.action = create ? Action::create_database : Action::drop_database;
            statement.target = name(1);
        }
        else if (take_word("schema"))
        {
            statement.action = create ? Action::create_schema : Action::drop_schema;
            statement.target = name(2);
        }
        else
        {
            expect_word("table", "Expected DATABASE, SCHEMA, TABLE, INDEX, SEQUENCE or VIEW");
            statement.action = create ? Action::create_table : Action::drop_table;
            statement.target = name(3);
            if (create) create_table_columns(statement);
        }
    }

    void check_variable_limit(const pl::Routine& routine) const
    {
        if (routine.variables.size() + routine.cursors.size() + routine.out_cursors.size() >= routine_variable_limit)
            throw Error(Code::limit, current().source, "Routine variable limit exceeded");
    }

    void routine_parameters(pl::Routine& routine)
    {
        expect(TokenKind::lparen, "Expected opening parenthesis before routine parameters");
        if (take(TokenKind::rparen)) return;
        do
        {
            check_variable_limit(routine);
            pl::Variable variable{};
            variable.name = name(1);
            if (take_word("in"))
                variable.mode = take_word("out") ? pl::Mode::in_out : pl::Mode::in;
            else
                variable.mode = take_word("out") ? pl::Mode::out : pl::Mode::in;
            if (take_word("refcursor"))
            {
                if (routine.function || variable.mode != pl::Mode::out)
                    throw Error(Code::unsupported, variable.name.source, "REFCURSOR parameters require procedure OUT");
                routine.out_cursors.push_back(std::move(variable.name.parts.front()));
            }
            else
            {
                variable.type = type(true);
                routine.variables.push_back(std::move(variable));
            }
        } while (take(TokenKind::comma));
        expect(TokenKind::rparen, "Expected closing parenthesis after routine parameters");
    }

    void local_variables(pl::Routine& routine)
    {
        while (!word_is(current(), "begin"))
        {
            check_variable_limit(routine);
            auto local_name = name(1);
            if (take_word("refcursor"))
            {
                expect(TokenKind::semicolon, "Expected semicolon after REFCURSOR declaration");
                routine.cursors.push_back(std::move(local_name.parts.front()));
                continue;
            }
            pl::Variable variable{std::move(local_name), type(true), pl::Mode::local, {}};
            if (take(TokenKind::assign)) variable.initial = expression(1, 1).node;
            expect(TokenKind::semicolon, "Expected semicolon after local declaration");
            routine.variables.push_back(std::move(variable));
        }
    }

    void routine_body(pl::Routine& routine)
    {
        const auto declarations_follow = take_word("as") || take_word("is");
        if (take_word("declare") || declarations_follow) local_variables(routine);
        block_contents(routine.body, routine.handlers, 1);
        if (!routine.name.parts.empty() && is_name(current()))
        {
            const auto source = current().source;
            if (identifier() != routine.name.parts.back())
                throw Error(Code::name, source, "Routine END name does not match its declaration");
        }
        routine.source.assign(input_);
    }

    void create_routine(Statement& statement, bool replace)
    {
        statement.action = Action::create_routine;
        statement.routine = std::make_shared<pl::Routine>();
        auto& routine = *statement.routine;
        routine.function = take_word("function");
        if (!routine.function) expect_word("procedure", "Expected FUNCTION or PROCEDURE");
        routine.replace = replace;
        routine.name = name(3);
        statement.target = routine.name;
        if (current().kind == TokenKind::lparen) routine_parameters(routine);
        if (routine.function)
        {
            if (!take_word("return")) expect_word("returns", "Expected RETURN type for function");
            routine.returns = type(true);
        }
        routine_body(routine);
    }

    void trigger(Statement& statement, bool create)
    {
        statement.action = create ? Action::create_trigger : Action::drop_trigger;
        statement.target = name(3);
        if (!create) return;

        if (!word_is(current(), "before") && !word_is(current(), "after"))
            throw Error(Code::unsupported, current().source, "Trigger timing must be BEFORE or AFTER");
        statement.trigger_timing = consume().text;
        if (!word_is(current(), "insert") && !word_is(current(), "update") && !word_is(current(), "delete"))
            throw Error(Code::unsupported, current().source, "Trigger event must be INSERT, UPDATE or DELETE");
        statement.trigger_event = consume().text;
        expect_word("on", "Expected ON after trigger event");
        statement.related = name(3);
        expect_word("for", "Expected FOR EACH ROW after trigger target");
        expect_word("each", "Expected EACH after trigger FOR");
        if (!take_word("row"))
            throw Error(Code::unsupported, current().source, "Only FOR EACH ROW triggers are available");

        statement.routine = std::make_shared<pl::Routine>();
        statement.routine->name = statement.target;
        routine_body(*statement.routine);
    }

    void anonymous_block(Statement& statement)
    {
        statement.action = Action::pl_block;
        statement.routine = std::make_shared<pl::Routine>();
        routine_body(*statement.routine);
    }

    void call_routine(Statement& statement)
    {
        statement.action = Action::call_routine;
        auto call = function_call(name(3), 1);
        statement.target = std::move(call.node.name);
        statement.projection = std::move(call.node.args);
    }

    void check_program_depth(size_t depth) const
    {
        if (depth > program_depth_limit)
            throw Error(Code::limit, current().source, "PL/SQL block depth exceeded");
    }

    bool procedural_boundary() const
    {
        if (current().kind == TokenKind::end) return true;
        if (following(TokenKind::assign)) return false;
        constexpr std::array boundaries{"end"sv, "elsif"sv, "else"sv, "exception"sv, "when"sv};
        return current().kind == TokenKind::word && contains_word(boundaries, current().text);
    }

    std::vector<pl::Node> procedural_body(size_t depth)
    {
        check_program_depth(depth);
        std::vector<pl::Node> nodes;
        while (!procedural_boundary())
        {
            nodes.push_back(procedural_node(depth));
            expect(TokenKind::semicolon, "Expected semicolon after PL/SQL statement");
        }
        return nodes;
    }

    void block_contents(std::vector<pl::Node>& body, std::vector<pl::Node>& handlers, size_t depth)
    {
        expect_word("begin", "Expected BEGIN before PL/SQL body");
        body = procedural_body(depth);
        if (take_word("exception"))
        {
            expect_word("when", "Expected WHEN after EXCEPTION");
            if (!take_word("others"))
                throw Error(Code::unsupported, current().source, "Only WHEN OTHERS exception handling is available");
            expect_word("then", "Expected THEN after WHEN OTHERS");
            handlers = procedural_body(depth);
            if (handlers.empty())
                fail("WHEN OTHERS requires a handler statement");
            if (word_is(current(), "when"))
                throw Error(Code::unsupported, current().source, "Additional exception handlers are unavailable");
        }
        expect_word("end", "Expected END after PL/SQL body");
    }

    pl::Node conditional(Source start, bool case_statement, size_t depth)
    {
        std::vector<pl::Node> branches;
        do
        {
            check_program_depth(depth + branches.size());
            pl::Node branch{};
            branch.kind = pl::Node::Kind::if_;
            branch.source = start;
            branch.first = expression(1, 1).node;
            expect_word("then", "Expected THEN after condition");
            branch.body = procedural_body(depth + branches.size() + 1);
            branches.push_back(std::move(branch));
            start = current().source;
        } while (take_word(case_statement ? "when" : "elsif"));

        std::vector<pl::Node> otherwise;
        if (take_word("else")) otherwise = procedural_body(depth + branches.size());
        expect_word("end", "Expected END after conditional statements");
        expect_word(case_statement ? "case" : "if", "Expected matching CASE or IF after END");
        while (!branches.empty())
        {
            auto branch = std::move(branches.back());
            branches.pop_back();
            branch.alternate = std::move(otherwise);
            otherwise.clear();
            otherwise.push_back(std::move(branch));
        }
        return std::move(otherwise.front());
    }

    pl::Node loop(pl::Node node, size_t depth)
    {
        node.body = procedural_body(depth + 1);
        expect_word("end", "Expected END after loop body");
        expect_word("loop", "Expected LOOP after END");
        return node;
    }

    std::vector<Name> procedural_targets()
    {
        std::vector<Name> result;
        do
        {
            if (result.size() == column_limit)
                throw Error(Code::limit, current().source, "PL/SQL target list limit exceeded");
            result.push_back(name(1));
        } while (take(TokenKind::comma));
        return result;
    }

    void procedural_select(pl::Node& node)
    {
        node.kind = pl::Node::Kind::select_into;
        Statement hosted{};
        hosted.source = current().source;
        SelectInto destination;
        query(hosted, &destination);
        node.targets = std::move(destination.targets);
        node.host_sql.assign(input_.substr(hosted.source.offset, destination.begin - hosted.source.offset));
        node.host_sql.append(input_.substr(destination.end, current().source.offset - destination.end));
    }

    void procedural_mutation(pl::Node& node)
    {
        node.kind = pl::Node::Kind::sql_mutation;
        Statement hosted{};
        hosted.source = node.source;
        if (take_word("insert")) insert(hosted);
        else if (take_word("update")) update(hosted);
        else
        {
            expect_word("delete", "Expected DELETE in PL/SQL mutation");
            remove(hosted);
        }
        if (word_is(current(), "returning"))
            throw Error(Code::unsupported, current().source, "DML RETURNING is unavailable");
        node.host_sql.assign(input_.substr(node.source.offset, current().source.offset - node.source.offset));
    }

    void cursor_operation(pl::Node& node)
    {
        using Kind = pl::Node::Kind;
        const auto operation = consume();
        node.name = name(1);
        if (operation.text == "open")
        {
            node.kind = Kind::open_cursor;
            expect_word("for", "Expected FOR after cursor name");
            Statement hosted{};
            hosted.source = current().source;
            query(hosted);
            node.host_sql.assign(input_.substr(hosted.source.offset, current().source.offset - hosted.source.offset));
        }
        else if (operation.text == "fetch")
        {
            node.kind = Kind::fetch_cursor;
            expect_word("into", "Expected INTO after cursor name");
            node.targets = procedural_targets();
        }
        else if (operation.text == "emit")
        {
            node.kind = Kind::emit_cursor;
            expect_word("as", "Expected AS before result set name");
            node.result_name = identifier();
        }
        else
        {
            node.kind = Kind::close_cursor;
        }
    }

    pl::Node procedural_node(size_t depth)
    {
        using Kind = pl::Node::Kind;
        pl::Node node{};
        node.source = current().source;
        if (is_name(current()) && following(TokenKind::assign))
        {
            node.kind = Kind::assign;
            node.name = name(1);
            expect(TokenKind::assign, "Expected := after local variable name");
            node.first = expression(1, 1).node;
        }
        else if (take_word("if"))
        {
            return conditional(node.source, false, depth);
        }
        else if (take_word("case"))
        {
            if (!take_word("when"))
                throw Error(Code::unsupported, current().source, "Only searched CASE WHEN is available");
            return conditional(node.source, true, depth);
        }
        else if (take_word("for"))
        {
            node.kind = Kind::for_;
            node.name = name(1);
            expect_word("in", "Expected IN in FOR declaration");
            if (word_is(current(), "reverse"))
                throw Error(Code::unsupported, current().source, "Reverse FOR loops are unavailable");
            node.first = expression(1, 1).node;
            expect(TokenKind::range, "Expected .. between FOR bounds");
            node.second = expression(1, 1).node;
            expect_word("loop", "Expected LOOP after FOR bounds");
            return loop(std::move(node), depth);
        }
        else if (take_word("while"))
        {
            node.kind = Kind::while_;
            node.first = expression(1, 1).node;
            expect_word("loop", "Expected LOOP after WHILE condition");
            return loop(std::move(node), depth);
        }
        else if (take_word("loop"))
        {
            node.kind = Kind::loop_;
            return loop(std::move(node), depth);
        }
        else if (word_is(current(), "exit") || word_is(current(), "continue"))
        {
            node.kind = take_word("exit") ? Kind::exit_ : Kind::continue_;
            if (node.kind == Kind::continue_) expect_word("continue", "Expected CONTINUE");
            if (take_word("when")) node.first = expression(1, 1).node;
        }
        else if (take_word("return"))
        {
            node.kind = Kind::return_;
            if (current().kind != TokenKind::semicolon) node.first = expression(1, 1).node;
        }
        else if (take_word("raise"))
        {
            node.kind = Kind::raise;
            if (is_name(current())) node.name = name(1);
        }
        else if (word_is(current(), "begin"))
        {
            node.kind = Kind::block;
            block_contents(node.body, node.handlers, depth + 1);
        }
        else if (word_is(current(), "select"))
        {
            procedural_select(node);
        }
        else if (word_is(current(), "insert") || word_is(current(), "update") || word_is(current(), "delete"))
        {
            procedural_mutation(node);
        }
        else if (word_is(current(), "open") || word_is(current(), "fetch") ||
            word_is(current(), "close") || word_is(current(), "emit"))
        {
            cursor_operation(node);
        }
        else if (word_is(current(), "declare"))
        {
            throw Error(Code::unsupported, node.source, "Nested DECLARE blocks are unavailable");
        }
        else
        {
            fail("Expected a supported PL/SQL statement");
        }
        return node;
    }

    uint64_t unsigned_number(uint64_t minimum, uint64_t maximum, const char* range_error)
    {
        if (current().kind != TokenKind::integer) fail("Expected an unsigned integer");
        const auto token = consume();
        uint64_t result{};
        const auto end = token.text.data() + token.text.size();
        const auto parsed = std::from_chars(token.text.data(), end, result);
        if (parsed.ec != std::errc{} || parsed.ptr != end || result < minimum || result > maximum)
            throw Error(Code::range, token.source, range_error);
        return result;
    }

    uint64_t unsigned_history_number(uint64_t minimum = 1, uint64_t maximum = UINT64_MAX)
    {
        return unsigned_number(minimum, maximum, "History number is out of range");
    }

    int64_t history_epoch()
    {
        expect_word("utc", "Expected UTC before epoch seconds");
        const auto source = current().source;
        const bool negative = take(TokenKind::minus);
        if (!negative) take(TokenKind::plus);
        if (current().kind != TokenKind::integer) fail("Expected signed epoch seconds");
        auto token = consume();
        if (negative) token.text.insert(token.text.begin(), '-');
        int64_t result{};
        const auto end = token.text.data() + token.text.size();
        const auto parsed = std::from_chars(token.text.data(), end, result);
        if (parsed.ec != std::errc{} || parsed.ptr != end)
            throw Error(Code::range, source, "History UTC epoch is out of range");
        return result;
    }

    uint64_t history_revision()
    {
        expect_word("revision", "Expected REVISION before History position");
        return unsigned_history_number();
    }

    void history_pagination(HistoryCommand& command)
    {
        if (take_word("offset")) command.offset = static_cast<uint32_t>(unsigned_history_number(0, UINT32_MAX));
        if (take_word("limit")) command.limit = static_cast<uint32_t>(unsigned_history_number(1, history_page_limit));
    }

    void history_log(HistoryCommand& command)
    {
        command.kind = HistoryCommand::Kind::log;
        if (take_word("for")) command.target = name(3);
        if (take_word("row")) command.row_id = unsigned_history_number();
        if (take_word("from")) command.from = history_revision();
        if (take_word("to")) command.to = history_revision();
        if (take_word("since")) command.since_utc = history_epoch();
        if (take_word("until")) command.until_utc = history_epoch();
        if (command.from && command.to && command.from > command.to)
            throw Error(Code::range, current().source, "History revision range is reversed");
        if (command.since_utc && command.until_utc && *command.since_utc > *command.until_utc)
            throw Error(Code::range, current().source, "History UTC range is reversed");
        history_pagination(command);
    }

    void history_read(HistoryCommand& command, bool diff)
    {
        command.kind = diff ? HistoryCommand::Kind::diff : HistoryCommand::Kind::read;
        command.target = name(3);
        if (diff)
        {
            expect_word("from", "Expected FROM before the first diff revision");
            command.from = history_revision();
            expect_word("to", "Expected TO before the second diff revision");
            command.to = history_revision();
            if (command.from > command.to)
                throw Error(Code::range, current().source, "History revision range is reversed");
        }
        else
        {
            expect_word("as", "Expected AS OF for a historical read");
            expect_word("of", "Expected OF after AS in a historical read");
            command.revision = history_revision();
        }
        if (take_word("row")) command.row_id = unsigned_history_number();
        history_pagination(command);
    }

    void history_pin(HistoryCommand& command, bool pin)
    {
        command.kind = pin ? HistoryCommand::Kind::pin : HistoryCommand::Kind::unpin;
        constexpr std::array pin_kinds{"snapshot"sv, "dataset"sv, "backup"sv, "restore"sv};
        if (current().kind != TokenKind::word || !contains_word(pin_kinds, current().text))
            throw Error(Code::unsupported, current().source, "Unsupported History pin kind");
        command.pin_kind = consume().text;
        if (current().kind != TokenKind::quoted_text) fail("Expected a quoted History pin name");
        auto name = consume();
        if (name.text.empty() || name.text.size() > identifier_bytes)
            throw Error(Code::range, name.source, "History pin name must use 1 to 128 UTF-8 bytes");
        command.pin_name = std::move(name.text);
        if (pin)
        {
            expect_word("at", "Expected AT before the pin revision");
            command.revision = history_revision();
        }
    }

    std::string history_argument(const char* expected)
    {
        if (current().kind != TokenKind::quoted_text) fail(expected);
        return consume().text;
    }

    void history_restore(HistoryCommand& command, bool revert)
    {
        command.kind = revert ? HistoryCommand::Kind::revert : HistoryCommand::Kind::restore;
        if (word_is(current(), "apply") && following(TokenKind::quoted_text))
        {
            ++index_;
            const auto source = current().source;
            command.plan_id = history_argument("Expected a quoted History plan ID");
            if (command.plan_id.empty() || command.plan_id.size() > 128)
                throw Error(Code::range, source, "History plan ID size is out of range");
            expect_word("request", "Expected REQUEST before the idempotency token");
            const auto token_source = current().source;
            command.request_token = history_argument("Expected a quoted History request token");
            if (command.request_token.size() != 32 ||
                command.request_token.find_first_not_of("0123456789abcdef") != command.request_token.npos)
                throw Error(Code::syntax, token_source, "History request token must be 32 lowercase hex characters");
            return;
        }
        command.target = name(3);
        if (!revert)
        {
            expect_word("as", "Expected AS OF before the restoration revision");
            expect_word("of", "Expected OF after AS in History restore");
        }
        command.revision = history_revision();
        if (take_word("row")) command.row_id = unsigned_history_number();
        if (take_word("field")) command.field = identifier();
    }

    void history(Statement& statement)
    {
        using Kind = HistoryCommand::Kind;
        statement.action = Action::history;
        auto& command = statement.history.emplace();
        if (take_word("log")) history_log(command);
        else if (take_word("diff")) history_read(command, true);
        else if (take_word("read")) history_read(command, false);
        else if (take_word("verify")) command.kind = Kind::verify;
        else if (take_word("status")) command.kind = Kind::status;
        else if (take_word("pin")) history_pin(command, true);
        else if (take_word("unpin")) history_pin(command, false);
        else if (take_word("restore")) history_restore(command, false);
        else if (take_word("revert")) history_restore(command, true);
        else if (word_is(current(), "retain") || word_is(current(), "purge"))
        {
            const bool retain = take_word("retain");
            if (!retain) expect_word("purge", "Expected PURGE");
            command.kind = retain ? Kind::retain : Kind::purge;
            expect_word("before", "Expected BEFORE in History retention");
            command.revision = history_revision();
            if (retain && take_word("quota")) command.quota_bytes = unsigned_history_number();
        }
        else
        {
            throw Error(Code::unsupported, current().source, "Unsupported History command");
        }
    }

    void permissions(Statement& statement)
    {
        if (take_word("all"))
        {
            take_word("privileges");
            statement.permissions.emplace_back("all");
            if (current().kind == TokenKind::comma)
                throw Error(Code::syntax, current().source, "ALL cannot be combined with another privilege");
            return;
        }
        do
        {
            if (current().kind != TokenKind::word || !contains_word(permission_words, current().text))
                throw Error(Code::unsupported, current().source, "Unsupported SQL privilege");
            auto privilege = consume();
            if (std::ranges::find(statement.permissions, privilege.text) != statement.permissions.end())
                throw Error(Code::syntax, privilege.source, "Repeated SQL privilege");
            statement.permissions.push_back(std::move(privilege.text));
        } while (take(TokenKind::comma));
    }

    void security_object(Statement& statement)
    {
        expect_word("on", "Expected ON after SQL privileges");
        if (current().kind != TokenKind::word || !contains_word(security_kinds, current().text))
            throw Error(Code::unsupported, current().source, "Expected a supported privilege scope");
        statement.security_kind = consume().text;
        if (statement.security_kind == "global") return;
        const auto maximum = statement.security_kind == "database" ? 1 : statement.security_kind == "schema" ? 2 : 3;
        statement.target = name(maximum);
    }

    void grant(Statement& statement, bool granting)
    {
        statement.action = granting ? Action::grant : Action::revoke;
        const auto direction = granting ? "to"sv : "from"sv;
        if (is_name(current()) && following_word(direction))
        {
            statement.related = name(1);
            expect_word(direction, "Expected TO or FROM after role name");
            expect_word("user", "Role membership requires a USER");
        }
        else
        {
            permissions(statement);
            security_object(statement);
            expect_word(direction, "Expected TO or FROM after privilege scope");
            statement.security_role = take_word("role");
            if (!statement.security_role) expect_word("user", "Expected USER or ROLE as privilege recipient");
        }
        statement.security_subject = identifier();
        if (current().kind != TokenKind::end && current().kind != TokenKind::semicolon)
            throw Error(Code::unsupported, current().source, "GRANT and REVOKE options are unavailable");
    }

    void decision_rule(Statement& statement, bool create)
    {
        using Event = DecisionRule::Event;
        using Effect = DecisionRule::Effect;
        expect_word("rule", "Expected RULE after DECISION");
        statement.related = name(1);
        expect_word("on", "Expected ON before the protected table");
        statement.target = name(3);
        auto& rule = statement.decision.emplace();
        if (!create)
        {
            statement.action = Action::activate_decision_rule;
            expect_word("activate", "Expected ACTIVATE for an existing Decision rule");
            expect_word("version", "Expected VERSION after ACTIVATE");
            rule.version = static_cast<uint32_t>(unsigned_number(1, UINT32_MAX, "Decision version is out of range"));
            return;
        }

        statement.action = Action::create_decision_rule;
        expect_word("for", "Expected FOR before the Decision event");
        if (take_word("insert")) rule.event = Event::insert;
        else if (take_word("update")) rule.event = Event::update;
        else if (take_word("delete")) rule.event = Event::delete_;
        else if (take_word("write")) rule.event = Event::write;
        else fail("Expected INSERT, UPDATE, DELETE or WRITE");
        expect_word("priority", "Expected PRIORITY before the Decision condition");
        rule.priority = static_cast<uint32_t>(unsigned_number(0, UINT32_MAX, "Decision priority is out of range"));
        expect_word("when", "Expected WHEN before the Decision condition");
        expect(TokenKind::lparen, "Expected '(' before the Decision condition");
        rule.condition = expression(1, 1).node;
        expect(TokenKind::rparen, "Expected ')' after the Decision condition");
        expect_word("then", "Expected THEN after the Decision condition");
        if (take_word("allow")) rule.effect = Effect::allow;
        else if (take_word("deny")) rule.effect = Effect::deny;
        else
        {
            expect_word("set", "Expected ALLOW, DENY or SET");
            rule.effect = Effect::compute;
            auto column = name(1);
            expect(TokenKind::eq, "Expected '=' after the computed column");
            rule.assignment = Assignment{std::move(column), expression(1, 1).node};
        }
        rule.definition.assign(input_);
    }

    void decision_inspect(Statement& statement)
    {
        using Kind = DecisionInspection::Kind;
        Kind kind;
        if (take_word("explain")) kind = Kind::explain;
        else if (take_word("whynot")) kind = Kind::whynot;
        else if (take_word("whatif")) kind = Kind::whatif;
        else if (take_word("test")) kind = Kind::test;
        else if (take_word("recheck")) kind = Kind::recheck;
        else if (take_word("validate")) kind = Kind::validate;
        else fail("Expected EXPLAIN, WHYNOT, WHATIF, TEST, RECHECK or VALIDATE");
        expect_word("using", "Expected USING before the Decision request");
        if (current().kind != TokenKind::quoted_text) fail("Expected a quoted Decision JSON request");
        statement.action = Action::decision_inspect;
        statement.decision_inspection = DecisionInspection{kind, consume().text};
    }

    void model_request(Statement& statement, ModelCommand::Kind kind, bool after_model)
    {
        if (!after_model) expect_word("model", "Expected MODEL");
        expect_word("using", "Expected USING before the model request");
        if (current().kind != TokenKind::quoted_text) fail("Expected a quoted model JSON request");
        statement.action = Action::model_command;
        statement.model = ModelCommand{kind, consume().text};
    }

    void alter(Statement& statement)
    {
        if (take_word("model"))
        {
            model_request(statement, ModelCommand::Kind::activate, true);
            return;
        }
        if (take_word("decision"))
        {
            decision_rule(statement, false);
            return;
        }
        if (take_word("index"))
        {
            statement.action = Action::rename_index;
            statement.target = name(3);
            if (!take_word("rename"))
                throw Error(Code::unsupported, current().source, "Only ALTER INDEX RENAME TO is available");
            expect_word("to", "Expected TO after RENAME");
            statement.related = name(1);
            return;
        }
        expect_word("table", "Expected TABLE or INDEX after ALTER");
        statement.action = Action::add_column;
        statement.target = name(3);
        if (!take_word("add"))
            throw Error(Code::unsupported, current().source, "Only ALTER TABLE ADD COLUMN is available");
        take_word("column");
        if (constraint_starts(false))
            throw Error(Code::unsupported, current().source, "ALTER TABLE ADD CONSTRAINT is unavailable");
        statement.columns.push_back(column(statement));
    }

    void truncate(Statement& statement)
    {
        statement.action = Action::truncate_table;
        take_word("table");
        statement.target = name(3);
        if (current().kind == TokenKind::word || current().kind == TokenKind::comma)
            throw Error(Code::unsupported, current().source, "TRUNCATE accepts one table without options");
    }

    void explain(Statement& statement)
    {
        statement.action = take_word("analyze") ? Action::explain_analyze : Action::explain;
        if (!word_is(current(), "select") && !word_is(current(), "with"))
            throw Error(Code::unsupported, current().source, "EXPLAIN accepts read queries only");
        statement.query = std::make_shared<Statement>();
        statement.query->source = current().source;
        query(*statement.query);
    }

    void analyze(Statement& statement)
    {
        statement.action = Action::analyze;
        take_word("table");
        statement.target = name(3);
    }

    void reindex(Statement& statement)
    {
        statement.action = Action::reindex;
        take_word("index");
        statement.target = name(3);
        if (take_word("collate")) statement.collation = identifier();
    }

    std::optional<std::string> table_alias(bool implicit)
    {
        if (take_word("as") || (implicit && is_name(current()))) return identifier();
        return {};
    }

    void row_target(Statement& statement, bool implicit_alias = false)
    {
        statement.target = name(3);
        statement.alias = table_alias(implicit_alias);
    }

    void where(Statement& statement)
    {
        if (take_word("where")) statement.filter = expression(1, 1).node;
    }

    std::vector<Expr> expression_list()
    {
        std::vector<Expr> result;
        do
        {
            if (result.size() == column_limit)
                throw Error(Code::limit, current().source, "SQL expression list column limit exceeded");
            result.push_back(expression(1, 1).node);
        } while (take(TokenKind::comma));
        return result;
    }

    std::shared_ptr<Statement> parenthesized_query(std::string* source = nullptr)
    {
        expect(TokenKind::lparen, "Expected opening parenthesis before query");
        auto result = std::make_shared<Statement>();
        result->source = current().source;
        query(*result);
        if (source)
            source->assign(input_.substr(result->source.offset, current().source.offset - result->source.offset));
        expect(TokenKind::rparen, "Expected closing parenthesis after query");
        return result;
    }

    void common_tables(Statement& statement)
    {
        if (word_is(current(), "recursive"))
            throw Error(Code::unsupported, current().source, "Recursive common table expressions are unavailable");
        do
        {
            if (statement.ctes.size() == query_part_limit)
                throw Error(Code::limit, current().source, "Common table expression limit exceeded");
            auto label = identifier();
            if (current().kind == TokenKind::lparen)
                throw Error(Code::unsupported, current().source, "Explicit CTE column lists are unavailable");
            expect_word("as", "Expected AS after CTE name");
            statement.ctes.push_back({std::move(label), parenthesized_query()});
        } while (take(TokenKind::comma));
    }

    void ordering(Statement& statement)
    {
        expect_word("by", "Expected BY after ORDER");
        do
        {
            if (statement.order_by.size() == column_limit)
                throw Error(Code::limit, current().source, "ORDER BY term limit exceeded");
            OrderTerm term{expression(1, 1).node};
            if (take_word("collate")) term.collation = identifier();
            term.descending = take_word("desc");
            if (!term.descending) take_word("asc");
            statement.order_by.push_back(std::move(term));
        } while (take(TokenKind::comma));
    }

    void query(Statement& statement, SelectInto* destination = nullptr)
    {
        if (query_depth_ == query_depth_limit)
            throw Error(Code::limit, current().source, "Nested SQL query limit exceeded");
        ++query_depth_;
        if (take_word("with")) common_tables(statement);
        expect_word("select", "Expected SELECT in read query");
        select(statement, destination);
        while (take_word("union"))
        {
            if (statement.unions.size() == query_part_limit)
                throw Error(Code::limit, current().source, "UNION arm limit exceeded");
            const auto all = take_word("all");
            if (!all) take_word("distinct");
            auto arm = std::make_shared<Statement>();
            arm->source = current().source;
            expect_word("select", "Expected SELECT after UNION");
            select(*arm);
            statement.unions.push_back({all, std::move(arm)});
        }
        if (take_word("order")) ordering(statement);
        --query_depth_;
    }

    std::optional<JoinClause::Kind> join_kind()
    {
        using Join = JoinClause::Kind;
        if (take_word("join")) return Join::inner;
        if (take_word("inner"))
        {
            expect_word("join", "Expected JOIN after INNER");
            return Join::inner;
        }
        if (take_word("left"))
        {
            take_word("outer");
            expect_word("join", "Expected JOIN after LEFT");
            return Join::left;
        }
        if (take_word("cross"))
        {
            expect_word("join", "Expected JOIN after CROSS");
            return Join::cross;
        }
        return {};
    }

    void joins(Statement& statement)
    {
        while (const auto kind = join_kind())
        {
            if (statement.joins.size() == query_part_limit)
                throw Error(Code::limit, current().source, "JOIN limit exceeded");
            if (current().kind == TokenKind::lparen)
                throw Error(Code::unsupported, current().source, "Derived JOIN targets are unavailable");
            JoinClause join{*kind, name(3), table_alias(true), {}};
            if (*kind != JoinClause::Kind::cross)
            {
                expect_word("on", "Expected ON for INNER or LEFT JOIN");
                join.on = expression(1, 1).node;
            }
            else if (word_is(current(), "on"))
            {
                throw Error(Code::syntax, current().source, "CROSS JOIN cannot use an ON predicate");
            }
            statement.joins.push_back(std::move(join));
        }
    }

    void select(Statement& statement, SelectInto* destination = nullptr)
    {
        statement.action = Action::select;
        statement.distinct = take_word("distinct");
        if (!statement.distinct) take_word("all");
        statement.all_columns = take(TokenKind::star);
        if (!statement.all_columns) statement.projection = expression_list();
        if (destination)
        {
            destination->begin = current().source.offset;
            expect_word("into", "SELECT in PL/SQL requires INTO");
            destination->targets = procedural_targets();
            destination->end = current().source.offset;
        }

        if (take_word("from"))
        {
            if (current().kind == TokenKind::lparen)
                throw Error(Code::unsupported, current().source, "Derived tables are unavailable");
            row_target(statement, true);
            if (current().kind == TokenKind::comma)
                throw Error(Code::unsupported, current().source, "Comma-separated table sources are unavailable");
            joins(statement);
        }
        else if (statement.all_columns)
        {
            fail("SELECT * requires a FROM target");
        }
        where(statement);
        if (take_word("group"))
        {
            expect_word("by", "Expected BY after GROUP");
            statement.group_by = expression_list();
        }
        if (take_word("having")) statement.having = expression(1, 1).node;
    }

    void insert(Statement& statement)
    {
        statement.action = Action::insert;
        expect_word("into", "Expected INTO after INSERT");
        statement.target = name(3);
        if (current().kind == TokenKind::lparen) statement.insert_columns = column_names();
        if (word_is(current(), "select"))
            throw Error(Code::unsupported, current().source, "INSERT SELECT is unavailable");
        expect_word("values", "Expected VALUES after INSERT target");
        do
        {
            if (statement.values.size() == insert_row_limit)
                throw Error(Code::limit, current().source, "INSERT row limit exceeded");
            expect(TokenKind::lparen, "Expected opening parenthesis for INSERT row");
            statement.values.push_back(expression_list());
            expect(TokenKind::rparen, "Expected closing parenthesis after INSERT row");
        } while (take(TokenKind::comma));
    }

    void update(Statement& statement)
    {
        statement.action = Action::update;
        row_target(statement);
        expect_word("set", "Expected SET after UPDATE target");
        do
        {
            if (statement.assignments.size() == column_limit)
                throw Error(Code::limit, current().source, "UPDATE assignment limit exceeded");
            auto column = name(1);
            expect(TokenKind::eq, "Expected equals sign in UPDATE assignment");
            statement.assignments.push_back({std::move(column), expression(1, 1).node});
        } while (take(TokenKind::comma));
        if (word_is(current(), "from"))
            throw Error(Code::unsupported, current().source, "UPDATE FROM is unavailable");
        where(statement);
    }

    void remove(Statement& statement)
    {
        statement.action = Action::delete_;
        expect_word("from", "Expected FROM after DELETE");
        row_target(statement);
        where(statement);
    }

    Expression typed_literal(Op op, Source start)
    {
        if (current().kind != TokenKind::quoted_text) fail("Expected a quoted temporal literal");
        return {{op, start, consume().text, {}, {}}};
    }

    Expression number()
    {
        auto token = consume();
        const auto op = token.kind == TokenKind::integer ? Op::integer : Op::decimal;
        return {{op, token.source, std::move(token.text), {}, {}}};
    }

    void check_depth(size_t depth, Source source) const
    {
        if (depth > expression_depth)
            throw Error(Code::limit, source, "SQL expression depth exceeded");
    }

    Expression unary(Op op, Source source, Expression value)
    {
        const auto height = value.height + 1;
        check_depth(height, source);
        Expr node{op, source, {}, {}, {}};
        node.args.push_back(std::move(value.node));
        return {std::move(node), height};
    }

    Expression aggregate(size_t depth)
    {
        auto function = consume();
        expect(TokenKind::lparen, "Expected opening parenthesis for aggregate");
        Expr node{Op::aggregate, function.source, std::move(function.text), {}, {}, {}};
        size_t height = 1;
        if (take(TokenKind::star))
        {
            if (node.text != "count")
                throw Error(Code::syntax, function.source, "Only COUNT accepts a wildcard argument");
        }
        else
        {
            if (word_is(current(), "distinct"))
                throw Error(Code::unsupported, current().source, "DISTINCT inside an aggregate is unavailable");
            auto argument = expression(1, depth + 1);
            height = argument.height + 1;
            check_depth(height, function.source);
            node.args.push_back(std::move(argument.node));
        }
        expect(TokenKind::rparen, "Expected closing parenthesis after aggregate argument");
        return {std::move(node), height};
    }

    Expression function_call(Name name, size_t depth)
    {
        expect(TokenKind::lparen, "Expected opening parenthesis before routine arguments");
        Expr node{Op::routine_call, name.source, {}, std::move(name), {}, {}};
        size_t height = 1;
        if (take(TokenKind::rparen)) return {std::move(node), height};
        do
        {
            if (node.args.size() == routine_variable_limit)
                throw Error(Code::limit, current().source, "Routine argument limit exceeded");
            auto argument = expression(1, depth + 1);
            height = (std::max)(height, argument.height + 1);
            check_depth(height, node.source);
            node.args.push_back(std::move(argument.node));
        } while (take(TokenKind::comma));
        expect(TokenKind::rparen, "Expected closing parenthesis after routine arguments");
        return {std::move(node), height};
    }

    Expression query_operand(Op op, Source start)
    {
        Expr node{op, start, {}, {}, {}, {}};
        node.query = parenthesized_query(&node.text);
        return {std::move(node)};
    }

    Expression primary(unsigned minimum, size_t depth)
    {
        const auto start = current().source;
        check_depth(depth, start);
        if (take_word("exists")) return query_operand(Op::exists, start);
        if (current().kind == TokenKind::lparen && (following_word("select") || following_word("with")))
            return query_operand(Op::subquery, start);
        if (take(TokenKind::lparen))
        {
            auto result = expression(1, depth + 1);
            expect(TokenKind::rparen, "Expected closing parenthesis");
            return result;
        }
        if (current().kind == TokenKind::word && contains_word(aggregate_words, current().text) &&
            following(TokenKind::lparen))
            return aggregate(depth);
        if (minimum <= 3 && take_word("not"))
            return unary(Op::logical_not, start, expression(3, depth + 1));
        if (take(TokenKind::minus))
        {
            if (current().kind == TokenKind::integer || current().kind == TokenKind::decimal)
            {
                auto result = number();
                result.node.source = start;
                result.node.text.insert(result.node.text.begin(), '-');
                return result;
            }
            return unary(Op::negate, start, expression(6, depth + 1));
        }
        if (word_is(current(), "next") && following_word("value"))
        {
            index_ += 2;
            expect_word("for", "Expected FOR after NEXT VALUE");
            return {{Op::next_value, start, {}, name(3), {}}};
        }
        if (take_word("null")) return {{Op::null, start, {}, {}, {}}};
        if (word_is(current(), "true") || word_is(current(), "false"))
            return {{Op::boolean, start, consume().text, {}, {}}};
        if (take_word("date")) return typed_literal(Op::date, start);
        if (take_word("time")) return typed_literal(Op::time, start);
        if (take_word("timestamp"))
        {
            if (take_word("with"))
            {
                expect_word("time", "Expected TIME in TIMESTAMP WITH TIME ZONE");
                expect_word("zone", "Expected ZONE in TIMESTAMP WITH TIME ZONE");
            }
            return typed_literal(Op::timestamp_tz, start);
        }
        if (current().kind == TokenKind::integer || current().kind == TokenKind::decimal) return number();
        if (current().kind == TokenKind::quoted_text)
            return {{Op::text, start, consume().text, {}, {}}};
        if (take(TokenKind::colon))
        {
            const bool row_binding = word_is(current(), "old") || word_is(current(), "new");
            auto variable = identifier();
            if (row_binding && take(TokenKind::dot)) variable += "." + identifier();
            return {{Op::parameter, start, std::move(variable), {}, {}}};
        }
        if (is_name(current()))
        {
            auto target = name(4);
            if (current().kind == TokenKind::lparen)
            {
                if (target.parts.size() > 3) fail("Routine name has too many qualifiers");
                return function_call(std::move(target), depth);
            }
            return {{Op::name, start, {}, std::move(target), {}}};
        }
        fail("Expected SQL expression");
    }

    Expression expression(unsigned minimum, size_t depth)
    {
        auto left = primary(minimum, depth);
        bool compared = false;
        while (const auto operation = infix(current()))
        {
            if (operation->precedence < minimum) break;
            const auto source = consume().source;
            if (operation->precedence == 3 && std::exchange(compared, true))
                throw Error(Code::syntax, source, "A comparison cannot be chained");
            if (operation->null_test)
            {
                const auto op = take_word("not") ? Op::is_not_null : Op::is_null;
                expect_word("null", "Expected NULL in IS test");
                left = unary(op, source, std::move(left));
                continue;
            }
            auto right = expression(operation->precedence + 1, depth + 1);
            const auto height = (std::max)(left.height, right.height) + 1;
            check_depth(height, source);
            Expr node{operation->op, source, {}, {}, {}};
            node.args.push_back(std::move(left.node));
            node.args.push_back(std::move(right.node));
            left = {std::move(node), height};
        }
        return left;
    }

    std::string_view input_;
    std::vector<Token> tokens_;
    size_t index_{};
    size_t query_depth_{};
};
}

Statement parse(std::string_view sql)
{
    return Parser(sql, Lexer(sql).tokenize()).parse();
}

Name parse_history_target(std::string_view target)
{
    return Parser(target, Lexer(target).tokenize()).parse_history_target();
}

}
