#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <string>
#include <vector>

namespace tz::pkg {

struct JsonValue {
  enum class Kind { Null, Bool, Number, String, Array, Object };
  Kind kind = Kind::Null;
  bool boolean = false;
  double number = 0;
  std::string string;
  std::vector<JsonValue> items;
  std::vector<std::string> keys;
  std::vector<size_t> keyOffsets;
  size_t begin = 0;
  size_t end = 0;

  bool isString() const { return kind == Kind::String; }
  bool isObject() const { return kind == Kind::Object; }
  bool isArray() const { return kind == Kind::Array; }
  const JsonValue *get(llvm::StringRef key) const;
  std::string getString(llvm::StringRef key) const;
};

}
