#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class GlobalName {
  ARRAY,
  BIG_INT,
  BIG_INT64_ARRAY,
  BIG_UINT64_ARRAY,
  BIT,
  BOOLEAN,
  BUFFER,
  CLEAR_IMMEDIATE,
  CLEAR_INTERVAL,
  CLEAR_TIMEOUT,
  CONSOLE,
  CRYPTO,
  DATE,
  DECIMAL,
  DECODE_URI,
  DECODE_URI_COMPONENT,
  EMBED_BYTES,
  EMBED_TEXT,
  ENCODE_URI,
  ENCODE_URI_COMPONENT,
  ERR,
  ERR_TYPE,
  ESCAPE,
  GLOBAL_THIS,
  INTL,
  ITERATOR,
  JSON,
  JSON_TYPE,
  MAP,
  MATH,
  NUMBER,
  OBJECT,
  OK,
  OK_TYPE,
  PARSE_FLOAT,
  PARSE_INT,
  PERFORMANCE,
  PROCESS,
  PROMISE,
  QUEUE_MICROTASK,
  REFLECT,
  REG_EXP,
  RESULT,
  SET,
  SET_IMMEDIATE,
  SET_INTERVAL,
  SET_TIMEOUT,
  STRING,
  SYMBOL,
  UNESCAPE,
  WEAK_MAP,
  WEAK_REF,
  WEAK_SET,
};

struct GlobalNameInfo {
  llvm::StringLiteral name;
  GlobalName global;
};

class GlobalNameTable {
public:
  static std::optional<GlobalName> find(llvm::StringRef name);
  static llvm::StringRef getName(GlobalName global);
};

}
