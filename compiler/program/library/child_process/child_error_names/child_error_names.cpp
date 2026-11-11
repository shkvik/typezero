#include "compiler/program/library/child_process/child_error_names/child_error_names.h"

namespace tz {

namespace {

const llvm::StringLiteral kChildTypeNames[] = {
    "ExecTextError",          "ExecBytesError",          "ExecFileTextError",
    "ExecFileBytesError",     "SpawnTextError",          "SpawnBytesError",
    "ExecFileShellTextError", "ExecFileShellBytesError", "SpawnShellTextError",
    "SpawnShellBytesError",   "SpawnTextResult",         "SpawnBytesResult"};

}

std::vector<llvm::StringRef> ChildErrorNames::getTypeNames() {
  std::vector<llvm::StringRef> names;
  for (llvm::StringLiteral name : kChildTypeNames) {
    names.push_back(name);
  }

  return names;
}

}
