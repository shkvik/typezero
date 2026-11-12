#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class EmitterMember {
  ON,
  ADD_LISTENER,
  PREPEND_LISTENER,
  ONCE,
  PREPEND_ONCE_LISTENER,
  OFF,
  REMOVE_LISTENER,
  EMIT,
  LISTENERS,
  RAW_LISTENERS,
  LISTENER_COUNT,
  REMOVE_ALL_LISTENERS,
  EVENT_NAMES,
};

struct EmitterMemberInfo {
  llvm::StringLiteral name;
  EmitterMember value;
};

class EmitterMemberTable {
public:
  static std::optional<EmitterMember> find(llvm::StringRef name);
  static llvm::StringRef getName(EmitterMember value);
  static bool isName(llvm::StringRef name, EmitterMember value);
};

}
