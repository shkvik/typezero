#include "compiler/semantic/library/process/process_exports/process_exports_errors.h"
#include "compiler/semantic/library/builtin_modules/module_export_errors/module_export_errors.h"

namespace tz {

std::string ErrorTextProcessExport::notYet(llvm::StringRef name, llvm::StringRef path) {
  return ErrorTextModuleExport::notYet(name, path) +
         "; tz has: argv, env, exit, exitCode, cwd, platform, "
         "execPath, stdin, stdout, stderr, uptime and pid";
}

}
