// mini-db - sql_parser.h
//
// A lightweight recursive-descent SQL parser.
//
// Supported statements:
//   CREATE TABLE name (col TYPE, ...);
//   DROP TABLE name;
//   INSERT INTO name VALUES (v, v, ...);
//   DELETE FROM name [WHERE expr];
//   UPDATE name SET col = val [WHERE expr];
//   SELECT cols FROM table [JOIN table ON expr]
//          [WHERE expr] [GROUP BY cols] [ORDER BY cols] [LIMIT n];
//   EXPLAIN <select>;
//
// The parser produces an AST (Abstract Syntax Tree).  The execution engine
// then converts the AST into a physical plan.
//
#pragma once

#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "common/types.h"

namespace minidb {

// ---- Expression AST ----
enum class ExprType {
  COLUMN_REF,    // e.g. users.id
  LITERAL_INT,
  LITERAL_STRING,
  BINARY_OP,     // e.g. a = b, a > b
  UNARY_OP,      // e.g. NOT a
};

enum class OpType {
  EQ, NE, LT, LE, GT, GE, AND, OR, NOT
};

struct Expr {
  ExprType type;

  // For COLUMN_REF: table name (optional) and column name.
  std::string table;
  std::string column;

  // For literals.
  int64_t int_val{0};
  std::string str_val;

  // For BINARY_OP / UNARY_OP.
  OpType op{OpType::EQ};
  std::unique_ptr<Expr> left;
  std::unique_ptr<Expr> right;

  std::string toString() const;
};

// ---- Statement AST ----

struct ColumnDef {
  std::string name;
  TypeId type;
  bool nullable{true};
};

struct CreateTableStmt {
  std::string table_name;
  std::vector<ColumnDef> columns;
};

struct DropTableStmt {
  std::string table_name;
};

struct InsertStmt {
  std::string table_name;
  std::vector<std::vector<Value>> rows;  // support multi-row INSERT
};

struct DeleteStmt {
  std::string table_name;
  std::unique_ptr<Expr> where;
};

struct UpdateStmt {
  std::string table_name;
  std::vector<std::pair<std::string, std::unique_ptr<Expr>>> assignments;
  std::unique_ptr<Expr> where;
};

struct OrderByItem {
  std::string column;
  bool descending{false};
};

struct SelectStmt {
  // SELECT * or explicit column list.
  bool select_all{false};
  std::vector<std::pair<std::string, std::string>> columns;  // (table, column)

  // FROM clause: first table + optional JOIN.
  std::string from_table;
  std::string join_table;
  std::unique_ptr<Expr> join_condition;

  std::unique_ptr<Expr> where;
  std::vector<std::string> group_by;
  std::vector<OrderByItem> order_by;
  int64_t limit{-1};  // -1 = no limit

  bool is_explain{false};  // EXPLAIN SELECT ...
};

// Top-level statement variant.
struct SQLStatement {
  enum class Kind {
    CREATE_TABLE,
    DROP_TABLE,
    INSERT,
    DELETE,
    UPDATE,
    SELECT,
    EXPLAIN,
    UNKNOWN
  } kind{Kind::UNKNOWN};

  std::variant<std::monostate, CreateTableStmt, DropTableStmt, InsertStmt,
               DeleteStmt, UpdateStmt, SelectStmt>
      data;

  std::string toString() const;
};

// ---- Tokenizer ----
enum class TokenType {
  IDENTIFIER,
  NUMBER,
  STRING,
  KEYWORD,
  SYMBOL,
  END_OF_INPUT
};

struct Token {
  TokenType type;
  std::string value;
};

// ---- Parser ----
class SQLParser {
 public:
  explicit SQLParser(const std::string& sql);

  // Parse the SQL string into a statement.  Returns nullptr on error.
  std::unique_ptr<SQLStatement> parse();

  const std::string& getError() const { return error_; }

 private:
  // Tokenizer.
  std::vector<Token> tokenize();
  Token nextToken();

  // Parser helpers.
  bool matchKeyword(const std::string& kw);
  bool matchSymbol(const std::string& sym);
  Token expectIdentifier();
  Token expectSymbol(const std::string& sym);
  bool isKeyword(const std::string& word) const;

  // Expression parsing (precedence climbing).
  std::unique_ptr<Expr> parseExpr();
  std::unique_ptr<Expr> parseOrExpr();
  std::unique_ptr<Expr> parseAndExpr();
  std::unique_ptr<Expr> parseComparison();
  std::unique_ptr<Expr> parsePrimary();

  // Statement parsers.
  std::unique_ptr<SQLStatement> parseCreateTable();
  std::unique_ptr<SQLStatement> parseDropTable();
  std::unique_ptr<SQLStatement> parseInsert();
  std::unique_ptr<SQLStatement> parseDelete();
  std::unique_ptr<SQLStatement> parseUpdate();
  std::unique_ptr<SQLStatement> parseSelect(bool is_explain = false);

  std::string sql_;
  std::vector<Token> tokens_;
  size_t pos_{0};
  std::string error_;
};

}  // namespace minidb