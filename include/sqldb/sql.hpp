#pragma once
#include "storage.hpp"
#include <cctype>
#include <charconv>
#include <variant>
namespace sqldb {
using Value = std::variant<std::int64_t, std::string>;
using Row = std::vector<Value>;
struct Column {
  std::string name;
  bool integer;
};
struct Statement {
  enum class Kind { Create, Insert, Select, Update, Delete } kind;
  std::string table, column, where, join, left, right;
  std::vector<Column> columns;
  Row values;
  Value value = std::int64_t{0}, key = std::int64_t{0};
};
class Parser {
  struct Token {
    std::string text;
    bool quoted = false;
  };
  std::vector<Token> tokens_;
  std::size_t pos_ = 0;
  bool eat(const std::string &s) {
    if (pos_ < tokens_.size() && !tokens_[pos_].quoted && tokens_[pos_].text == s) {
      pos_++;
      return true;
    }
    return false;
  }
  void need(const std::string &s) {
    if (!eat(s))
      throw Error("Expected " + s);
  }
  std::string name() {
    if (pos_ == tokens_.size() || tokens_[pos_].quoted)
      throw Error("Expected identifier");
    auto s = tokens_[pos_++].text;
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_'))
      throw Error("Bad identifier");
    for (char c : s)
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.'))
        throw Error("Bad identifier");
    return s;
  }
  std::string table() {
    auto s = name();
    if (s.size() > 40 || s.find('.') != std::string::npos)
      throw Error("Table/column name restriction");
    return s;
  }
  Value literal() {
    if (pos_ == tokens_.size())
      throw Error("Expected literal");
    auto t = tokens_[pos_++];
    if (t.quoted)
      return t.text;
    std::int64_t value{};
    auto result = std::from_chars(t.text.data(), t.text.data() + t.text.size(), value);
    if (result.ec != std::errc{} || result.ptr != t.text.data() + t.text.size())
      throw Error("Expected INT or quoted TEXT");
    return value;
  }

public:
  explicit Parser(const std::string &source) {
    if (source.size() > 65536)
      throw Error("SQL length budget");
    for (std::size_t i = 0; i < source.size();) {
      auto c = source[i];
      if (std::isspace(static_cast<unsigned char>(c))) {
        i++;
        continue;
      }
      if (c == '\'') {
        std::string s;
        i++;
        bool closed = false;
        while (i < source.size()) {
          auto v = source[i++];
          if (v == '\'') {
            if (i < source.size() && source[i] == '\'') {
              i++;
              s += '\'';
            } else {
              closed = true;
              break;
            }
          } else
            s += v;
        }
        if (!closed)
          throw Error("Unterminated text");
        tokens_.push_back({s, true});
      } else if (std::string("(),=*;").find(c) != std::string::npos) {
        tokens_.push_back({std::string(1, c), false});
        i++;
      } else {
        auto begin = i;
        while (i < source.size() && !std::isspace(static_cast<unsigned char>(source[i])) &&
               std::string("(),=*;'").find(source[i]) == std::string::npos)
          i++;
        auto s = source.substr(begin, i - begin);
        for (char &v : s)
          v = static_cast<char>(std::tolower(static_cast<unsigned char>(v)));
        tokens_.push_back({s, false});
      }
    }
  }
  Statement parse() {
    Statement s{};
    if (eat("create")) {
      s.kind = Statement::Kind::Create;
      need("table");
      s.table = table();
      need("(");
      do {
        auto n = table();
        auto type = name();
        if (type != "int" && type != "text")
          throw Error("Types are INT and TEXT");
        if (eat("primary")) {
          need("key");
          if (!s.columns.empty() || type != "int")
            throw Error("First INT is primary key");
        }
        s.columns.push_back({n, type == "int"});
      } while (eat(","));
      need(")");
    } else if (eat("insert")) {
      s.kind = Statement::Kind::Insert;
      need("into");
      s.table = table();
      need("values");
      need("(");
      do {
        s.values.push_back(literal());
      } while (eat(","));
      need(")");
    } else if (eat("update")) {
      s.kind = Statement::Kind::Update;
      s.table = table();
      need("set");
      s.column = name();
      need("=");
      s.value = literal();
      need("where");
      s.where = name();
      need("=");
      s.key = literal();
    } else if (eat("delete")) {
      s.kind = Statement::Kind::Delete;
      need("from");
      s.table = table();
      need("where");
      s.where = name();
      need("=");
      s.key = literal();
    } else {
      s.kind = Statement::Kind::Select;
      need("select");
      need("*");
      need("from");
      s.table = table();
      if (eat("where")) {
        s.where = name();
        need("=");
        s.value = literal();
      } else if (eat("join")) {
        s.join = table();
        need("on");
        s.left = name();
        need("=");
        s.right = name();
      }
    }
    eat(";");
    if (pos_ != tokens_.size())
      throw Error("Unsupported or trailing SQL");
    return s;
  }
};
inline std::string display(const Value &v) {
  if (auto n = std::get_if<std::int64_t>(&v))
    return std::to_string(*n);
  return std::get<std::string>(v);
}
} // namespace sqldb
