#include "sqldb/engine.hpp"
#include <limits>
#include <list>
#include <mutex>
#include <set>
#include <unordered_map>
namespace sqldb {
namespace {
class HeapPage {
  Page p_{};
  std::uint16_t at(std::size_t n) const {
    return std::uint16_t(p_.at(n)) | (std::uint16_t(p_.at(n + 1)) << 8);
  }
  void set(std::size_t n, std::uint16_t v) {
    p_.at(n) = v & 255;
    p_.at(n + 1) = v >> 8;
  }

public:
  static HeapPage empty() {
    Writer w;
    w.u32(0x48454131);
    w.u64(0);
    w.u16(0);
    w.u16(4096);
    return HeapPage(w.finish());
  }
  explicit HeapPage(Page p) : p_(p) {
    Reader r(p_);
    if (r.u32() != 0x48454131)
      throw Error("Bad heap page");
    if (16 + std::size_t(count()) * 4 > at(14) || at(14) > 4096)
      throw Error("Heap directory corruption");
  }
  std::uint16_t count() const { return at(12); }
  Page bytes() const { return p_; }
  PageId next() const {
    Reader r(p_.data() + 4, 8);
    return r.u64();
  }
  void next(PageId id) {
    Writer w;
    w.u64(id);
    std::copy(w.data.begin(), w.data.end(), p_.begin() + 4);
  }
  std::vector<std::uint8_t> get(std::uint16_t slot) const {
    if (slot >= count())
      throw Error("Invalid row slot");
    auto begin = at(16 + slot * 4), size = at(18 + slot * 4);
    if (begin < at(14) || std::size_t(begin) + size > 4096)
      throw Error("Tuple bounds corruption");
    return {p_.begin() + begin, p_.begin() + begin + size};
  }
  int append(const std::vector<std::uint8_t> &tuple) {
    auto n = count(), end = at(14);
    if (tuple.size() > end || end - tuple.size() < 16 + (std::size_t(n) + 1) * 4)
      return -1;
    auto begin = static_cast<std::uint16_t>(end - tuple.size());
    std::copy(tuple.begin(), tuple.end(), p_.begin() + begin);
    set(16 + n * 4, begin);
    set(18 + n * 4, static_cast<std::uint16_t>(tuple.size()));
    set(12, n + 1);
    set(14, begin);
    return n;
  }
  void replace(std::uint16_t slot, const std::vector<std::uint8_t> &tuple) {
    if (slot >= count())
      throw Error("Invalid slot");
    auto replacement = empty();
    replacement.next(next());
    for (std::uint16_t i = 0; i < count(); i++)
      if (replacement.append(i == slot ? tuple : get(i)) < 0)
        throw Error("Updated row exceeds page capacity");
    p_ = replacement.p_;
  }
};
} // namespace
struct Engine::State {
  Store disk;
  Wal wal;
  std::mutex mutex;
  std::size_t capacity;
  bool poisoned = false;
  std::uint64_t clock = 0, next_tx = 1;
  PageId next_page;
  std::map<std::uint64_t, std::uint64_t> active;
  std::map<std::thread::id, std::uint64_t> owners;
  std::map<PageId, std::uint64_t> latest;
  std::map<PageId, std::map<std::uint64_t, Page>> versions;
  std::list<PageId> lru;
  struct Cached {
    Page page;
    std::list<PageId>::iterator at;
  };
  std::unordered_map<PageId, Cached> cache;
  State(const std::filesystem::path &path, std::size_t pages, Hook hook)
      : disk(path), wal(disk, std::move(hook)), capacity(pages), next_page(1) {
    if (!pages)
      throw Error("Positive cache size required");
    wal.recover();
    next_page = std::max<PageId>(1, disk.count());
  }
  Page current(PageId id) {
    auto it = cache.find(id);
    if (it != cache.end()) {
      lru.splice(lru.begin(), lru, it->second.at);
      return it->second.page;
    }
    auto p = disk.read(id);
    lru.push_front(id);
    cache.emplace(id, Cached{p, lru.begin()});
    if (cache.size() > capacity) {
      cache.erase(lru.back());
      lru.pop_back();
    }
    return p;
  }
  void invalidate(PageId id) {
    auto it = cache.find(id);
    if (it != cache.end()) {
      lru.erase(it->second.at);
      cache.erase(it);
    }
  }
  void prune() {
    auto oldest = clock;
    for (const auto &[id, snapshot] : active)
      oldest = std::min(oldest, snapshot);
    for (auto it = versions.begin(); it != versions.end();) {
      auto &history = it->second;
      auto floor = history.upper_bound(oldest);
      if (floor != history.begin()) {
        --floor;
        history.erase(history.begin(), floor);
      }
      if (history.rbegin()->first <= oldest)
        it = versions.erase(it);
      else
        ++it;
    }
  }
};
Engine::Engine(const std::filesystem::path &path, std::size_t pages, Hook hook)
    : state_(std::make_shared<State>(path, pages, std::move(hook))) {}
Engine::Transaction Engine::begin() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->poisoned)
    throw Error("Reopen database for recovery");
  auto owner = std::this_thread::get_id();
  if (state_->owners.count(owner))
    throw Error("Nested transaction unsupported");
  auto id = state_->next_tx++;
  auto snapshot = state_->clock;
  state_->active[id] = snapshot;
  state_->owners[owner] = id;
  try {
    return Transaction(state_, id, snapshot);
  } catch (...) {
    state_->active.erase(id);
    state_->owners.erase(owner);
    state_->prune();
    throw;
  }
}
Engine::Transaction::Transaction(std::shared_ptr<State> state, std::uint64_t id,
                                 std::uint64_t snapshot)
    : state_(std::move(state)), id_(id), snapshot_(snapshot), owner_(std::this_thread::get_id()) {
  // begin() owns the state latch here. Catalog is read directly with snapshot visibility.
  Page p;
  auto version = state_->versions.find(0);
  if (version != state_->versions.end()) {
    auto entry = version->second.upper_bound(snapshot_);
    if (entry == version->second.begin())
      p = state_->current(0);
    else {
      --entry;
      p = entry->second;
    }
  } else
    p = state_->current(0);
  if (std::all_of(p.begin(), p.end(), [](auto b) { return b == 0; }))
    return;
  Reader r(p);
  if (r.u32() != 0x43415431)
    throw Error("Bad catalog");
  auto count = r.u16();
  if (count > 64)
    throw Error("Catalog table count");
  for (unsigned i = 0; i < count; i++) {
    Table t;
    t.name = r.text();
    auto columns = r.u8();
    if (!columns || columns > 32)
      throw Error("Catalog columns");
    for (unsigned c = 0; c < columns; c++) {
      auto name = r.text();
      auto type = r.u8();
      if (type > 1)
        throw Error("Catalog type");
      t.columns.push_back({name, type == 1});
    }
    t.index = r.u64();
    t.head = r.u64();
    t.tail = r.u64();
    catalog_.emplace(t.name, std::move(t));
  }
}
void Engine::Transaction::active() const {
  if (finished_ || owner_ != std::this_thread::get_id())
    throw Error("Inactive/thread-confined transaction");
}
Engine::Transaction::~Transaction() {
  if (!finished_) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    finish_locked();
  }
}
Page Engine::Transaction::read(PageId id) {
  active();
  auto own = writes_.find(id);
  if (own != writes_.end())
    return own->second;
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->poisoned)
    throw Error("Reopen for recovery");
  auto it = state_->versions.find(id);
  if (it != state_->versions.end()) {
    auto version = it->second.upper_bound(snapshot_);
    if (version != it->second.begin()) {
      --version;
      return version->second;
    }
  }
  return state_->current(id);
}
void Engine::Transaction::write(PageId id, const Page &p) {
  active();
  if (!writes_.count(id) && writes_.size() >= 4096)
    throw Error("Transaction page budget");
  writes_[id] = p;
}
PageId Engine::Transaction::allocate() {
  active();
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->poisoned)
    throw Error("Reopen for recovery");
  if (state_->next_page >= (1ull << 40))
    throw Error("Page address budget");
  return state_->next_page++;
}
void Engine::Transaction::catalog_write() {
  Writer w;
  w.u32(0x43415431);
  w.u16(static_cast<std::uint16_t>(catalog_.size()));
  for (const auto &[name, t] : catalog_) {
    w.text(name);
    w.u8(static_cast<std::uint8_t>(t.columns.size()));
    for (const auto &c : t.columns) {
      w.text(c.name);
      w.u8(c.integer);
    }
    w.u64(t.index);
    w.u64(t.head);
    w.u64(t.tail);
  }
  write(0, w.finish());
}
Engine::Transaction::Table &Engine::Transaction::table(const std::string &name) {
  auto it = catalog_.find(name);
  if (it == catalog_.end())
    throw Error("Unknown table " + name);
  return it->second;
}
std::size_t Engine::Transaction::column(const Table &t, const std::string &reference) {
  auto name = reference;
  auto dot = name.find('.');
  if (dot != std::string::npos) {
    if (name.substr(0, dot) != t.name)
      throw Error("Wrong table qualifier");
    name = name.substr(dot + 1);
  }
  for (std::size_t i = 0; i < t.columns.size(); i++)
    if (t.columns[i].name == name)
      return i;
  throw Error("Unknown column " + reference);
}
std::vector<std::string> Engine::Transaction::headers(const Table &t) {
  std::vector<std::string> out;
  for (const auto &c : t.columns)
    out.push_back(t.name + "." + c.name);
  return out;
}
std::vector<std::uint8_t> Engine::Transaction::encode(const Table &t, const Row &row) {
  if (row.size() != t.columns.size())
    throw Error("Column count mismatch");
  Writer w;
  for (std::size_t i = 0; i < row.size(); i++) {
    if (t.columns[i].integer) {
      auto n = std::get_if<std::int64_t>(&row[i]);
      if (!n)
        throw Error("Expected INT");
      w.u64(static_cast<std::uint64_t>(*n));
    } else {
      auto s = std::get_if<std::string>(&row[i]);
      if (!s)
        throw Error("Expected TEXT");
      w.text(*s);
    }
  }
  if (w.data.size() > 4076)
    throw Error("Tuple exceeds page capacity");
  return w.data;
}
Row Engine::Transaction::decode(const Table &t, const std::vector<std::uint8_t> &bytes) {
  Reader r(bytes.data(), bytes.size());
  Row row;
  for (const auto &c : t.columns) {
    if (c.integer)
      row.emplace_back(static_cast<std::int64_t>(r.u64()));
    else
      row.emplace_back(r.text());
  }
  if (r.pos != r.size)
    throw Error("Trailing tuple bytes");
  return row;
}
Row Engine::Transaction::row(const Table &t, std::uint64_t id) {
  return decode(t, HeapPage(read(id >> 16)).get(static_cast<std::uint16_t>(id)));
}
std::vector<Row> Engine::Transaction::scan(const Table &t) {
  std::vector<Row> rows;
  std::set<PageId> seen;
  for (auto id = t.head; id;) {
    if (!seen.insert(id).second)
      throw Error("Heap chain cycle");
    HeapPage heap(read(id));
    for (std::uint16_t i = 0; i < heap.count(); i++) {
      auto bytes = heap.get(i);
      if (!bytes.empty())
        rows.push_back(decode(t, bytes));
    }
    id = heap.next();
  }
  return rows;
}
Result Engine::Transaction::execute(const std::string &sql) {
  active();
  if (rollback_only_)
    throw Error("Transaction requires rollback");
  try {
    return execute_statement(Parser(sql).parse());
  } catch (...) {
    rollback_only_ = true;
    throw;
  }
}
Result Engine::Transaction::execute_statement(const Statement &s) {
  if (s.kind == Statement::Kind::Create) {
    if (catalog_.count(s.table) || catalog_.size() >= 64 || s.columns.empty() ||
        s.columns.size() > 32 || !s.columns[0].integer)
      throw Error("Invalid schema or duplicate table");
    std::set<std::string> names;
    for (const auto &c : s.columns)
      if (!names.insert(c.name).second)
        throw Error("Duplicate column");
    Table t;
    t.name = s.table;
    t.columns = s.columns;
    t.index = allocate();
    BPlusTree(*this, t.index).initialize();
    catalog_[s.table] = t;
    catalog_write();
    return {{}, {}, "CreateTable"};
  }
  auto &t = table(s.table);
  BPlusTree index(*this, t.index);
  if (s.kind == Statement::Kind::Insert) {
    auto tuple = encode(t, s.values);
    auto key = std::get<std::int64_t>(s.values.at(0));
    if (index.get(key))
      throw Error("Duplicate primary key");
    PageId id = t.tail;
    auto heap = id ? HeapPage(read(id)) : HeapPage::empty();
    int slot = heap.append(tuple);
    if (!id || slot < 0) {
      auto fresh = allocate();
      if (id) {
        auto previous = HeapPage(read(id));
        previous.next(fresh);
        write(id, previous.bytes());
      } else
        t.head = fresh;
      t.tail = fresh;
      id = fresh;
      heap = HeapPage::empty();
      slot = heap.append(tuple);
      if (slot < 0)
        throw Error("Row too large");
      catalog_write();
    }
    write(id, heap.bytes());
    index.put(key, (id << 16) | static_cast<std::uint16_t>(slot));
    return {{}, {}, "Insert + BPlusTree"};
  }
  if (s.kind == Statement::Kind::Update || s.kind == Statement::Kind::Delete) {
    auto key = std::get_if<std::int64_t>(&s.key);
    if (!key || column(t, s.where) != 0)
      throw Error("Mutation requires primary-key equality");
    auto found = index.get(*key);
    if (!found)
      return {{}, {}, "Mutation 0"};
    auto id = *found;
    HeapPage heap(read(id >> 16));
    if (s.kind == Statement::Kind::Delete) {
      heap.replace(static_cast<std::uint16_t>(id), {});
      index.remove(*key);
    } else {
      auto col = column(t, s.column);
      if (col == 0)
        throw Error("Primary keys are immutable");
      auto values = row(t, id);
      values[col] = s.value;
      heap.replace(static_cast<std::uint16_t>(id), encode(t, values));
    }
    write(id >> 16, heap.bytes());
    return {{}, {}, "Mutation + MVCC"};
  }
  Result result;
  result.columns = headers(t);
  if (s.join.empty()) {
    if (!s.where.empty() && column(t, s.where) == 0 &&
        std::holds_alternative<std::int64_t>(s.value)) {
      auto found = index.get(std::get<std::int64_t>(s.value));
      if (found)
        result.rows.push_back(row(t, *found));
      result.plan = "BPlusTreePointLookup";
    } else {
      result.rows = scan(t);
      result.plan = "SequentialScan";
      if (!s.where.empty()) {
        auto col = column(t, s.where);
        result.rows.erase(std::remove_if(result.rows.begin(), result.rows.end(),
                                         [&](const auto &row) { return row[col] != s.value; }),
                          result.rows.end());
      }
    }
    return result;
  }
  auto &right = table(s.join);
  auto a = column(t, s.left), b = column(right, s.right);
  auto other_headers = headers(right);
  result.columns.insert(result.columns.end(), other_headers.begin(), other_headers.end());
  if (b == 0) {
    result.plan = "IndexNestedLoopJoin";
    BPlusTree other(*this, right.index);
    for (auto left : scan(t)) {
      auto key = std::get_if<std::int64_t>(&left[a]);
      if (!key)
        continue;
      auto found = other.get(*key);
      if (found) {
        auto rr = row(right, *found);
        left.insert(left.end(), rr.begin(), rr.end());
        result.rows.push_back(std::move(left));
      }
    }
  } else {
    result.plan = "HashJoin";
    std::unordered_map<Value, std::vector<Row>> buckets;
    for (auto rr : scan(right))
      buckets[rr[b]].push_back(std::move(rr));
    for (const auto &left : scan(t)) {
      auto it = buckets.find(left[a]);
      if (it == buckets.end())
        continue;
      for (const auto &rr : it->second) {
        auto joined = left;
        joined.insert(joined.end(), rr.begin(), rr.end());
        result.rows.push_back(std::move(joined));
      }
    }
  }
  return result;
}
void Engine::Transaction::finish_locked() {
  writes_.clear();
  finished_ = true;
  state_->active.erase(id_);
  state_->owners.erase(owner_);
  state_->prune();
}
void Engine::Transaction::commit() {
  active();
  if (rollback_only_) {
    rollback();
    throw Error("Failed transaction rolled back");
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  try {
    if (state_->poisoned)
      throw Error("Reopen for recovery");
    Images before;
    for (const auto &[id, page] : writes_) {
      if (state_->latest[id] > snapshot_)
        throw WriteConflict("First-committer-wins page conflict");
      before[id] = state_->current(id);
    }
    if (!writes_.empty()) {
      try {
        state_->wal.commit(writes_);
      } catch (...) {
        state_->poisoned = true;
        throw;
      }
      auto stamp = ++state_->clock;
      for (const auto &[id, page] : writes_) {
        auto &history = state_->versions[id];
        history.emplace(state_->latest[id], before.at(id));
        history[stamp] = page;
        state_->latest[id] = stamp;
        state_->invalidate(id);
      }
    }
  } catch (...) {
    finish_locked();
    throw;
  }
  finish_locked();
}
void Engine::Transaction::rollback() {
  active();
  std::lock_guard<std::mutex> lock(state_->mutex);
  finish_locked();
}
Result Engine::execute(const std::string &sql) {
  auto tx = begin();
  auto result = tx.execute(sql);
  tx.commit();
  return result;
}
} // namespace sqldb
