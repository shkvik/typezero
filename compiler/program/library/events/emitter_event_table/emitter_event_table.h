#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class EmitterEvent {
  ERROR_EVENT,
  NEW_LISTENER,
  REMOVE_LISTENER,
};

struct EmitterEventInfo {
  llvm::StringLiteral name;
  EmitterEvent value;
};

class EmitterEventTable {
public:
  static std::optional<EmitterEvent> find(llvm::StringRef name);
  static llvm::StringRef getName(EmitterEvent value);
};

}
