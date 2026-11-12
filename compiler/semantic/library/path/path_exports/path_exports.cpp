#include "compiler/semantic/library/path/path_exports/path_exports.h"
#include "compiler/program/library/builtin_modules/pending_export_table/pending_export_table.h"
#include "compiler/program/library/path/path_function_table/path_function_table.h"
#include "compiler/program/library/path/path_value_table/path_value_table.h"

namespace tz {

PathExports::ResultExportFound PathExports::checkPathExport(llvm::StringRef name,
                                                            llvm::StringRef path) {
  bool isExported = PathFunctionTable::find(name) != nullptr ||
                    PathValueTable::find(name).has_value() ||
                    KnownModules::findSubmodule(NodeModule::Path, name).has_value();
  if (isExported) {
    return ok(name);
  }
  if (PendingExportTable::isListed(NodeModule::Path, name)) {
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
