#include "compiler/program/library/child_process/child_error_names/child_error_names.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const ChildErrorInfo kErrors[] = {
    {
        .name = "ExecTextError",
        .function = ChildFunction::EXEC_SYNC,
        .isShell = false,
        .isText = true,
    },
    {
        .name = "ExecBytesError",
        .function = ChildFunction::EXEC_SYNC,
        .isShell = false,
        .isText = false,
    },
    {
        .name = "ExecFileTextError",
        .function = ChildFunction::EXEC_FILE_SYNC,
        .isShell = false,
        .isText = true,
    },
    {
        .name = "ExecFileBytesError",
        .function = ChildFunction::EXEC_FILE_SYNC,
        .isShell = false,
        .isText = false,
    },
    {
        .name = "SpawnTextError",
        .function = ChildFunction::SPAWN_SYNC,
        .isShell = false,
        .isText = true,
    },
    {
        .name = "SpawnBytesError",
        .function = ChildFunction::SPAWN_SYNC,
        .isShell = false,
        .isText = false,
    },
    {
        .name = "ExecFileShellTextError",
        .function = ChildFunction::EXEC_FILE_SYNC,
        .isShell = true,
        .isText = true,
    },
    {
        .name = "ExecFileShellBytesError",
        .function = ChildFunction::EXEC_FILE_SYNC,
        .isShell = true,
        .isText = false,
    },
    {
        .name = "SpawnShellTextError",
        .function = ChildFunction::SPAWN_SYNC,
        .isShell = true,
        .isText = true,
    },
    {
        .name = "SpawnShellBytesError",
        .function = ChildFunction::SPAWN_SYNC,
        .isShell = true,
        .isText = false,
    },
};

const ChildResultInfo kResults[] = {
    {.name = "SpawnTextResult", .isText = true},
    {.name = "SpawnBytesResult", .isText = false},
};
}

std::vector<llvm::StringRef> ChildErrorNames::getTypeNames() {
  std::vector<llvm::StringRef> names;
  for (const ChildErrorInfo &info : kErrors) {
    names.push_back(info.name);
  }
  for (const ChildResultInfo &info : kResults) {
    names.push_back(info.name);
  }

  return names;
}

llvm::StringRef ChildErrorNames::getErrorName(ChildFunction function, bool isText, bool isShell) {
  bool isNamedShell = function != ChildFunction::EXEC_SYNC && isShell;
  for (const ChildErrorInfo &info : kErrors) {
    if (info.function == function && info.isText == isText && info.isShell == isNamedShell) {
      return info.name;
    }
  }
  llvm_unreachable("child error missing from the table");
}

llvm::StringRef ChildErrorNames::getResultName(bool isText) {
  for (const ChildResultInfo &info : kResults) {
    if (info.isText == isText) {
      return info.name;
    }
  }
  llvm_unreachable("child result missing from the table");
}

}
