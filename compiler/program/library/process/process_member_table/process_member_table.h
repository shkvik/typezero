#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class ProcessMember {
  NEXT_TICK,
  EXIT,
  CWD,
  UPTIME,
  ON,
  ONCE,
  OFF,
  ADD_LISTENER,
  PREPEND_LISTENER,
  ARGV,
  ENV,
  EXIT_CODE,
  EXEC_PATH,
  PLATFORM,
  PID,
  STDIN,
  STDOUT,
  STDERR,
};

struct ProcessMemberInfo {
  llvm::StringLiteral name;
  ProcessMember value;
};

class ProcessMemberTable {
public:
  static llvm::StringRef getName(ProcessMember value);
  static std::optional<ProcessMember> find(llvm::StringRef name);
};

}
