// bench_insert.cpp - Benchmark: bulk insert and scan performance
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

#include "catalog/catalog.h"
#include "execution/planner.h"
#include "parser/sql_parser.h"
#include "storage/buffer_pool.h"

using namespace minidb;

int main() {
  std::string db_file = "bench.db";
  std::filesystem::remove(db_file);

  BufferPoolManager bpm(db_file, 1024, 2);
  Catalog catalog(&bpm);
  Planner planner(&catalog);

  // Create table.
  SQLParser p("CREATE TABLE bench (id INTEGER, val INTEGER);");
  auto stmt = p.parse();
  planner.execute(*stmt);

  // Bulk insert.
  const int N = 10000;
  auto start = std::chrono::high_resolution_clock::now();

  for (int i = 0; i < N; ++i) {
    std::string sql = "INSERT INTO bench VALUES (" + std::to_string(i) + ", " +
                      std::to_string(i * 2) + ");";
    SQLParser parser(sql);
    auto s = parser.parse();
    planner.execute(*s);
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  std::cout << "Insert " << N << " rows: " << ms << " ms ("
            << (N * 1000.0 / ms) << " ops/s)\n";

  // Scan.
  start = std::chrono::high_resolution_clock::now();
  SQLParser sp("SELECT * FROM bench;");
  auto ss = sp.parse();
  auto result = planner.execute(*ss);
  end = std::chrono::high_resolution_clock::now();
  ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  std::cout << "Scan " << result.rows.size() << " rows: " << ms << " ms\n";

  // Scan with filter.
  start = std::chrono::high_resolution_clock::now();
  SQLParser fp("SELECT * FROM bench WHERE val > 5000;");
  auto fs = fp.parse();
  result = planner.execute(*fs);
  end = std::chrono::high_resolution_clock::now();
  ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  std::cout << "Filtered scan (" << result.rows.size() << " rows): " << ms
            << " ms\n";

  // Sort.
  start = std::chrono::high_resolution_clock::now();
  SQLParser op("SELECT * FROM bench ORDER BY val DESC LIMIT 10;");
  auto os = op.parse();
  result = planner.execute(*os);
  end = std::chrono::high_resolution_clock::now();
  ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  std::cout << "Sort + limit (" << result.rows.size() << " rows): " << ms
            << " ms\n";

  bpm.shutdown();
  std::filesystem::remove(db_file);
  return 0;
}