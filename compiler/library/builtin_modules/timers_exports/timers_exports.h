#pragma once

#include "compiler/library/builtin_modules/timers_exports/timers_exports_errors.h"
#include "compiler/library/feature/feature_errors.h"
#include "compiler/result/result/result.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

class TimersExports {
public:
  using ResultExportFound = Result<llvm::StringRef, FeatureExportFoundError>;

  static ResultExportFound timersExport(llvm::StringRef name, llvm::StringRef path);
};

}
