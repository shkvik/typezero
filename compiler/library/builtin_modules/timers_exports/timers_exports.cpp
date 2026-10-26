#include "compiler/library/builtin_modules/timers_exports/timers_exports.h"

namespace tz {

std::string timersExportProblem(llvm::StringRef name, llvm::StringRef path) {
  if (name == "setTimeout" || name == "setImmediate" || name == "setInterval" ||
      name == "scheduler") {
    return "";
  }

  return "'" + path.str() + "' has no export named '" + name.str() +
         "'; it has setTimeout, setImmediate, setInterval and scheduler";
}

}
