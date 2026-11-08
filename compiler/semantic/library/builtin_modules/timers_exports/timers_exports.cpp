#include "compiler/semantic/library/builtin_modules/timers_exports/timers_exports.h"

namespace tz {

TimersExports::ResultExportFound TimersExports::timersExport(llvm::StringRef name,
                                                             llvm::StringRef path) {
  if (name == "setTimeout" || name == "setImmediate" || name == "setInterval" ||
      name == "scheduler") {
    return ok(name);
  }
  std::string message = ErrorTextTimersExport::none(path, name);

  return err(FeatureExportFoundError{
      .code = FeatureExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

}
