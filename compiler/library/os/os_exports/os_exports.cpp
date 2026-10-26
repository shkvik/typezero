#include "compiler/library/os/os_exports/os_exports.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

const llvm::StringLiteral kOsFunctions[] = {
    "platform", "homedir", "tmpdir",     "hostname", "cpus",    "availableParallelism",
    "totalmem", "freemem", "arch",       "type",     "release", "version",
    "machine",  "uptime",  "endianness", "loadavg",  "userInfo"};

const llvm::StringLiteral kOsExports[] = {"EOL",         "devNull",           "getPriority",
                                          "setPriority", "networkInterfaces", "constants"};

}

std::string osExportProblem(llvm::StringRef name, llvm::StringRef path) {
  std::string quoted = "'" + path.str() + "'";
  if (llvm::is_contained(kOsFunctions, name) || name == "EOL" || name == "devNull") {
    return "";
  }
  if (llvm::is_contained(kOsExports, name)) {
    return "'" + name.str() + "' of " + quoted + " is not supported yet";
  }

  return quoted + " has no export named '" + name.str() + "'";
}

}
