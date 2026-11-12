#pragma once

#include "llvm/ADT/StringRef.h"

namespace tz {

enum class ProcessEvent {
  UNCAUGHT_EXCEPTION,
  UNHANDLED_REJECTION,
  SIGINT,
  SIGTERM,
  SIGHUP,
  SIGBREAK,
};

enum class ProcessEventKind {
  THROWN,
  SIGNAL,
};

struct ProcessEventInfo {
  llvm::StringLiteral name;
  ProcessEvent value;
  ProcessEventKind kind;
};

class ProcessEventTable {
public:
  static const ProcessEventInfo *find(llvm::StringRef name);
};

}
