#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
namespace sqldb {
using PageId = std::uint64_t;
using Page = std::array<std::uint8_t, 4096>;
using Images = std::map<PageId, Page>;
using Hook = std::function<void(const std::string &)>;
struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};
struct Writer {
  std::vector<std::uint8_t> data;
  void u8(std::uint8_t n) { data.push_back(n); }
  void u16(std::uint16_t n) {
    for (int i = 0; i < 2; i++)
      u8((n >> (8 * i)) & 255);
  }
  void u32(std::uint32_t n) {
    for (int i = 0; i < 4; i++)
      u8((n >> (8 * i)) & 255);
  }
  void u64(std::uint64_t n) {
    for (int i = 0; i < 8; i++)
      u8((n >> (8 * i)) & 255);
  }
  void text(const std::string &s) {
    if (s.size() > 65535)
      throw Error("String too long");
    u16(static_cast<std::uint16_t>(s.size()));
    data.insert(data.end(), s.begin(), s.end());
  }
  void page(const Page &p) { data.insert(data.end(), p.begin(), p.end()); }
  Page finish() const {
    if (data.size() > 4096)
      throw Error("Page capacity exceeded");
    Page p{};
    std::copy(data.begin(), data.end(), p.begin());
    return p;
  }
};
struct Reader {
  const std::uint8_t *data;
  std::size_t size, pos = 0;
  Reader(const std::uint8_t *p, std::size_t n) : data(p), size(n) {}
  explicit Reader(const Page &p) : Reader(p.data(), p.size()) {}
  std::uint8_t u8() {
    if (pos >= size)
      throw Error("Truncated encoding");
    return data[pos++];
  }
  std::uint16_t u16() {
    std::uint16_t n = 0;
    for (int i = 0; i < 2; i++)
      n |= std::uint16_t(u8()) << (8 * i);
    return n;
  }
  std::uint32_t u32() {
    std::uint32_t n = 0;
    for (int i = 0; i < 4; i++)
      n |= std::uint32_t(u8()) << (8 * i);
    return n;
  }
  std::uint64_t u64() {
    std::uint64_t n = 0;
    for (int i = 0; i < 8; i++)
      n |= std::uint64_t(u8()) << (8 * i);
    return n;
  }
  std::string text() {
    auto n = u16();
    if (n > size - pos)
      throw Error("Truncated text");
    std::string s(reinterpret_cast<const char *>(data + pos), n);
    pos += n;
    return s;
  }
  Page page() {
    Page p{};
    for (auto &b : p)
      b = u8();
    return p;
  }
};
std::uint32_t crc32(const std::uint8_t *, std::size_t);
class Store {
  std::filesystem::path root_;
  int lock_ = -1, fd_ = -1;

public:
  explicit Store(const std::filesystem::path &);
  ~Store();
  Store(const Store &) = delete;
  Store &operator=(const Store &) = delete;
  const std::filesystem::path &root() const { return root_; }
  Page read(PageId) const;
  std::uint64_t page_lsn(PageId) const noexcept;
  void write(PageId, const Page &, std::uint64_t, bool force = true);
  PageId count() const;
  void force();
  void sync_directory() const;
};
class Wal {
  enum class Kind : std::uint8_t { Begin, Update, Commit, Abort, Clr, End, Checkpoint };
  struct Record {
    std::uint64_t lsn, tx, prev, undo;
    Kind kind;
    PageId page;
    Page before{}, after{};
  };
  Store &store_;
  Hook hook_;
  int fd_ = -1;
  std::map<std::uint64_t, Record> log_;
  std::uint64_t next_tx_ = 1;
  void scan();
  void force();
  Record append(Kind, std::uint64_t, std::uint64_t, std::uint64_t, PageId = 0,
                const Page & = Page{}, const Page & = Page{});

public:
  Wal(Store &, Hook);
  ~Wal();
  void recover();
  void commit(const Images &);
};
Hook environment_hook();
} // namespace sqldb
