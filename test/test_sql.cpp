// test_sql.cpp - End-to-end SQL tests (parse + execute)
#include <cassert>
#include <filesystem>
#include <iostream>

#include "catalog/catalog.h"
#include "execution/planner.h"
#include "parser/sql_parser.h"
#include "storage/buffer_pool.h"

using namespace minidb;

static ExecutionResult execSQL(Planner& planner, const std::string& sql) {
  SQLParser parser(sql);
  auto stmt = parser.parse();
  assert(stmt != nullptr);
  return planner.execute(*stmt);
}

void testCreateInsertSelect() {
  std::string db_file = "test_sql1.db";
  std::filesystem::remove(db_file);

  BufferPoolManager bpm(db_file, 100, 2);
  Catalog catalog(&bpm);
  Planner planner(&catalog);

  // CREATE TABLE
  auto r = execSQL(planner, "CREATE TABLE users (id INTEGER, name VARCHAR, age INTEGER);");
  assert(r.success);

  // INSERT
  r = execSQL(planner, "INSERT INTO users VALUES (1, 'Alice', 30);");
  assert(r.success);
  r = execSQL(planner, "INSERT INTO users VALUES (2, 'Bob', 25);");
  assert(r.success);
  r = execSQL(planner, "INSERT INTO users VALUES (3, 'Charlie', 35);");
  assert(r.success);

  // SELECT *
  r = execSQL(planner, "SELECT * FROM users;");
  assert(r.success);
  assert(r.rows.size() == 3);
  assert(r.rows[0][1].varchar == "Alice");

  // SELECT with WHERE
  r = execSQL(planner, "SELECT * FROM users WHERE age > 28;");
  assert(r.success);
  assert(r.rows.size() == 2);  // Alice(30) and Charlie(35)

  // SELECT with ORDER BY
  r = execSQL(planner, "SELECT * FROM users ORDER BY age DESC;");
  assert(r.success);
  assert(r.rows.size() == 3);
  assert(r.rows[0][1].varchar == "Charlie");  // age 35

  // SELECT with LIMIT
  r = execSQL(planner, "SELECT * FROM users LIMIT 2;");
  assert(r.success);
  assert(r.rows.size() == 2);

  // DELETE
  r = execSQL(planner, "DELETE FROM users WHERE id = 2;");
  assert(r.success);
  r = execSQL(planner, "SELECT * FROM users;");
  assert(r.rows.size() == 2);

  // UPDATE
  r = execSQL(planner, "UPDATE users SET age = 31 WHERE id = 1;");
  assert(r.success);
  r = execSQL(planner, "SELECT * FROM users WHERE id = 1;");
  assert(r.rows.size() == 1);
  assert(r.rows[0][2].integer == 31);

  // GROUP BY
  r = execSQL(planner, "SELECT * FROM users GROUP BY age;");
  assert(r.success);

  bpm.shutdown();
  std::filesystem::remove(db_file);
  std::cout << "[PASS] testCreateInsertSelect\n";
}

void testJoin() {
  std::string db_file = "test_sql2.db";
  std::filesystem::remove(db_file);

  BufferPoolManager bpm(db_file, 100, 2);
  Catalog catalog(&bpm);
  Planner planner(&catalog);

  execSQL(planner, "CREATE TABLE dept (id INTEGER, name VARCHAR);");
  execSQL(planner, "CREATE TABLE emp (id INTEGER, dept_id INTEGER, name VARCHAR);");

  execSQL(planner, "INSERT INTO dept VALUES (1, 'Engineering');");
  execSQL(planner, "INSERT INTO dept VALUES (2, 'Sales');");
  execSQL(planner, "INSERT INTO emp VALUES (1, 1, 'Alice');");
  execSQL(planner, "INSERT INTO emp VALUES (2, 1, 'Bob');");
  execSQL(planner, "INSERT INTO emp VALUES (3, 2, 'Charlie');");

  auto r = execSQL(planner, "SELECT * FROM emp JOIN dept ON emp.dept_id = dept.id;");
  assert(r.success);
  assert(r.rows.size() == 3);

  bpm.shutdown();
  std::filesystem::remove(db_file);
  std::cout << "[PASS] testJoin\n";
}

void testParser() {
  // Test that the parser handles various constructs.
  SQLParser p1("SELECT * FROM t WHERE a = 1 AND b > 2;");
  assert(p1.parse() != nullptr);

  SQLParser p2("INSERT INTO t VALUES (1, 'hello', 42);");
  assert(p2.parse() != nullptr);

  SQLParser p3("CREATE TABLE t (a INTEGER, b VARCHAR);");
  assert(p3.parse() != nullptr);

  SQLParser p4("DELETE FROM t WHERE a = 1;");
  assert(p4.parse() != nullptr);

  SQLParser p5("UPDATE t SET a = 5 WHERE b = 'x';");
  assert(p5.parse() != nullptr);

  SQLParser p6("EXPLAIN SELECT * FROM t;");
  assert(p6.parse() != nullptr);

  SQLParser p7("SELECT id, name FROM users ORDER BY name ASC LIMIT 10;");
  assert(p7.parse() != nullptr);

  std::cout << "[PASS] testParser\n";
}

int main() {
  testParser();
  testCreateInsertSelect();
  testJoin();
  std::cout << "All SQL tests passed!\n";
  return 0;
}