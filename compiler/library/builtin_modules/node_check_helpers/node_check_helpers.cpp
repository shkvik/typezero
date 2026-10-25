#include "compiler/library/builtin_modules/node_check_helpers/node_check_helpers.h"
#include "compiler/library/crypto/crypto_names/crypto_names.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

std::string exportProblem(NodeModule module, llvm::StringRef name, llvm::StringRef path) {
  std::string quoted = "'" + path.str() + "'";
  std::string what = "'" + name.str() + "' of " + quoted;
  switch (module) {
  case NodeModule::Fs: {
    if (llvm::is_contained(kFsFunctions, name) || name == "constants" || name == "Stats" ||
        name == "Dirent") {
      return "";
    }
    std::string sync = (name + "Sync").str();
    if (llvm::is_contained(kFsFunctions, sync)) {
      // The one of node:fs/promises gives a Promise instead.
      if (llvm::is_contained(kFspFunctions, name)) {
        return what + " takes a callback, which tz does not have; import '" + name.str() +
               "' from \"node:fs/promises\", which gives a Promise, or '" + sync +
               "', which gives what it reads" +
               (name == "readFile" ? ", e.g. 'const text = await readFile(path, \"utf8\")'" : "");
      }
      return what + " takes a callback, which tz does not have; import '" + sync +
             "' and use what it returns";
    }
    if (name == "promises") {
      return "";
    }
    if (llvm::is_contained(kFsExports, name)) {
      return what + " is not supported yet";
    }
    return quoted + " has no export named '" + name.str() + "'";
  }
  case NodeModule::FsPromises:
    if (llvm::is_contained(kFspFunctions, name) || name == "constants") {
      return "";
    }
    if (llvm::is_contained(kFspExports, name)) {
      return what + " is not supported yet";
    }
    if (llvm::is_contained(kFsFunctions, name)) {
      return quoted + " has no export named '" + name.str() + "'; the function that waits is '" +
             name.drop_back(4).str() + "', or import '" + name.str() + "' from \"node:fs\"";
    }
    return quoted + " has no export named '" + name.str() + "'";
  case NodeModule::TimersPromises:
    if (name == "setTimeout" || name == "setImmediate" || name == "setInterval" ||
        name == "scheduler") {
      return "";
    }
    return quoted + " has no export named '" + name.str() +
           "'; it has setTimeout, setImmediate, setInterval and scheduler";
  case NodeModule::Readline:
  case NodeModule::ReadlinePromises:
    if (name == "createInterface" || name == "Interface") {
      return "";
    }
    if (name == "clearLine" || name == "clearScreenDown" || name == "cursorTo" ||
        name == "emitKeypressEvents" || name == "moveCursor" || name == "Readline" ||
        (module == NodeModule::Readline && name == "promises")) {
      return what + " is not supported yet; tz has createInterface";
    }
    return quoted + " has no export named '" + name.str() + "'";
  case NodeModule::Path:
  case NodeModule::PathPosix:
  case NodeModule::PathWin32:
    if (llvm::is_contained(kPathFunctions, name) || name == "sep" || name == "delimiter" ||
        name == "posix" || name == "win32") {
      return "";
    }
    if (llvm::is_contained(kPathExports, name)) {
      return what + " is not supported yet";
    }
    return quoted + " has no export named '" + name.str() + "'";
  case NodeModule::Os:
    if (llvm::is_contained(kOsFunctions, name) || name == "EOL" || name == "devNull") {
      return "";
    }
    if (llvm::is_contained(kOsExports, name)) {
      return what + " is not supported yet";
    }
    return quoted + " has no export named '" + name.str() + "'";
  case NodeModule::Process:
    if (llvm::is_contained(kProcessNames, name)) {
      return "";
    }
    return what + " is not supported yet; tz has: argv, env, exit, exitCode, cwd, platform, "
                  "execPath, stdin, stdout and stderr";
  case NodeModule::Buffer:
    if (name == "Buffer") {
      return "";
    }
    if (llvm::is_contained(kBufferExports, name)) {
      return what + " is not supported yet; tz has Buffer";
    }
    return quoted + " has no export named '" + name.str() + "'";
  case NodeModule::TzIo:
    if (name == "readLine") {
      return "";
    }
    return quoted + " has no export named '" + name.str() + "'; it has readLine";
  case NodeModule::ChildProcess:
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
  case NodeModule::Crypto:
    return cryptoExportProblem(name, path);
  }

  return "";
}

std::string moduleLabel(NodeModule module) {
  switch (module) {
  case NodeModule::Fs:
    return "node:fs";
  case NodeModule::FsPromises:
    return "node:fs/promises";
  case NodeModule::TimersPromises:
    return "node:timers/promises";
  case NodeModule::Readline:
    return "node:readline";
  case NodeModule::ReadlinePromises:
    return "node:readline/promises";
  case NodeModule::Path:
    return "node:path";
  case NodeModule::PathPosix:
    return "node:path/posix";
  case NodeModule::PathWin32:
    return "node:path/win32";
  case NodeModule::Os:
    return "node:os";
  case NodeModule::Process:
    return "node:process";
  case NodeModule::Buffer:
    return "node:buffer";
  case NodeModule::TzIo:
    return "tz:io";
  case NodeModule::ChildProcess:
    return "node:child_process";
  case NodeModule::Crypto:
    return "node:crypto";
  }

  return "";
}

} // namespace tz
