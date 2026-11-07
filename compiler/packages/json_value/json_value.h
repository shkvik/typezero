#pragma once

#include "compiler/union/union/union.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <string>
#include <vector>

namespace tz::pkg {

struct JsonValue;

enum class JsonKind {
  JSON_NULL,
  JSON_BOOL,
  JSON_NUMBER,
  JSON_STRING,
  JSON_ARRAY,
  JSON_OBJECT,
};

struct JsonNull {
  static constexpr JsonKind kind = JsonKind::JSON_NULL;
};

struct JsonBool {
  static constexpr JsonKind kind = JsonKind::JSON_BOOL;
  bool value = false;
};

struct JsonNumber {
  static constexpr JsonKind kind = JsonKind::JSON_NUMBER;
  double value = 0;
};

struct JsonString {
  static constexpr JsonKind kind = JsonKind::JSON_STRING;
  std::string value;
};

struct JsonArray {
  static constexpr JsonKind kind = JsonKind::JSON_ARRAY;
  std::vector<JsonValue> items;
};

struct JsonObject {
  static constexpr JsonKind kind = JsonKind::JSON_OBJECT;
  std::vector<std::string> keys;
  std::vector<size_t> keyOffsets;
  std::vector<JsonValue> items;
};

using JsonForm = Union<JsonNull, JsonBool, JsonNumber, JsonString, JsonArray, JsonObject>;

struct JsonValue {
  JsonForm form = JsonNull{};
  size_t begin = 0;
  size_t end = 0;

  bool isString() const;
  bool isObject() const;
  bool isArray() const;
  bool boolean() const;
  double number() const;
  const std::string &string() const;
  llvm::ArrayRef<JsonValue> items() const;
  llvm::ArrayRef<std::string> keys() const;
  llvm::ArrayRef<size_t> keyOffsets() const;
  const JsonValue *get(llvm::StringRef key) const;
  const std::string &getString(llvm::StringRef key) const;
};

}
