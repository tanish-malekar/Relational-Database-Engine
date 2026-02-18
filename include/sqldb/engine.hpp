#pragma once
#include "index.hpp"
#include "sql.hpp"
#include <thread>
namespace sqldb {
struct Result {
  std::vector<std::string> columns;
  std::vector<Row> rows;
  std::string plan;
};
struct WriteConflict : Error {
  using Error::Error;
};
class Engine {
  struct State;
  std::shared_ptr<State> state_;

public:
  class Transaction : public Pages {
    struct Table {
      std::string name;
      std::vector<Column> columns;
      PageId index = 0, head = 0, tail = 0;
    };
    std::shared_ptr<State> state_;
    std::uint64_t id_ = 0, snapshot_ = 0;
    std::thread::id owner_;
    Images writes_;
    std::map<std::string, Table> catalog_;
    bool finished_ = false, rollback_only_ = false;
    friend class Engine;
    Transaction(std::shared_ptr<State>, std::uint64_t, std::uint64_t);
    void active() const;
    void catalog_write();
    Table &table(const std::string &);
    static std::size_t column(const Table &, const std::string &);
    std::vector<std::uint8_t> encode(const Table &, const Row &);
    Row decode(const Table &, const std::vector<std::uint8_t> &);
    Row row(const Table &, std::uint64_t);
    std::vector<Row> scan(const Table &);
    std::vector<std::string> headers(const Table &);
    void finish_locked();
    Result execute_statement(const Statement &);

  public:
    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;
    Transaction(Transaction &&) = delete;
    ~Transaction();
    Page read(PageId) override;
    void write(PageId, const Page &) override;
    PageId allocate() override;
    Result execute(const std::string &);
    void commit();
    void rollback();
    std::uint64_t snapshot() const { return snapshot_; }
  };
  explicit Engine(
      const std::filesystem::path &, std::size_t cache_pages = 256,
      Hook hook = [](const auto &) {});
  Transaction begin();
  Result execute(const std::string &);
};
} // namespace sqldb
