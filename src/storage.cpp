#include "sqldb/storage.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <queue>
#include <set>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
namespace sqldb {
namespace {
constexpr std::size_t physical = 4112;
void fail(const char *s) { throw Error(std::string(s) + ": " + std::strerror(errno)); }
void read_all(int fd, std::uint8_t *p, std::size_t n, std::uint64_t at) {
  while (n) {
    auto got = ::pread(fd, p, n, static_cast<off_t>(at));
    if (got < 0 && errno == EINTR)
      continue;
    if (got < 0)
      fail("pread");
    if (got == 0)
      throw Error("Torn file tail");
    p += got;
    n -= got;
    at += got;
  }
}
void write_all(int fd, const std::uint8_t *p, std::size_t n, std::uint64_t at) {
  while (n) {
    auto put = ::pwrite(fd, p, n, static_cast<off_t>(at));
    if (put < 0 && errno == EINTR)
      continue;
    if (put <= 0)
      fail("pwrite");
    p += put;
    n -= put;
    at += put;
  }
}
std::uint64_t length(int fd) {
  struct stat s{};
  if (::fstat(fd, &s))
    fail("fstat");
  return static_cast<std::uint64_t>(s.st_size);
}
void sync(int fd) {
  if (::fsync(fd))
    fail("fsync");
}
} // namespace
std::uint32_t crc32(const std::uint8_t *p, std::size_t n) {
  std::uint32_t c = ~0u;
  for (std::size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int b = 0; b < 8; b++)
      c = (c >> 1) ^ (0xedb88320u & (~(c & 1) + 1));
  }
  return ~c;
}
Store::Store(const std::filesystem::path &root) : root_(std::filesystem::absolute(root)) {
  std::filesystem::create_directories(root_);
  lock_ = ::open((root_ / "LOCK").c_str(), O_CREAT | O_RDWR, 0600);
  if (lock_ < 0)
    fail("open lock");
  if (::flock(lock_, LOCK_EX | LOCK_NB)) {
    ::close(lock_);
    lock_ = -1;
    throw Error("Database already open");
  }
  try {
    auto format = root_ / "FORMAT";
    const std::string expected = "CPP_SQL_MVCC_ARIES_1\n";
    if (std::filesystem::exists(format)) {
      int f = ::open(format.c_str(), O_RDONLY);
      if (f < 0)
        fail("open format");
      std::string actual(length(f), '\0');
      try {
        read_all(f, reinterpret_cast<std::uint8_t *>(actual.data()), actual.size(), 0);
      } catch (...) {
        ::close(f);
        throw;
      }
      ::close(f);
      if (actual != expected)
        throw Error("Unsupported database format");
    } else {
      for (const auto &e : std::filesystem::directory_iterator(root_))
        if (e.path().filename() != "LOCK")
          throw Error("Refusing unmarked nonempty directory");
      int f = ::open(format.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
      if (f < 0)
        fail("create format");
      try {
        write_all(f, reinterpret_cast<const std::uint8_t *>(expected.data()), expected.size(), 0);
        sync(f);
      } catch (...) {
        ::close(f);
        throw;
      }
      ::close(f);
    }
    fd_ = ::open((root_ / "pages.dat").c_str(), O_CREAT | O_RDWR, 0600);
    if (fd_ < 0)
      fail("open pages");
    sync_directory();
    int parent = ::open(root_.parent_path().c_str(), O_RDONLY);
    if (parent < 0)
      fail("open parent");
    try {
      sync(parent);
    } catch (...) {
      ::close(parent);
      throw;
    }
    ::close(parent);
  } catch (...) {
    if (fd_ >= 0)
      ::close(fd_);
    ::close(lock_);
    throw;
  }
}
Store::~Store() {
  if (fd_ >= 0)
    ::close(fd_);
  if (lock_ >= 0) {
    ::flock(lock_, LOCK_UN);
    ::close(lock_);
  }
}
void Store::sync_directory() const {
  int fd = ::open(root_.c_str(), O_RDONLY);
  if (fd < 0)
    fail("open directory");
  try {
    sync(fd);
  } catch (...) {
    ::close(fd);
    throw;
  }
  ::close(fd);
}
PageId Store::count() const { return (length(fd_) + physical - 1) / physical; }
Page Store::read(PageId id) const {
  if (id >= count())
    return Page{};
  std::array<std::uint8_t, physical> raw{};
  read_all(fd_, raw.data(), raw.size(), id * physical);
  if (std::all_of(raw.begin(), raw.end(), [](auto b) { return b == 0; }))
    return Page{}; // Unallocated sparse hole.
  Reader r(raw.data(), raw.size());
  if (r.u32() != 0x50414731)
    throw Error("Invalid page envelope");
  auto lsn = r.u64();
  auto checksum = r.u32();
  Writer check;
  check.u64(lsn);
  Page p = r.page();
  check.page(p);
  if (crc32(check.data.data(), check.data.size()) != checksum)
    throw Error("Page checksum mismatch");
  return p;
}
std::uint64_t Store::page_lsn(PageId id) const noexcept {
  try {
    if (id >= count())
      return 0;
    std::array<std::uint8_t, physical> raw{};
    read_all(fd_, raw.data(), raw.size(), id * physical);
    Reader r(raw.data(), raw.size());
    if (r.u32() != 0x50414731)
      return 0;
    auto lsn = r.u64();
    auto checksum = r.u32();
    Writer c;
    c.u64(lsn);
    c.page(r.page());
    return crc32(c.data.data(), c.data.size()) == checksum ? lsn : 0;
  } catch (...) {
    return 0;
  }
}
void Store::write(PageId id, const Page &p, std::uint64_t lsn, bool durable) {
  if (id > (1ull << 40) || lsn == 0)
    throw Error("Invalid page address/LSN");
  Writer body;
  body.u64(lsn);
  body.page(p);
  Writer out;
  out.u32(0x50414731);
  out.u64(lsn);
  out.u32(crc32(body.data.data(), body.data.size()));
  out.page(p);
  write_all(fd_, out.data.data(), out.data.size(), id * physical);
  if (durable)
    sync(fd_);
}
void Store::force() {
  sync(fd_);
  sync_directory();
}
Wal::Wal(Store &s, Hook hook) : store_(s), hook_(std::move(hook)) {
  fd_ = ::open((s.root() / "wal.log").c_str(), O_CREAT | O_RDWR, 0600);
  if (fd_ < 0)
    fail("open WAL");
  try {
    s.sync_directory();
    scan();
  } catch (...) {
    ::close(fd_);
    throw;
  }
}
Wal::~Wal() {
  if (fd_ >= 0)
    ::close(fd_);
}
void Wal::force() { sync(fd_); }
void Wal::scan() {
  std::uint64_t at = 0, size = length(fd_);
  while (size - at >= 8) {
    std::array<std::uint8_t, 8> h{};
    read_all(fd_, h.data(), h.size(), at);
    Reader head(h.data(), h.size());
    if (head.u32() != 0x57414c31)
      throw Error("WAL magic corruption");
    auto n = head.u32();
    if (n < 33 || n > 8300)
      throw Error("WAL length corruption");
    if (size - at < n + 12ull)
      break;
    std::vector<std::uint8_t> payload(n + 4);
    read_all(fd_, payload.data(), payload.size(), at + 8);
    Reader tail(payload.data() + n, 4);
    if (crc32(payload.data(), n) != tail.u32())
      throw Error("Complete WAL record checksum corruption");
    Reader r(payload.data(), n);
    auto type = r.u8();
    if (type > static_cast<unsigned>(Kind::Checkpoint))
      throw Error("Unknown WAL type");
    Record e{};
    e.lsn = at + 1;
    e.kind = static_cast<Kind>(type);
    e.tx = r.u64();
    e.prev = r.u64();
    e.undo = r.u64();
    e.page = r.u64();
    if (e.prev >= e.lsn || e.undo >= e.lsn)
      throw Error("Invalid WAL chain");
    if (e.kind == Kind::Update || e.kind == Kind::Clr) {
      e.before = r.page();
      e.after = r.page();
    }
    if (r.pos != r.size)
      throw Error("Trailing WAL payload");
    log_[e.lsn] = e;
    next_tx_ = std::max(next_tx_, e.tx + 1);
    at += n + 12ull;
  }
  if (::ftruncate(fd_, static_cast<off_t>(at)))
    fail("truncate torn WAL tail");
  force();
}
Wal::Record Wal::append(Kind kind, std::uint64_t tx, std::uint64_t prev, std::uint64_t undo,
                        PageId page, const Page &before, const Page &after) {
  Record e{length(fd_) + 1, tx, prev, undo, kind, page, before, after};
  Writer p;
  p.u8(static_cast<std::uint8_t>(kind));
  p.u64(tx);
  p.u64(prev);
  p.u64(undo);
  p.u64(page);
  if (kind == Kind::Update || kind == Kind::Clr) {
    p.page(before);
    p.page(after);
  }
  Writer frame;
  frame.u32(0x57414c31);
  frame.u32(static_cast<std::uint32_t>(p.data.size()));
  frame.data.insert(frame.data.end(), p.data.begin(), p.data.end());
  frame.u32(crc32(p.data.data(), p.data.size()));
  write_all(fd_, frame.data.data(), frame.data.size(), e.lsn - 1);
  log_[e.lsn] = e;
  return e;
}
void Wal::commit(const Images &images) {
  if (images.empty())
    return;
  auto tx = next_tx_++;
  auto prev = append(Kind::Begin, tx, 0, 0).lsn;
  std::vector<Record> changed;
  for (const auto &[id, page] : images) {
    auto e = append(Kind::Update, tx, prev, 0, id, store_.read(id), page);
    prev = e.lsn;
    changed.push_back(e);
    hook_("update_appended");
  }
  force();
  hook_("updates_forced");
  // STEAL is deliberate: recovery must undo losers even when their pages reached disk.
  for (const auto &e : changed) {
    store_.write(e.page, e.after, e.lsn);
    hook_("steal_page_written");
  }
  prev = append(Kind::Commit, tx, prev, 0).lsn;
  force();
  hook_("commit_forced");
  store_.force();
  hook_("data_forced");
  append(Kind::End, tx, prev, 0);
  force();
  hook_("transaction_ended");
}
void Wal::recover() {
  std::map<std::uint64_t, std::uint64_t> last;
  std::set<std::uint64_t> committed;
  std::map<PageId, std::uint64_t> dirty;
  for (const auto &[lsn, e] : log_) {
    if (e.kind == Kind::End) {
      last.erase(e.tx);
      committed.erase(e.tx);
    } else if (e.tx) {
      last[e.tx] = lsn;
      if (e.kind == Kind::Commit)
        committed.insert(e.tx);
    }
    if (e.kind == Kind::Update || e.kind == Kind::Clr)
      dirty.emplace(e.page, lsn);
  }
  std::uint64_t start = ~std::uint64_t{};
  for (const auto &e : dirty)
    start = std::min(start, e.second);
  for (auto it = log_.lower_bound(start); it != log_.end(); ++it) {
    const auto &e = it->second;
    if ((e.kind == Kind::Update || e.kind == Kind::Clr) && store_.page_lsn(e.page) < e.lsn) {
      store_.write(e.page, e.after, e.lsn);
      hook_("recovery_redo");
    }
  }
  using Work = std::pair<std::uint64_t, std::uint64_t>;
  std::priority_queue<Work> undo;
  for (const auto &[tx, lsn] : last) {
    if (committed.count(tx))
      append(Kind::End, tx, lsn, 0);
    else
      undo.emplace(lsn, tx);
  }
  while (!undo.empty()) {
    auto [lsn, tx] = undo.top();
    undo.pop();
    auto it = log_.find(lsn);
    if (it == log_.end())
      throw Error("Broken undo chain");
    auto e = it->second;
    auto next = e.kind == Kind::Clr ? e.undo : e.prev;
    if (e.kind == Kind::Update) {
      auto clr = append(Kind::Clr, tx, last.at(tx), e.prev, e.page, e.after, e.before);
      last[tx] = clr.lsn;
      force();
      hook_("recovery_clr_forced");
      store_.write(e.page, e.before, clr.lsn);
      hook_("recovery_undo");
    }
    if (next)
      undo.emplace(next, tx);
    else
      append(Kind::End, tx, last.at(tx), 0);
  }
  store_.force();
  force();
  append(Kind::Checkpoint, 0, 0, 0);
  force();
}
Hook environment_hook() {
  auto wanted = std::getenv("SQL_CRASH_POINT");
  if (!wanted)
    return [](const auto &) {};
  std::string name = wanted;
  auto n = std::getenv("SQL_CRASH_OCCURRENCE");
  int occurrence = n ? std::stoi(n) : 1;
  auto count = std::make_shared<int>(0);
  return [name, occurrence, count](const auto &point) {
    if (point == name && ++*count == occurrence)
      ::_exit(91);
  };
}
} // namespace sqldb
