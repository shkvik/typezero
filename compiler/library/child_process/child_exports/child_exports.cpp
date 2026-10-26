#include "compiler/library/child_process/child_exports/child_exports.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

const llvm::StringLiteral kChildExports[] = {"ChildProcess", "_forkChild",   "exec",
                                             "execFile",     "execFileSync", "execSync",
                                             "fork",         "spawn",        "spawnSync"};

}

std::string childExportProblem(llvm::StringRef name, llvm::StringRef path) {
  std::string quoted = "'" + path.str() + "'";
  std::string what = "'" + name.str() + "' of " + quoted;
  if (name == "spawnSync" || name == "execFileSync" || name == "execSync") {
    return "";
  }
  if (name == "spawn" || name == "exec" || name == "execFile") {
    return what +
           " is not supported yet: it comes with EventEmitter and the streams of Node "
           "(docs/process.md, step 2); use '" +
           name.str() + "Sync', which waits for the program and gives what it printed";
  }
  if (name == "fork") {
    return what + " is not possible: it starts another Node with a channel to it, and tz has "
                  "no Node (docs/process.md, section 4); for parallel work use async functions "
                  "(docs/async.md), and to run a program 'spawnSync(process.execPath, [...])'";
  }
  if (llvm::is_contained(kChildExports, name)) {
    return what + " is not supported yet; tz has spawnSync, execFileSync and execSync";
  }

  return quoted + " has no export named '" + name.str() + "'";
}

}
