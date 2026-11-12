#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class EventTargetMethod {
  ADD_EVENT_LISTENER,
  REMOVE_EVENT_LISTENER,
};

struct EventTargetMethodInfo {
  llvm::StringLiteral name;
  EventTargetMethod value;
};

class EventTargetMethodTable {
public:
  static std::optional<EventTargetMethod> find(llvm::StringRef name);
};

}
