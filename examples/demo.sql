CREATE TABLE items (id INT PRIMARY KEY, value TEXT);
INSERT INTO items VALUES (1, 'hello');
BEGIN;
UPDATE items SET value = 'updated' WHERE id = 1;
COMMIT;
SELECT * FROM items WHERE id = 1;
