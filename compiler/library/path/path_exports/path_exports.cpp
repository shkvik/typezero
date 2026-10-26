#include "compiler/library/path/path_exports/path_exports.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

const llvm::StringLiteral kPathFunctions[] = {
    "join",    "resolve",  "normalize", "isAbsolute", "dirname",         "basename",
    "extname", "relative", "parse",     "format",     "toNamespacedPath"};

const llvm::StringLiteral kPathExports[] = {"sep",   "delimiter",   "posix",
                                            "win32", "matchesGlob", "_makeLong"};

}

std::string pathExportProblem(llvm::StringRef name, llvm::StringRef path) {
  std::string quoted = "'" + path.str() + "'";
  if (llvm::is_contained(kPathFunctions, name) || name == "sep" || name == "delimiter" ||
      name == "posix" || name == "win32") {
    return "";
  }
  if (llvm::is_contained(kPathExports, name)) {
    return "'" + name.str() + "' of " + quoted + " is not supported yet";
  }

  return quoted + " has no export named '" + name.str() + "'";
}

}
