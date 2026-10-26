// mini-db - sql_parser.cpp
#include "parser/sql_parser.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace minidb {

// ---- Expr toString ----
static std::string opToString(OpType op) {
  switch (op) {
    case OpType::EQ: return "=";
    case OpType::NE: return "!=";
    case OpType::LT: return "<";
    case OpType::LE: return "<=";
    case OpType::GT: return ">";
    case OpType::GE: return ">=";
    case OpType::AND: return "AND";
    case OpType::OR: return "OR";
    case OpType::NOT: return "NOT";
  }
  return "?";
}

std::string Expr::toString() const {
  switch (type) {
    case ExprType::COLUMN_REF:
      return table.empty() ? column : (table + "." + column);
    case ExprType::LITERAL_INT:
      return std::to_string(int_val);
    case ExprType::LITERAL_STRING:
      return "'" + str_val + "'";
    case ExprType::BINARY_OP:
      return "(" + (left ? left->toString() : "") + " " +
             opToString(op) + " " +
             (right ? right->toString() : "") + ")";
    case ExprType::UNARY_OP:
      return opToString(op) + " " + (left ? left->toString() : "");
  }
  return "?";
}

std::string SQLStatement::toString() const {
  switch (kind) {
    case Kind::CREATE_TABLE: {
      const auto& s = std::get<CreateTableStmt>(data);
      std::string out = "CREATE TABLE " + s.table_name + " (";
      for (size_t i = 0; i < s.columns.size(); ++i) {
        if (i > 0) out += ", ";
        out += s.columns[i].name;
        out += s.columns[i].type == TypeId::INTEGER ? " INTEGER" : " VARCHAR";
      }
      out += ")";
      return out;
    }
    case Kind::DROP_TABLE: {
      const auto& s = std::get<DropTableStmt>(data);
      return "DROP TABLE " + s.table_name;
    }
    case Kind::INSERT: {
      const auto& s = std::get<InsertStmt>(data);
      return "INSERT INTO " + s.table_name + " VALUES (...)";
    }
    case Kind::DELETE: {
      const auto& s = std::get<DeleteStmt>(data);
      std::string out = "DELETE FROM " + s.table_name;
      if (s.where) out += " WHERE " + s.where->toString();
      return out;
    }
    case Kind::UPDATE: {
      const auto& s = std::get<UpdateStmt>(data);
      std::string out = "UPDATE " + s.table_name + " SET ...";
      if (s.where) out += " WHERE " + s.where->toString();
      return out;
    }
    case Kind::SELECT:
    case Kind::EXPLAIN: {
      const auto& s = std::get<SelectStmt>(data);
      std::string out = "SELECT ";
      if (s.select_all) {
        out += "*";
      } else {
        for (size_t i = 0; i < s.columns.size(); ++i) {
          if (i > 0) out += ", ";
          out += s.columns[i].first.empty()
                     ? s.columns[i].second
                     : s.columns[i].first + "." + s.columns[i].second;
        }
      }
      out += " FROM " + s.from_table;
      if (!s.join_table.empty()) {
        out += " JOIN " + s.join_table + " ON " +
               (s.join_condition ? s.join_condition->toString() : "");
      }
      if (s.where) out += " WHERE " + s.where->toString();
      if (!s.group_by.empty()) {
        out += " GROUP BY ";
        for (size_t i = 0; i < s.group_by.size(); ++i) {
          if (i > 0) out += ", ";
          out += s.group_by[i];
        }
      }
      if (!s.order_by.empty()) {
        out += " ORDER BY ";
        for (size_t i = 0; i < s.order_by.size(); ++i) {
          if (i > 0) out += ", ";
          out += s.order_by[i].column;
          if (s.order_by[i].descending) out += " DESC";
        }
      }
      if (s.limit >= 0) out += " LIMIT " + std::to_string(s.limit);
      return out;
    }
    case Kind::UNKNOWN:
      return "UNKNOWN";
  }
  return "";
}

// ---- Tokenizer ----
static const std::set<std::string> KEYWORDS = {
    "create", "table", "drop", "insert", "into", "values",
    "delete", "from", "update", "set", "select", "where",
    "order", "by", "group", "limit", "join", "inner", "on",
    "and", "or", "not", "integer", "varchar", "text",
    "explain", "desc", "asc", "null", "not_null"};

bool SQLParser::isKeyword(const std::string& word) const {
  std::string lower = word;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
  return KEYWORDS.count(lower) > 0;
}

SQLParser::SQLParser(const std::string& sql) : sql_(sql) {
  tokens_ = tokenize();
}

std::vector<Token> SQLParser::tokenize() {
  std::vector<Token> tokens;
  size_t i = 0;
  while (i < sql_.size()) {
    char c = sql_[i];
    // Skip whitespace.
    if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }

    // Single-line comment: --
    if (c == '-' && i + 1 < sql_.size() && sql_[i + 1] == '-') {
      while (i < sql_.size() && sql_[i] != '\n') ++i;
      continue;
    }

    // Identifier or keyword.
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      size_t start = i;
      while (i < sql_.size() &&
             (std::isalnum(static_cast<unsigned char>(sql_[i])) ||
              sql_[i] == '_')) {
        ++i;
      }
      std::string word = sql_.substr(start, i - start);
      std::string lower = word;
      std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
      if (isKeyword(word)) {
        tokens.push_back({TokenType::KEYWORD, lower});
      } else {
        tokens.push_back({TokenType::IDENTIFIER, word});
      }
      continue;
    }

    // Number.
    if (std::isdigit(static_cast<unsigned char>(c)) ||
        (c == '-' && i + 1 < sql_.size() &&
         std::isdigit(static_cast<unsigned char>(sql_[i + 1])))) {
      size_t start = i;
      if (c == '-') ++i;
      while (i < sql_.size() &&
             std::isdigit(static_cast<unsigned char>(sql_[i]))) {
        ++i;
      }
      tokens.push_back({TokenType::NUMBER, sql_.substr(start, i - start)});
      continue;
    }

    // String literal.
    if (c == '\'') {
      ++i;  // skip opening quote
      size_t start = i;
      while (i < sql_.size() && sql_[i] != '\'') ++i;
      std::string str = sql_.substr(start, i - start);
      if (i < sql_.size()) ++i;  // skip closing quote
      tokens.push_back({TokenType::STRING, str});
      continue;
    }

    // Two-character symbols.
    if (i + 1 < sql_.size()) {
      std::string two = sql_.substr(i, 2);
      if (two == "!=" || two == "<=" || two == ">=") {
        tokens.push_back({TokenType::SYMBOL, two});
        i += 2;
        continue;
      }
    }

    // Single-character symbols.
    if (c == '(' || c == ')' || c == ',' || c == ';' || c == '*' ||
        c == '=' || c == '<' || '>' || c == '.') {
      tokens.push_back({TokenType::SYMBOL, std::string(1, c)});
      ++i;
      continue;
    }

    // Unknown character: skip.
    ++i;
  }
  tokens.push_back({TokenType::END_OF_INPUT, ""});
  return tokens;
}

// ---- Parser helpers ----
Token SQLParser::nextToken() {
  if (pos_ < tokens_.size()) return tokens_[pos_++];
  return {TokenType::END_OF_INPUT, ""};
}

bool SQLParser::matchKeyword(const std::string& kw) {
  if (pos_ < tokens_.size() && tokens_[pos_].type == TokenType::KEYWORD &&
      tokens_[pos_].value == kw) {
    ++pos_;
    return true;
  }
  return false;
}

bool SQLParser::matchSymbol(const std::string& sym) {
  if (pos_ < tokens_.size() && tokens_[pos_].type == TokenType::SYMBOL &&
      tokens_[pos_].value == sym) {
    ++pos_;
    return true;
  }
  return false;
}

Token SQLParser::expectIdentifier() {
  if (pos_ < tokens_.size() && tokens_[pos_].type == TokenType::IDENTIFIER) {
    return tokens_[pos_++];
  }
  error_ = "Expected identifier at position " + std::to_string(pos_);
  return {TokenType::END_OF_INPUT, ""};
}

Token SQLParser::expectSymbol(const std::string& sym) {
  if (pos_ < tokens_.size() && tokens_[pos_].type == TokenType::SYMBOL &&
      tokens_[pos_].value == sym) {
    return tokens_[pos_++];
  }
  error_ = "Expected '" + sym + "' at position " + std::to_string(pos_);
  return {TokenType::END_OF_INPUT, ""};
}

// ---- Expression parsing (precedence climbing) ----
std::unique_ptr<Expr> SQLParser::parseExpr() { return parseOrExpr(); }

std::unique_ptr<Expr> SQLParser::parseOrExpr() {
  auto left = parseAndExpr();
  while (matchKeyword("or")) {
    auto right = parseAndExpr();
    auto node = std::make_unique<Expr>();
    node->type = ExprType::BINARY_OP;
    node->op = OpType::OR;
    node->left = std::move(left);
    node->right = std::move(right);
    left = std::move(node);
  }
  return left;
}

std::unique_ptr<Expr> SQLParser::parseAndExpr() {
  auto left = parseComparison();
  while (matchKeyword("and")) {
    auto right = parseComparison();
    auto node = std::make_unique<Expr>();
    node->type = ExprType::BINARY_OP;
    node->op = OpType::AND;
    node->left = std::move(left);
    node->right = std::move(right);
    left = std::move(node);
  }
  return left;
}

std::unique_ptr<Expr> SQLParser::parseComparison() {
  auto left = parsePrimary();

  if (pos_ < tokens_.size() && tokens_[pos_].type == TokenType::SYMBOL) {
    OpType op;
    std::string sym = tokens_[pos_].value;
    if (sym == "=") op = OpType::EQ;
    else if (sym == "!=") op = OpType::NE;
    else if (sym == "<") op = OpType::LT;
    else if (sym == "<=") op = OpType::LE;
    else if (sym == ">") op = OpType::GT;
    else if (sym == ">=") op = OpType::GE;
    else return left;  // not a comparison
    ++pos_;
    auto right = parsePrimary();
    auto node = std::make_unique<Expr>();
    node->type = ExprType::BINARY_OP;
    node->op = op;
    node->left = std::move(left);
    node->right = std::move(right);
    return node;
  }
  return left;
}

std::unique_ptr<Expr> SQLParser::parsePrimary() {
  if (pos_ >= tokens_.size()) return nullptr;

  // Parenthesized expression.
  if (tokens_[pos_].type == TokenType::SYMBOL && tokens_[pos_].value == "(") {
    ++pos_;
    auto expr = parseExpr();
    expectSymbol(")");
    return expr;
  }

  // NOT
  if (tokens_[pos_].type == TokenType::KEYWORD && tokens_[pos_].value == "not") {
    ++pos_;
    auto child = parsePrimary();
    auto node = std::make_unique<Expr>();
    node->type = ExprType::UNARY_OP;
    node->op = OpType::NOT;
    node->left = std::move(child);
    return node;
  }

  // Number literal.
  if (tokens_[pos_].type == TokenType::NUMBER) {
    auto node = std::make_unique<Expr>();
    node->type = ExprType::LITERAL_INT;
    node->int_val = std::stoll(tokens_[pos_].value);
    ++pos_;
    return node;
  }

  // String literal.
  if (tokens_[pos_].type == TokenType::STRING) {
    auto node = std::make_unique<Expr>();
    node->type = ExprType::LITERAL_STRING;
    node->str_val = tokens_[pos_].value;
    ++pos_;
    return node;
  }

  // Column reference: identifier or identifier.identifier
  if (tokens_[pos_].type == TokenType::IDENTIFIER) {
    auto node = std::make_unique<Expr>();
    node->type = ExprType::COLUMN_REF;
    node->table = tokens_[pos_].value;
    ++pos_;
    if (pos_ < tokens_.size() && tokens_[pos_].type == TokenType::SYMBOL &&
        tokens_[pos_].value == ".") {
      ++pos_;
      if (pos_ < tokens_.size() && tokens_[pos_].type == TokenType::IDENTIFIER) {
        node->column = tokens_[pos_].value;
        ++pos_;
      }
      // Handle table.*
      if (pos_ < tokens_.size() && tokens_[pos_].type == TokenType::SYMBOL &&
          tokens_[pos_].value == "*") {
        node->column = "*";
        ++pos_;
      }
    } else {
      // No dot: the identifier is the column name.
      node->column = node->table;
      node->table = "";
    }
    return node;
  }

  error_ = "Unexpected token: " + tokens_[pos_].value;
  return nullptr;
}

// ---- Statement parsers ----
std::unique_ptr<SQLStatement> SQLParser::parse() {
  if (pos_ >= tokens_.size() || tokens_[pos_].type == TokenType::END_OF_INPUT) {
    error_ = "Empty input";
    return nullptr;
  }

  if (tokens_[pos_].type == TokenType::KEYWORD) {
    const std::string& kw = tokens_[pos_].value;
    if (kw == "create") return parseCreateTable();
    if (kw == "drop") return parseDropTable();
    if (kw == "insert") return parseInsert();
    if (kw == "delete") return parseDelete();
    if (kw == "update") return parseUpdate();
    if (kw == "select") return parseSelect();
    if (kw == "explain") {
      ++pos_;
      if (matchKeyword("select")) {
        --pos_;  // re-consume 'select'
        return parseSelect(true);
      }
      error_ = "Expected SELECT after EXPLAIN";
      return nullptr;
    }
  }
  error_ = "Unknown statement";
  return nullptr;
}

std::unique_ptr<SQLStatement> SQLParser::parseCreateTable() {
  matchKeyword("create");
  matchKeyword("table");

  auto stmt = std::make_unique<SQLStatement>();
  stmt->kind = SQLStatement::Kind::CREATE_TABLE;
  auto& s = stmt->data.emplace<CreateTableStmt>();

  s.table_name = expectIdentifier().value;
  expectSymbol("(");

  while (true) {
    ColumnDef col;
    col.name = expectIdentifier().value;
    if (matchKeyword("integer")) {
      col.type = TypeId::INTEGER;
    } else if (matchKeyword("varchar") || matchKeyword("text")) {
      col.type = TypeId::VARCHAR;
    } else {
      error_ = "Expected column type";
      return nullptr;
    }
    col.nullable = !matchKeyword("not_null");
    s.columns.push_back(col);

    if (matchSymbol(",")) continue;
    break;
  }
  expectSymbol(")");
  matchSymbol(";");

  return stmt;
}

std::unique_ptr<SQLStatement> SQLParser::parseDropTable() {
  matchKeyword("drop");
  matchKeyword("table");

  auto stmt = std::make_unique<SQLStatement>();
  stmt->kind = SQLStatement::Kind::DROP_TABLE;
  auto& s = stmt->data.emplace<DropTableStmt>();
  s.table_name = expectIdentifier().value;
  matchSymbol(";");
  return stmt;
}

std::unique_ptr<SQLStatement> SQLParser::parseInsert() {
  matchKeyword("insert");
  matchKeyword("into");

  auto stmt = std::make_unique<SQLStatement>();
  stmt->kind = SQLStatement::Kind::INSERT;
  auto& s = stmt->data.emplace<InsertStmt>();
  s.table_name = expectIdentifier().value;

  // Optional column list (we ignore it for simplicity).
  if (matchSymbol("(")) {
    while (!matchSymbol(")")) {
      expectIdentifier();
      matchSymbol(",");
    }
  }

  matchKeyword("values");

  while (true) {
    expectSymbol("(");
    std::vector<Value> row;
    while (true) {
      if (tokens_[pos_].type == TokenType::NUMBER) {
        row.push_back(Value::makeInteger(std::stoll(tokens_[pos_].value)));
        ++pos_;
      } else if (tokens_[pos_].type == TokenType::STRING) {
        row.push_back(Value::makeVarchar(tokens_[pos_].value));
        ++pos_;
      } else {
        error_ = "Expected value in VALUES";
        return nullptr;
      }
      if (matchSymbol(",")) continue;
      break;
    }
    expectSymbol(")");
    s.rows.push_back(std::move(row));
    if (matchSymbol(",")) continue;
    break;
  }
  matchSymbol(";");
  return stmt;
}

std::unique_ptr<SQLStatement> SQLParser::parseDelete() {
  matchKeyword("delete");
  matchKeyword("from");

  auto stmt = std::make_unique<SQLStatement>();
  stmt->kind = SQLStatement::Kind::DELETE;
  auto& s = stmt->data.emplace<DeleteStmt>();
  s.table_name = expectIdentifier().value;

  if (matchKeyword("where")) {
    s.where = parseExpr();
  }
  matchSymbol(";");
  return stmt;
}

std::unique_ptr<SQLStatement> SQLParser::parseUpdate() {
  matchKeyword("update");

  auto stmt = std::make_unique<SQLStatement>();
  stmt->kind = SQLStatement::Kind::UPDATE;
  auto& s = stmt->data.emplace<UpdateStmt>();
  s.table_name = expectIdentifier().value;
  matchKeyword("set");

  while (true) {
    std::string col = expectIdentifier().value;
    expectSymbol("=");
    auto val = parseExpr();
    s.assignments.emplace_back(col, std::move(val));
    if (matchSymbol(",")) continue;
    break;
  }

  if (matchKeyword("where")) {
    s.where = parseExpr();
  }
  matchSymbol(";");
  return stmt;
}

std::unique_ptr<SQLStatement> SQLParser::parseSelect(bool is_explain) {
  matchKeyword("select");

  auto stmt = std::make_unique<SQLStatement>();
  stmt->kind = is_explain ? SQLStatement::Kind::EXPLAIN
                          : SQLStatement::Kind::SELECT;
  auto& s = stmt->data.emplace<SelectStmt>();
  s.is_explain = is_explain;

  // Parse column list or *.
  if (matchSymbol("*")) {
    s.select_all = true;
  } else {
    while (true) {
      std::string first = expectIdentifier().value;
      std::string second;
      if (matchSymbol(".")) {
        second = expectIdentifier().value;
        s.columns.emplace_back(first, second);
      } else {
        s.columns.emplace_back("", first);
      }
      if (matchSymbol(",")) continue;
      break;
    }
  }

  matchKeyword("from");
  s.from_table = expectIdentifier().value;

  // Optional INNER JOIN.
  bool has_join = false;
  if (matchKeyword("inner")) {
    matchKeyword("join");
    has_join = true;
  } else if (matchKeyword("join")) {
    has_join = true;
  }

  if (has_join) {
    s.join_table = expectIdentifier().value;
    matchKeyword("on");
    s.join_condition = parseExpr();
  }

  if (matchKeyword("where")) {
    s.where = parseExpr();
  }

  if (matchKeyword("group")) {
    matchKeyword("by");
    while (true) {
      s.group_by.push_back(expectIdentifier().value);
      if (matchSymbol(",")) continue;
      break;
    }
  }

  if (matchKeyword("order")) {
    matchKeyword("by");
    while (true) {
      OrderByItem item;
      item.column = expectIdentifier().value;
      if (matchKeyword("desc")) item.descending = true;
      else matchKeyword("asc");
      s.order_by.push_back(item);
      if (matchSymbol(",")) continue;
      break;
    }
  }

  if (matchKeyword("limit")) {
    if (pos_ < tokens_.size() && tokens_[pos_].type == TokenType::NUMBER) {
      s.limit = std::stoll(tokens_[pos_].value);
      ++pos_;
    }
  }

  matchSymbol(";");
  return stmt;
}

}  // namespace minidb