#include "compiler/semantic/library/child_process/child_exports/child_exports.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

const llvm::StringLiteral kChildExports[] = {"ChildProcess", "_forkChild",   "exec",
                                             "execFile",     "execFileSync", "execSync",
                                             "fork",         "spawn",        "spawnSync"};

}

ChildExports::ResultExportFound ChildExports::childExport(llvm::StringRef name,
                                                          llvm::StringRef path) {
  if (name == "spawnSync" || name == "execFileSync" || name == "execSync") {
    return ok(name);
  }
  if (name == "spawn" || name == "exec" || name == "execFile") {
    std::string message = ErrorTextChildExport::async(name, path);

    return err(FeatureExportFoundError{
        .code = FeatureExportFoundErrorCode::NOT_YET,
        .message = message,
    });
  }
  if (name == "fork") {
    std::string message = ErrorTextChildExport::fork(name, path);

    return err(FeatureExportFoundError{
        .code = FeatureExportFoundErrorCode::NOT_POSSIBLE,
        .message = message,
    });
  }
  if (llvm::is_contained(kChildExports, name)) {
    std::string message = ErrorTextChildExport::notYet(name, path);

    return err(FeatureExportFoundError{
        .code = FeatureExportFoundErrorCode::NOT_YET,
        .message = message,
    });
  }
  std::string message = ErrorTextModuleExport::none(path, name);

  return err(FeatureExportFoundError{
      .code = FeatureExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

}
