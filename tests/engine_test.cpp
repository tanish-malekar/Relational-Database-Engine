#include "sqldb/engine.hpp"
#include <future>
#include <gtest/gtest.h>
#include <unistd.h>
namespace {
struct Database : testing::Test {
  std::filesystem::path path;
  void SetUp() override {
    std::string pattern = (std::filesystem::temp_directory_path() / "sql-unit-XXXXXX").string();
    auto *created = ::mkdtemp(pattern.data());
    ASSERT_NE(created, nullptr);
    path = created;
  }
  void TearDown() override { std::filesystem::remove_all(path); }
};
TEST_F(Database, SnapshotRetainsOldHeapAndIndexAfterConcurrentDelete) {
  sqldb::Engine db(path);
  db.execute("CREATE TABLE items (id INT, value TEXT)");
  db.execute("INSERT INTO items VALUES (1, 'old')");
  std::promise<void> ready, changed;
  auto signal = changed.get_future();
  auto reader = std::async(std::launch::async, [&] {
    auto tx = db.begin();
    ready.set_value();
    signal.wait();
    auto row = tx.execute("SELECT * FROM items WHERE id = 1");
    tx.commit();
    return row.rows;
  });
  ready.get_future().wait();
  db.execute("DELETE FROM items WHERE id = 1");
  changed.set_value();
  auto rows = reader.get();
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(sqldb::display(rows[0][1]), "old");
  EXPECT_TRUE(db.execute("SELECT * FROM items WHERE id = 1").rows.empty());
}
TEST_F(Database, FirstCommitterWinsAndConflictDoesNotPoisonDatabase) {
  sqldb::Engine db(path);
  db.execute("CREATE TABLE items (id INT, value TEXT)");
  db.execute("INSERT INTO items VALUES (1, 'old')");
  std::promise<void> ready, changed;
  auto signal = changed.get_future();
  auto writer = std::async(std::launch::async, [&] {
    auto tx = db.begin();
    tx.execute("UPDATE items SET value = 'loser' WHERE id = 1");
    ready.set_value();
    signal.wait();
    EXPECT_THROW(tx.commit(), sqldb::WriteConflict);
  });
  ready.get_future().wait();
  db.execute("UPDATE items SET value = 'winner' WHERE id = 1");
  changed.set_value();
  writer.get();
  EXPECT_EQ(sqldb::display(db.execute("SELECT * FROM items WHERE id = 1").rows[0][1]), "winner");
}
TEST_F(Database, SplitIndexReopenAndNegativeKeys) {
  {
    sqldb::Engine db(path, 4);
    db.execute("CREATE TABLE items (id INT, value TEXT)");
    auto tx = db.begin();
    for (int i = -300; i < 700; i++)
      tx.execute("INSERT INTO items VALUES (" + std::to_string(i) + ", 'v')");
    tx.commit();
  }
  sqldb::Engine db(path, 4);
  for (int key : {-300, -1, 0, 128, 699})
    EXPECT_EQ(db.execute("SELECT * FROM items WHERE id = " + std::to_string(key)).rows.size(), 1);
}
TEST_F(Database, RollbackDoesNotPublishCatalogOrRows) {
  sqldb::Engine db(path);
  {
    auto tx = db.begin();
    tx.execute("CREATE TABLE discarded (id INT)");
  }
  EXPECT_THROW(db.execute("SELECT * FROM discarded"), sqldb::Error);
  db.execute("CREATE TABLE kept (id INT)");
  {
    auto tx = db.begin();
    tx.execute("INSERT INTO kept VALUES (1)");
  }
  EXPECT_TRUE(db.execute("SELECT * FROM kept").rows.empty());
}
TEST_F(Database, FailedStatementMakesTransactionRollbackOnly) {
  sqldb::Engine db(path);
  db.execute("CREATE TABLE items (id INT)");
  auto tx = db.begin();
  tx.execute("INSERT INTO items VALUES (1)");
  EXPECT_THROW(tx.execute("INSERT INTO items VALUES (1)"), sqldb::Error);
  EXPECT_THROW(tx.commit(), sqldb::Error);
  EXPECT_TRUE(db.execute("SELECT * FROM items").rows.empty());
}
TEST_F(Database, SqlQuotedTextAndUpdateTypeValidation) {
  sqldb::Engine db(path);
  db.execute("CREATE TABLE items (id INT, value TEXT)");
  db.execute("INSERT INTO items VALUES (1, 'Ada''s note')");
  EXPECT_EQ(sqldb::display(db.execute("SELECT * FROM items WHERE id = 1").rows[0][1]),
            "Ada's note");
  EXPECT_THROW(db.execute("UPDATE items SET value = 2 WHERE id = 1"), sqldb::Error);
}
} // namespace
