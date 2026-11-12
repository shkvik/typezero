#pragma once

#include "llvm/ADT/StringRef.h"

namespace tz {

enum class LibraryClass {
  CONCURRENT_MAP,
  CRYPTO_KEY,
  CUSTOM_EVENT,
  EVENT,
  EVENT_EMITTER,
  EVENT_EMITTER_BASE,
  EVENT_TARGET,
  HEADERS,
  LOCALE,
  PIPELINE_STAGE,
  REQUEST,
  RESPONSE,
  SERVER,
  SOCKET,
  STREAM,
  STREAM_EMITTER,
  SUBTLE_CRYPTO,
  URL,
};

struct LibraryClassInfo {
  llvm::StringLiteral name;
  LibraryClass value;
};

class LibraryClassTable {
public:
  static llvm::StringRef getName(LibraryClass value);
  static bool isName(llvm::StringRef name, LibraryClass value);
};

}
