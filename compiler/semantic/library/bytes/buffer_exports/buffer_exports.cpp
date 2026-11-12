#include "compiler/semantic/library/bytes/buffer_exports/buffer_exports.h"
#include "compiler/program/library/bytes/buffer_export_table/buffer_export_table.h"

namespace tz {

BufferExports::ResultExportFound BufferExports::checkBufferExport(llvm::StringRef name,
                                                                  llvm::StringRef path) {
  auto exported = BufferExportTable::find(name);
  if (exported == BufferExport::BUFFER) {
    return ok(name);
  }
  if (exported == BufferExport::NOT_YET) {
    auto message = ErrorTextBufferExport::notYet(name, path);

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
