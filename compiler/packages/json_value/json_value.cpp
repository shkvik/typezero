#include "compiler/packages/json_value/json_value.h"

namespace tz::pkg {

const JsonValue *JsonValue::get(llvm::StringRef key) const {
  for (size_t i = 0; i < keys.size(); ++i) {
    if (keys[i] == key) {
      return &items[i];
    }
  }

  return nullptr;
}

std::string JsonValue::getString(llvm::StringRef key) const {
  const JsonValue *value = get(key);

  return value && value->isString() ? value->string : std::string();
}

}
