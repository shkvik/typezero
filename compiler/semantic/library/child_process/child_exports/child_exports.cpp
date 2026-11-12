#include "compiler/semantic/library/child_process/child_exports/child_exports.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

const llvm::StringLiteral kChildExports[] = {"ChildProcess", "_forkChild",   "exec",
                                             "execFile",     "execFileSync", "execSync",
                                             "fork",         "spawn",        "spawnSync"};

}

ChildExports::ResultExportFound ChildExports::checkChildExport(llvm::StringRef name,
                                                               llvm::StringRef path) {
  if (name == "spawnSync" || name == "execFileSync" || name == "execSync") {
    return ok(name);
  }
  if (name == "spawn" || name == "exec" || name == "execFile") {
    auto message = ErrorTextChildExport::async(name, path);

    return err(ModuleExportFoundError{
        .code = ModuleExportFoundErrorCode::NOT_YET,
        .message = message,
    });
  }
  if (name == "fork") {
    auto message = ErrorTextChildExport::fork(name, path);

    return err(ModuleExportFoundError{
        .code = ModuleExportFoundErrorCode::NOT_POSSIBLE,
        .message = message,
    });
  }
  if (llvm::is_contained(kChildExports, name)) {
    auto message = ErrorTextChildExport::notYet(name, path);

    return err(ModuleExportFoundError{
        .code = ModuleExportFoundErrorCode::NOT_YET,
        .message = message,
    });
  }
  auto message = ErrorTextModuleExport::none(path, name);

  return err(ModuleExportFoundError{
      .code = ModuleExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

}
