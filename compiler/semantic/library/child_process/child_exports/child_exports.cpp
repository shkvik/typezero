#include "compiler/semantic/library/child_process/child_exports/child_exports.h"
#include "compiler/program/library/child_process/child_export_table/child_export_table.h"

namespace tz {

ChildExports::ResultExportFound ChildExports::checkChildExport(llvm::StringRef name,
                                                               llvm::StringRef path) {
  auto exported = ChildExportTable::find(name);
  if (exported == ChildExport::SYNC_FUNCTION) {
    return ok(name);
  }
  if (exported == ChildExport::ASYNC_FUNCTION) {
    auto message = ErrorTextChildExport::async(name, path);

    return err(ModuleExportFoundError{
        .code = ModuleExportFoundErrorCode::NOT_YET,
        .message = message,
    });
  }
  if (exported == ChildExport::FORK) {
    auto message = ErrorTextChildExport::fork(name, path);

    return err(ModuleExportFoundError{
        .code = ModuleExportFoundErrorCode::NOT_POSSIBLE,
        .message = message,
    });
  }
  if (exported == ChildExport::NOT_YET) {
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
