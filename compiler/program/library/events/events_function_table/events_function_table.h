#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class EventsFunction {
  GET_EVENT_LISTENERS,
  LISTENER_COUNT,
  ONCE,
  ON,
};

struct EventsFunctionInfo {
  llvm::StringLiteral name;
  EventsFunction value;
};

class EventsFunctionTable {
public:
  static std::optional<EventsFunction> find(llvm::StringRef name);
};

}
