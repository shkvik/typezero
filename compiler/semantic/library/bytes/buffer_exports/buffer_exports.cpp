#include "compiler/semantic/library/bytes/buffer_exports/buffer_exports.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

const llvm::StringLiteral kBufferExports[] = {"Blob",
                                              "File",
                                              "SlowBuffer",
                                              "atob",
                                              "btoa",
                                              "constants",
                                              "isAscii",
                                              "isUtf8",
                                              "Type::kMaxLength",
                                              "kStringMaxLength",
                                              "resolveObjectURL",
                                              "transcode",
                                              "INSPECT_MAX_BYTES"};
}

BufferExports::ResultExportFound BufferExports::bufferExport(llvm::StringRef name,
                                                             llvm::StringRef path) {
  if (name == "Buffer") {
    return ok(name);
  }
  if (llvm::is_contained(kBufferExports, name)) {
    std::string message = ErrorTextBufferExport::notYet(name, path);

    return err(ModuleExportFoundError{
        .code = ModuleExportFoundErrorCode::NOT_YET,
        .message = message,
    });
  }
  std::string message = ErrorTextModuleExport::none(path, name);

  return err(ModuleExportFoundError{
      .code = ModuleExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

}
