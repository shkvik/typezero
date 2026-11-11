#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class OsFunction {
  HOMEDIR,
  TMPDIR,
  HOSTNAME,
  ARCH,
  TYPE,
  RELEASE,
  VERSION,
  MACHINE,
  TOTALMEM,
  FREEMEM,
  UPTIME,
  PLATFORM,
  ENDIANNESS,
  AVAILABLE_PARALLELISM,
  CPUS,
  LOADAVG,
  USER_INFO,
};

struct OsFunctionInfo {
  llvm::StringLiteral name;
  OsFunction value;
};

class OsFunctionTable {
public:
  static std::optional<OsFunction> find(llvm::StringRef name);
};

}
