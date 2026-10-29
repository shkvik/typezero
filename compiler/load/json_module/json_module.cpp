#include "compiler/load/json_module/json_module.h"
#include "compiler/packages/json_parser/json_parser.h"
#include "compiler/packages/json_text/json_text.h"
#include "compiler/packages/json_value/json_value.h"
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace tz {

namespace {

using pkg::JsonValue;

struct Shape {
  enum class Kind { Unknown, Json, Bool, Int, Double, String, Array, Object };
  Kind kind = Kind::Unknown;
  std::vector<Shape> items;
  std::vector<std::string> keys;
  std::vector<bool> optional;
};

using Fields = std::vector<std::pair<std::string, const JsonValue *>>;

Fields fieldsOf(const JsonValue &object) {
  Fields fields;
  for (size_t i = 0; i < object.keys.size(); ++i) {
    auto known = std::find_if(fields.begin(), fields.end(),
                              [&](const auto &field) { return field.first == object.keys[i]; });
    if (known != fields.end()) {
      known->second = &object.items[i];
    } else {
      fields.emplace_back(object.keys[i], &object.items[i]);
    }
  }

  return fields;
}

bool integral(llvm::StringRef raw) {
  if (raw.find_first_of(".eE") != llvm::StringRef::npos) {
    return false;
  }
  int64_t value = 0;

  return !raw.getAsInteger(10, value) && value != INT64_MIN;
}

Shape join(const Shape &a, const Shape &b) {
  using Kind = Shape::Kind;
  if (a.kind == Kind::Unknown) {
    return b;
  }
  if (b.kind == Kind::Unknown) {
    return a;
  }
  if ((a.kind == Kind::Int && b.kind == Kind::Double) ||
      (a.kind == Kind::Double && b.kind == Kind::Int)) {
    return Shape{Kind::Double, {}, {}, {}};
  }
  if (a.kind != b.kind || a.kind == Kind::Json) {
    return Shape{Kind::Json, {}, {}, {}};
  }
  if (a.kind == Kind::Array) {
    Shape element = join(a.items[0], b.items[0]);
    return Shape{Kind::Array, {std::move(element)}, {}, {}};
  }
  if (a.kind != Kind::Object) {
    return a;
  }
  Shape merged{Kind::Object, {}, {}, {}};
  for (size_t i = 0; i < a.keys.size(); ++i) {
    auto there = std::find(b.keys.begin(), b.keys.end(), a.keys[i]);
    merged.keys.push_back(a.keys[i]);
    if (there == b.keys.end()) {
      merged.items.push_back(a.items[i]);
      merged.optional.push_back(true);
    } else {
      size_t k = size_t(there - b.keys.begin());
      merged.items.push_back(join(a.items[i], b.items[k]));
      merged.optional.push_back(a.optional[i] || b.optional[k]);
    }
  }
  for (size_t k = 0; k < b.keys.size(); ++k) {
    if (std::find(a.keys.begin(), a.keys.end(), b.keys[k]) == a.keys.end()) {
      merged.keys.push_back(b.keys[k]);
      merged.items.push_back(b.items[k]);
      merged.optional.push_back(true);
    }
  }

  return merged;
}

Shape shapeOf(const JsonValue &value, llvm::StringRef text) {
  using Kind = Shape::Kind;
  switch (value.kind) {
  case JsonValue::Kind::Null:
    return Shape{Kind::Json, {}, {}, {}};
  case JsonValue::Kind::Bool:
    return Shape{Kind::Bool, {}, {}, {}};
  case JsonValue::Kind::Number:
    return Shape{
        integral(text.slice(value.begin, value.end)) ? Kind::Int : Kind::Double, {}, {}, {}};
  case JsonValue::Kind::String:
    return Shape{Kind::String, {}, {}, {}};
  case JsonValue::Kind::Array: {
    Shape element;
    for (const JsonValue &item : value.items) {
      element = join(element, shapeOf(item, text));
    }
    return Shape{Kind::Array, {std::move(element)}, {}, {}};
  }
  case JsonValue::Kind::Object: {
    Shape object{Kind::Object, {}, {}, {}};
    for (const auto &[key, field] : fieldsOf(value)) {
      object.keys.push_back(key);
      object.items.push_back(shapeOf(*field, text));
      object.optional.push_back(false);
    }
    return object;
  }
  }

  return Shape{Kind::Json, {}, {}, {}};
}

std::string typeText(const Shape &shape) {
  using Kind = Shape::Kind;
  switch (shape.kind) {
  case Kind::Unknown:
  case Kind::Json:
    return "Json";
  case Kind::Bool:
    return "boolean";
  case Kind::Int:
    return "int";
  case Kind::Double:
    return "double";
  case Kind::String:
    return "string";
  case Kind::Array:
    return typeText(shape.items[0]) + "[]";
  case Kind::Object: {
    std::string out = "{";
    for (size_t i = 0; i < shape.keys.size(); ++i) {
      out += (i ? "; " : " ") + pkg::jsonQuote(shape.keys[i]) + (shape.optional[i] ? "?: " : ": ") +
             typeText(shape.items[i]);
    }
    return out + (shape.keys.empty() ? "}" : " }");
  }
  }

  return "Json";
}

std::string jsonText(const JsonValue &value, llvm::StringRef text) {
  switch (value.kind) {
  case JsonValue::Kind::Null:
    return "null";
  case JsonValue::Kind::Bool:
    return value.boolean ? "true" : "false";
  case JsonValue::Kind::Number: {
    llvm::StringRef raw = text.slice(value.begin, value.end);
    return raw.find_first_of(".eE") == llvm::StringRef::npos && !integral(raw) ? raw.str() + ".0"
                                                                               : raw.str();
  }
  case JsonValue::Kind::String:
    return pkg::jsonQuote(value.string);
  case JsonValue::Kind::Array: {
    std::string out = "[";
    for (size_t i = 0; i < value.items.size(); ++i) {
      out += (i ? ", " : "") + jsonText(value.items[i], text);
    }
    return out + "]";
  }
  case JsonValue::Kind::Object: {
    std::string out = "{";
    bool first = true;
    for (const auto &[key, field] : fieldsOf(value)) {
      out += (first ? " " : ", ") + pkg::jsonQuote(key) + ": " + jsonText(*field, text);
      first = false;
    }
    return out + (first ? "}" : " }");
  }
  }

  return "null";
}

std::string valueText(const JsonValue &value, const Shape &shape, llvm::StringRef text) {
  using Kind = Shape::Kind;
  switch (shape.kind) {
  case Kind::Unknown:
  case Kind::Json:
    return jsonText(value, text);
  case Kind::Double: {
    std::string raw = text.slice(value.begin, value.end).str();
    return raw.find_first_of(".eE") == std::string::npos ? raw + ".0" : raw;
  }
  case Kind::Array: {
    std::string out = "[";
    for (size_t i = 0; i < value.items.size(); ++i) {
      out += (i ? ", " : "") + valueText(value.items[i], shape.items[0], text);
    }
    return out + "]";
  }
  case Kind::Object: {
    std::string out = "{";
    bool first = true;
    for (const auto &[key, field] : fieldsOf(value)) {
      size_t k = size_t(std::find(shape.keys.begin(), shape.keys.end(), key) - shape.keys.begin());
      out += (first ? " " : ", ") + pkg::jsonQuote(key) + ": " +
             valueText(*field, shape.items[k], text);
      first = false;
    }
    return out + (first ? "}" : " }");
  }
  default:
    return jsonText(value, text);
  }
}

}

bool jsonModuleText(llvm::StringRef json, llvm::StringRef exported, std::string &out,
                    size_t &errorOffset, std::string &error) {
  pkg::JsonProblem problem = pkg::checkJsonText(json);
  if (!problem.message.empty()) {
    errorOffset = problem.offset;
    error = problem.message;
    return false;
  }
  JsonValue value;
  if (!pkg::parseJson(json, value, errorOffset, error)) {
    return false;
  }
  Shape shape = shapeOf(value, json);
  out = "export const " + exported.str() + ": " + typeText(shape) + " = " +
        valueText(value, shape, json) + "\n";

  return true;
}

}
