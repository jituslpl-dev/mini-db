-- mini-db Demo SQL Script
-- Run with: ./minidb_cli < examples/demo.sql
-- Or paste into the interactive shell.

-- Create tables
CREATE TABLE departments (id INTEGER, name VARCHAR);
CREATE TABLE employees (id INTEGER, dept_id INTEGER, name VARCHAR, salary INTEGER);

-- Insert data
INSERT INTO departments VALUES (1, 'Engineering');
INSERT INTO departments VALUES (2, 'Sales');
INSERT INTO departments VALUES (3, 'Marketing');

INSERT INTO employees VALUES (1, 1, 'Alice', 95000);
INSERT INTO employees VALUES (2, 1, 'Bob', 87000);
INSERT INTO employees VALUES (3, 2, 'Charlie', 72000);
INSERT INTO employees VALUES (4, 2, 'Diana', 68000);
INSERT INTO employees VALUES (5, 3, 'Eve', 81000);
INSERT INTO employees VALUES (6, 1, 'Frank', 91000);

-- Simple queries
SELECT * FROM employees;
SELECT * FROM employees WHERE salary > 80000;
SELECT * FROM employees ORDER BY salary DESC LIMIT 3;

-- Join
SELECT * FROM employees JOIN departments ON employees.dept_id = departments.id;

-- Group by
SELECT * FROM employees GROUP BY dept_id;

-- Update
UPDATE employees SET salary = 100000 WHERE id = 1;
SELECT * FROM employees WHERE id = 1;

-- Delete
DELETE FROM employees WHERE id = 4;
SELECT * FROM employees;

-- Explain
EXPLAIN SELECT * FROM employees WHERE salary > 80000;