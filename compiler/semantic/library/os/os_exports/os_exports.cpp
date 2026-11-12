#include "compiler/semantic/library/os/os_exports/os_exports.h"
#include "compiler/program/library/builtin_modules/pending_export_table/pending_export_table.h"
#include "compiler/program/library/os/os_function_table/os_function_table.h"
#include "compiler/program/library/os/os_value_table/os_value_table.h"

namespace tz {

OsExports::ResultExportFound OsExports::checkOsExport(llvm::StringRef name, llvm::StringRef path) {
  if (OsFunctionTable::find(name).has_value() || OsValueTable::find(name).has_value()) {
    return ok(name);
  }
  if (PendingExportTable::isListed(NodeModule::Os, name)) {
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
