#include "compiler/semantic/library/process/process_exports/process_exports.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

const llvm::StringLiteral kProcessNames[] = {"argv",     "env",      "exit",   "exitCode",
                                             "cwd",      "platform", "execPath", "stdin",
                                             "stdout",   "stderr",   "uptime", "pid"};

}

ProcessExports::ResultExportFound ProcessExports::processExport(llvm::StringRef name,
                                                                llvm::StringRef path) {
  if (llvm::is_contained(kProcessNames, name)) {
    return ok(name);
  }
  std::string message = ErrorTextProcessExport::notYet(name, path);

  return err(ModuleExportFoundError{
      .code = ModuleExportFoundErrorCode::NOT_YET,
      .message = message,
  });
}

}
