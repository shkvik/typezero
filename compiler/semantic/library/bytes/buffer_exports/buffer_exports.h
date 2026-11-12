#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/builtin_modules/module_export_errors/module_export_errors.h"
#include "compiler/semantic/library/bytes/buffer_exports/buffer_exports_errors.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

class BufferExports {
public:
  using ResultExportFound = Result<llvm::StringRef, ModuleExportFoundError>;

  static ResultExportFound checkBufferExport(llvm::StringRef name, llvm::StringRef path);
};

}
