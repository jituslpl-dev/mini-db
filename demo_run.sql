CREATE TABLE users (id INTEGER, name VARCHAR, age INTEGER);
INSERT INTO users VALUES (1, 'Alice', 30);
INSERT INTO users VALUES (2, 'Bob', 25);
INSERT INTO users VALUES (3, 'Charlie', 35);
SELECT * FROM users;
SELECT * FROM users WHERE age > 28;
SELECT * FROM users ORDER BY age DESC LIMIT 2;
EXPLAIN SELECT * FROM users WHERE age > 28;
UPDATE users SET age = 26 WHERE id = 2;
SELECT * FROM users;
DELETE FROM users WHERE id = 3;
SELECT * FROM users;
exit