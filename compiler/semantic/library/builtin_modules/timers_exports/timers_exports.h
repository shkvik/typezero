#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/builtin_modules/module_export_errors/module_export_errors.h"
#include "compiler/semantic/library/builtin_modules/timers_exports/timers_exports_errors.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

class TimersExports {
public:
  using ResultExportFound = Result<llvm::StringRef, ModuleExportFoundError>;

  static ResultExportFound checkTimersExport(llvm::StringRef name, llvm::StringRef path);
};

}
