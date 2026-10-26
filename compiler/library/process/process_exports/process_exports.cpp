#include "compiler/library/process/process_exports/process_exports.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

const llvm::StringLiteral kProcessNames[] = {"argv",     "env",      "exit",  "exitCode", "cwd",
                                             "platform", "execPath", "stdin", "stdout",   "stderr"};

}

std::string processExportProblem(llvm::StringRef name, llvm::StringRef path) {
  if (llvm::is_contained(kProcessNames, name)) {
    return "";
  }

  return "'" + name.str() + "' of '" + path.str() +
         "' is not supported yet; tz has: argv, env, exit, exitCode, cwd, platform, "
         "execPath, stdin, stdout and stderr";
}

}
