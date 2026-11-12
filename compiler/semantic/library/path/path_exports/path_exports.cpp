#include "compiler/semantic/library/path/path_exports/path_exports.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

const llvm::StringLiteral kPathFunctions[] = {
    "join",    "resolve",  "normalize", "isAbsolute", "dirname",         "basename",
    "extname", "relative", "parse",     "format",     "toNamespacedPath"};

const llvm::StringLiteral kPathExports[] = {"sep",   "delimiter",   "posix",
                                            "win32", "matchesGlob", "_makeLong"};

}

PathExports::ResultExportFound PathExports::pathExport(llvm::StringRef name, llvm::StringRef path) {
  if (llvm::is_contained(kPathFunctions, name) || name == "sep" || name == "delimiter" ||
      name == "posix" || name == "win32") {
    return ok(name);
  }
  if (llvm::is_contained(kPathExports, name)) {
    auto message = ErrorTextModuleExport::notYet(name, path);

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
