#include "compiler/packages/json_value/json_value.h"

namespace tz::pkg {

bool JsonValue::isString() const {
  return this->form.is<JsonString>();
}

bool JsonValue::isObject() const {
  return this->form.is<JsonObject>();
}

bool JsonValue::isArray() const {
  return this->form.is<JsonArray>();
}

bool JsonValue::boolean() const {
  const JsonBool *flag = this->form.as<JsonBool>();

  return flag && flag->value;
}

double JsonValue::number() const {
  const JsonNumber *parsed = this->form.as<JsonNumber>();

  return parsed ? parsed->value : 0;
}

const std::string &JsonValue::string() const {
  static const std::string none;
  const JsonString *text = this->form.as<JsonString>();

  return text ? text->value : none;
}

llvm::ArrayRef<JsonValue> JsonValue::items() const {
  llvm::ArrayRef<JsonValue> values;
  switch (this->form.kind()) {
    case JsonKind::JSON_NULL:
    case JsonKind::JSON_BOOL:
    case JsonKind::JSON_NUMBER:
    case JsonKind::JSON_STRING:
      break;
    case JsonKind::JSON_ARRAY:
      values = this->form.as<JsonArray>()->items;
      break;
    case JsonKind::JSON_OBJECT:
      values = this->form.as<JsonObject>()->items;
      break;
  }

  return values;
}

llvm::ArrayRef<std::string> JsonValue::keys() const {
  const JsonObject *object = this->form.as<JsonObject>();

  return object ? llvm::ArrayRef<std::string>(object->keys) : llvm::ArrayRef<std::string>();
}

llvm::ArrayRef<size_t> JsonValue::keyOffsets() const {
  const JsonObject *object = this->form.as<JsonObject>();

  return object ? llvm::ArrayRef<size_t>(object->keyOffsets) : llvm::ArrayRef<size_t>();
}

const JsonValue *JsonValue::get(llvm::StringRef key) const {
  llvm::ArrayRef<std::string> names = this->keys();
  for (size_t i = 0; i < names.size(); ++i) {
    if (names[i] == key) {
      return &this->items()[i];
    }
  }

  return nullptr;
}

const std::string &JsonValue::getString(llvm::StringRef key) const {
  static const std::string none;
  const JsonValue *value = this->get(key);

  return value ? value->string() : none;
}

}
