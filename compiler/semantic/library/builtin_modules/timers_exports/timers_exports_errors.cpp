#include "compiler/semantic/library/builtin_modules/timers_exports/timers_exports_errors.h"
#include "compiler/semantic/feature/feature_errors.h"

namespace tz {

std::string ErrorTextTimersExport::none(llvm::StringRef path, llvm::StringRef name) {
  return ErrorTextModuleExport::none(path, name) +
         "; it has setTimeout, setImmediate, setInterval and scheduler";
}

}
