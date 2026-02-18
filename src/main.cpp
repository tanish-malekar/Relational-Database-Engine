#include "sqldb/engine.hpp"
#include <iostream>
#include <memory>
int main(int argc, char **argv) {
  try {
    sqldb::Engine db(argc > 1 ? argv[1] : "data", 256, sqldb::environment_hook());
    std::unique_ptr<sqldb::Engine::Transaction> tx;
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line.empty())
        continue;
      auto command = line;
      while (!command.empty() &&
             (command.back() == ';' || std::isspace(static_cast<unsigned char>(command.back()))))
        command.pop_back();
      for (char &c : command)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      try {
        if (command == "QUIT" || command == "EXIT")
          break;
        if (command == "BEGIN") {
          if (tx)
            throw sqldb::Error("Transaction already active");
          tx.reset(new sqldb::Engine::Transaction(db.begin()));
          continue;
        }
        if (command == "COMMIT") {
          if (!tx)
            throw sqldb::Error("No active transaction");
          auto current = std::move(tx);
          current->commit();
          continue;
        }
        if (command == "ROLLBACK") {
          if (tx) {
            tx->rollback();
            tx.reset();
          }
          continue;
        }
        auto result = tx ? tx->execute(line) : db.execute(line);
        for (const auto &col : result.columns)
          std::cout << col << '\t';
        std::cout << '\n';
        for (const auto &row : result.rows) {
          for (const auto &value : row)
            std::cout << sqldb::display(value) << '\t';
          std::cout << '\n';
        }
        std::cout << result.rows.size() << " rows; plan=" << result.plan << '\n';
      } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << '\n';
      }
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
