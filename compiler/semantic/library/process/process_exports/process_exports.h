#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/feature/feature_errors.h"
#include "compiler/semantic/library/process/process_exports/process_exports_errors.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

class ProcessExports {
public:
  using ResultExportFound = Result<llvm::StringRef, FeatureExportFoundError>;

  static ResultExportFound processExport(llvm::StringRef name, llvm::StringRef path);
};

}
