#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class BytesShapeField {
  JSON_TYPE,
  JSON_DATA,
  ENCODE_READ,
  ENCODE_WRITTEN,
  DECODE_STREAM,
  DECODER_FATAL,
  DECODER_IGNORE_BOM,
};

struct BytesShapeFieldInfo {
  llvm::StringLiteral name;
  BytesShapeField value;
};

class BytesShapeFieldTable {
public:
  static std::optional<BytesShapeField> find(llvm::StringRef name);
  static llvm::StringRef getName(BytesShapeField value);
};

}
