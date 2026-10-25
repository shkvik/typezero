#pragma once

#include "llvm/ADT/StringRef.h"

#include <string>
#include <utility>
#include <vector>

namespace tz::pkg {

// A JSON value as written in a file: objects keep the order of their members,
// and every value knows where it is in the text, so that an error points at
// it and an edit replaces one value and keeps the rest of the file as it was.
// (llvm::json keeps neither.)
struct JsonValue {
  enum class Kind { Null, Bool, Number, String, Array, Object };
  Kind kind = Kind::Null;
  bool boolean = false;
  double number = 0;
  std::string string;
  // The elements of an array, or the values of an object's members, in order.
  std::vector<JsonValue> items;
  // An object's keys: keys[i] names items[i].
  std::vector<std::string> keys;
  std::vector<size_t> keyOffsets;
  // Where the value is in the text: [begin, end).
  size_t begin = 0;
  size_t end = 0;

  bool isString() const { return kind == Kind::String; }
  bool isObject() const { return kind == Kind::Object; }
  bool isArray() const { return kind == Kind::Array; }
  // The member `key` of an object, or null.
  const JsonValue *get(llvm::StringRef key) const;
  // The string member `key`, or "" if there is none or it is not a string.
  std::string getString(llvm::StringRef key) const;
};

// Parses JSON, also with the comments and trailing commas that tsconfig.json
// and deno.jsonc allow. On failure, `errorOffset` and `error` say where and
// what.
bool parseJson(llvm::StringRef text, JsonValue &out, size_t &errorOffset, std::string &error);

// `s` as a JSON string literal, quotes included.
std::string jsonQuote(llvm::StringRef s);

// The line and column (both from 1) of `offset` in `text`.
std::pair<unsigned, unsigned> lineAndColumn(llvm::StringRef text, size_t offset);

} // namespace tz::pkg
