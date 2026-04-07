INSERT INTO users (id, name, age) VALUES (1, 'kim', 20);
INSERT INTO users (id, name, age) VALUES (2, 'lee', 30);
INSERT INTO users (id, name, age) VALUES (3, 'park', 40);
CREATE INDEX idx_users_age ON users(age);
SELECT id, name, age FROM users WHERE age >= 30 AND id >= 2;
