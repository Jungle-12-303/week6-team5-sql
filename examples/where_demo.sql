INSERT INTO users VALUES (1, 'kim', 20);
INSERT INTO users VALUES (2, 'lee', 30);
INSERT INTO users VALUES (3, 'park', 27);
INSERT INTO users VALUES (4, 'lee', 24);
SELECT name, age FROM users WHERE age >= 25;
SELECT id, name FROM users WHERE age >= 25 AND name = 'lee';
