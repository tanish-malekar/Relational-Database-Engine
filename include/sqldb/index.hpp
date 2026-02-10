#pragma once
#include "storage.hpp"
#include <algorithm>
#include <optional>
namespace sqldb {
struct Pages {
  virtual ~Pages() = default;
  virtual Page read(PageId) = 0;
  virtual void write(PageId, const Page &) = 0;
  virtual PageId allocate() = 0;
};
class BPlusTree {
  Pages &pages_;
  PageId header_;
  static constexpr std::size_t maximum = 128;
  struct Node {
    bool leaf = true;
    PageId next = 0;
    std::vector<std::int64_t> keys;
    std::vector<std::uint64_t> values;
  };
  struct Split {
    std::int64_t key;
    PageId right;
  };
  PageId root() {
    auto p = pages_.read(header_);
    Reader r(p);
    if (r.u32() != 0x42504831)
      throw Error("Bad index header");
    return r.u64();
  }
  void root(PageId id) {
    Writer w;
    w.u32(0x42504831);
    w.u64(id);
    pages_.write(header_, w.finish());
  }
  Node load(PageId id) {
    auto p = pages_.read(id);
    Reader r(p);
    if (r.u32() != 0x42504e31)
      throw Error("Bad index node");
    auto leaf = r.u8();
    if (leaf > 1)
      throw Error("Bad leaf flag");
    Node n;
    n.leaf = leaf;
    n.next = r.u64();
    auto count = r.u16();
    if (count > maximum)
      throw Error("Index key count");
    if (!n.leaf)
      n.values.push_back(r.u64());
    for (unsigned i = 0; i < count; i++) {
      n.keys.push_back(static_cast<std::int64_t>(r.u64()));
      n.values.push_back(r.u64());
    }
    if (!std::is_sorted(n.keys.begin(), n.keys.end()))
      throw Error("Index key order");
    return n;
  }
  void save(PageId id, const Node &n) {
    Writer w;
    w.u32(0x42504e31);
    w.u8(n.leaf);
    w.u64(n.next);
    w.u16(static_cast<std::uint16_t>(n.keys.size()));
    if (!n.leaf)
      w.u64(n.values.at(0));
    for (std::size_t i = 0; i < n.keys.size(); i++) {
      w.u64(static_cast<std::uint64_t>(n.keys[i]));
      w.u64(n.values.at(i + (n.leaf ? 0 : 1)));
    }
    pages_.write(id, w.finish());
  }
  static std::size_t lower(const Node &n, std::int64_t key) {
    return static_cast<std::size_t>(std::lower_bound(n.keys.begin(), n.keys.end(), key) -
                                    n.keys.begin());
  }
  std::optional<Split> insert(PageId id, std::int64_t key, std::uint64_t value) {
    auto n = load(id);
    auto at = lower(n, key);
    if (n.leaf) {
      if (at < n.keys.size() && n.keys[at] == key)
        throw Error("Duplicate primary key");
      n.keys.insert(n.keys.begin() + at, key);
      n.values.insert(n.values.begin() + at, value);
    } else {
      if (at < n.keys.size() && n.keys[at] == key)
        at++;
      auto split = insert(n.values.at(at), key, value);
      if (split) {
        n.keys.insert(n.keys.begin() + at, split->key);
        n.values.insert(n.values.begin() + at + 1, split->right);
      }
    }
    if (n.keys.size() <= maximum) {
      save(id, n);
      return std::nullopt;
    }
    auto middle = n.keys.size() / 2;
    auto separator = n.keys[middle];
    Node right;
    right.leaf = n.leaf;
    PageId right_id = pages_.allocate();
    if (n.leaf) {
      right.keys.assign(n.keys.begin() + middle, n.keys.end());
      right.values.assign(n.values.begin() + middle, n.values.end());
      n.keys.resize(middle);
      n.values.resize(middle);
      right.next = n.next;
      n.next = right_id;
    } else {
      right.keys.assign(n.keys.begin() + middle + 1, n.keys.end());
      right.values.assign(n.values.begin() + middle + 1, n.values.end());
      n.keys.resize(middle);
      n.values.resize(middle + 1);
    }
    save(right_id, right);
    save(id, n);
    return Split{separator, right_id};
  }

public:
  BPlusTree(Pages &p, PageId header) : pages_(p), header_(header) {}
  void initialize() {
    auto id = pages_.allocate();
    save(id, Node{});
    root(id);
  }
  std::optional<std::uint64_t> get(std::int64_t key) {
    auto n = load(root());
    while (!n.leaf) {
      auto at = lower(n, key);
      if (at < n.keys.size() && n.keys[at] == key)
        at++;
      n = load(n.values.at(at));
    }
    auto at = lower(n, key);
    if (at < n.keys.size() && n.keys[at] == key)
      return n.values.at(at);
    return std::nullopt;
  }
  void put(std::int64_t key, std::uint64_t value) {
    auto old = root();
    auto split = insert(old, key, value);
    if (split) {
      Node top;
      top.leaf = false;
      top.keys = {split->key};
      top.values = {old, split->right};
      auto id = pages_.allocate();
      save(id, top);
      root(id);
    }
  }
  void remove(std::int64_t key) {
    auto id = root();
    auto n = load(id);
    while (!n.leaf) {
      auto at = lower(n, key);
      if (at < n.keys.size() && n.keys[at] == key)
        at++;
      id = n.values.at(at);
      n = load(id);
    }
    auto at = lower(n, key);
    if (at < n.keys.size() && n.keys[at] == key) {
      n.keys.erase(n.keys.begin() + at);
      n.values.erase(n.values.begin() + at);
      save(id, n);
    }
  }
};
} // namespace sqldb
