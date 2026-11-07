#include "compiler/library/process/process_exports/process_exports_errors.h"
#include "compiler/library/feature/feature_errors.h"

namespace tz {

std::string ErrorTextProcessExport::notYet(llvm::StringRef name, llvm::StringRef path) {
  return ErrorTextModuleExport::notYet(name, path) +
         "; tz has: argv, env, exit, exitCode, cwd, platform, "
         "execPath, stdin, stdout, stderr, uptime and pid";
}

}
