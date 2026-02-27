#include "sqldb/engine.hpp"
#include <fcntl.h>
#include <iostream>
#include <unistd.h>
namespace {
bool value(sqldb::Engine &db, int key, const std::string &wanted) {
  auto result = db.execute("SELECT * FROM items WHERE id = " + std::to_string(key));
  return result.rows.size() == 1 && sqldb::display(result.rows[0][1]) == wanted;
}
void ack(const std::filesystem::path &root) {
  auto path = root / "ack.txt";
  int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0600);
  if (fd < 0)
    throw sqldb::Error("ack open");
  if (::write(fd, "100\n", 4) != 4 || ::fsync(fd)) {
    ::close(fd);
    throw sqldb::Error("ack force");
  }
  ::close(fd);
  int directory = ::open(root.c_str(), O_RDONLY);
  if (directory < 0)
    throw sqldb::Error("ack directory");
  int result = ::fsync(directory);
  ::close(directory);
  if (result)
    throw sqldb::Error("ack directory force");
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc != 3)
      return 2;
    std::filesystem::path root = argv[1];
    std::string mode = argv[2];
    if (mode == "seed") {
      sqldb::Engine db(root);
      db.execute("CREATE TABLE items (id INT PRIMARY KEY, value TEXT)");
      db.execute("INSERT INTO items VALUES (1, 'base')");
      db.execute("INSERT INTO items VALUES (2, 'stable')");
      return 0;
    }
    if (mode == "work") {
      bool armed = false;
      auto crash = sqldb::environment_hook();
      sqldb::Engine db(root, 8, [&](const auto &point) {
        if (armed)
          crash(point);
      });
      db.execute("INSERT INTO items VALUES (100, 'ack')");
      ack(root);
      armed = true;
      auto tx = db.begin();
      tx.execute("UPDATE items SET value = 'changed' WHERE id = 1");
      tx.execute("DELETE FROM items WHERE id = 2");
      tx.execute("INSERT INTO items VALUES (200, 'new')");
      tx.commit();
      return 0;
    }
    if (mode == "recover") {
      sqldb::Engine db(root, 8, sqldb::environment_hook());
      bool before = value(db, 1, "base") && value(db, 2, "stable") &&
                    db.execute("SELECT * FROM items WHERE id = 200").rows.empty();
      bool after = value(db, 1, "changed") &&
                   db.execute("SELECT * FROM items WHERE id = 2").rows.empty() &&
                   value(db, 200, "new");
      std::cout << "{\"ack_present\":" << (value(db, 100, "ack") ? "true" : "false")
                << ",\"state\":\""
                << (before  ? "before"
                    : after ? "after"
                            : "mixed")
                << "\"}\n";
      return 0;
    }
    return 2;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
