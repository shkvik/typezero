#include "compiler/semantic/library/child_process/child_exports/child_exports_errors.h"
#include "compiler/semantic/library/builtin_modules/module_export_errors/module_export_errors.h"

namespace tz {

std::string ErrorTextChildExport::async(llvm::StringRef name, llvm::StringRef path) {
  return "'" + name.str() + "' of '" + path.str() +
         "' is not supported yet: it comes with EventEmitter and the streams of Node "
         "(docs/process.md, step 2); use '" +
         name.str() + "Sync', which waits for the program and gives what it printed";
}

std::string ErrorTextChildExport::fork(llvm::StringRef name, llvm::StringRef path) {
  return "'" + name.str() + "' of '" + path.str() +
         "' is not possible: it starts another Node with a channel to it, and tz has "
         "no Node (docs/process.md, section 4); for parallel work use async functions "
         "(docs/async.md), and to run a program 'spawnSync(process.execPath, [...])'";
}

std::string ErrorTextChildExport::notYet(llvm::StringRef name, llvm::StringRef path) {
  return ErrorTextModuleExport::notYet(name, path) +
         "; tz has spawnSync, execFileSync and execSync";
}

}
