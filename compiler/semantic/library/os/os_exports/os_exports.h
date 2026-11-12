#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/builtin_modules/module_export_errors/module_export_errors.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

class OsExports {
public:
  using ResultExportFound = Result<llvm::StringRef, ModuleExportFoundError>;

  static ResultExportFound checkOsExport(llvm::StringRef name, llvm::StringRef path);
};

}
