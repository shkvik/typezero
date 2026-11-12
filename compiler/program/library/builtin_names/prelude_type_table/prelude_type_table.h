#pragma once

#include "llvm/ADT/StringRef.h"

namespace tz {

enum class PreludeType {
  BUFFER_ENCODING,
  DATE_ERROR,
  DECIMAL_ERROR,
  DECODE_ERROR,
  ENCODING_ERROR,
  JSON_ERROR,
  NUMBER_ERROR,
  QUESTION_ERROR,
  REFLECT_ERROR,
  REFLECT_FILL_ERROR,
  REFLECT_FILL_VALUE_ERROR,
  REGEX_ERROR,
  STREAM_ERROR,
  URI_ERROR,
};

struct PreludeTypeInfo {
  llvm::StringLiteral name;
  PreludeType type;
};

class PreludeTypeTable {
public:
  static llvm::StringRef getName(PreludeType type);
};

}
