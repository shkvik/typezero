#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class ReadlineMethod {
  CLOSE,
  QUESTION,
  ON,
  ONCE,
  ADD_LISTENER,
  OFF,
  REMOVE_LISTENER,
  REMOVE_ALL_LISTENERS,
  EMIT,
  PREPEND_LISTENER,
  PREPEND_ONCE_LISTENER,
  LISTENERS,
  LISTENER_COUNT,
  ADD_EVENT_LISTENER,
  REMOVE_EVENT_LISTENER,
  DISPATCH_EVENT,
};

struct ReadlineMethodInfo {
  llvm::StringLiteral name;
  ReadlineMethod value;
};

class ReadlineMethodTable {
public:
  static std::optional<ReadlineMethod> find(llvm::StringRef name);
};

}
