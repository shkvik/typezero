#include "compiler/semantic/library/builtin_modules/timers_exports/timers_exports.h"
#include "compiler/program/library/async/timers_promises_function_table/timers_promises_function_table.h"

namespace tz {

TimersExports::ResultExportFound TimersExports::checkTimersExport(llvm::StringRef name,
                                                                  llvm::StringRef path) {
  if (TimersPromisesFunctionTable::find(name).has_value()) {
    return ok(name);
  }
  auto message = ErrorTextTimersExport::none(path, name);

  return err(ModuleExportFoundError{
      .code = ModuleExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

}
