#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class BuiltinConstructor {
  NUMBER,
  STRING,
  BOOLEAN,
  OBJECT,
  ARRAY,
  FUNCTION,
  DATE,
  REG_EXP,
  MAP,
  SET,
  PROMISE,
  UINT8_ARRAY,
  INT8_ARRAY,
  UINT8_CLAMPED_ARRAY,
  INT16_ARRAY,
  UINT16_ARRAY,
  INT32_ARRAY,
  UINT32_ARRAY,
  FLOAT16_ARRAY,
  FLOAT32_ARRAY,
  FLOAT64_ARRAY,
  BUFFER,
  ARRAY_BUFFER,
  DATA_VIEW,
  TEXT_ENCODER,
  TEXT_DECODER,
};

struct BuiltinConstructorInfo {
  llvm::StringLiteral name;
  BuiltinConstructor value;
};

class BuiltinConstructorTable {
public:
  static llvm::StringRef getName(BuiltinConstructor value);
  static std::optional<BuiltinConstructor> find(llvm::StringRef name);
};

}
