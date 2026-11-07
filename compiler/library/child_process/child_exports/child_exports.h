#pragma once

#include "compiler/library/child_process/child_exports/child_exports_errors.h"
#include "compiler/library/feature/feature_errors.h"
#include "compiler/result/result/result.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

class ChildExports {
public:
  using ResultExportFound = Result<llvm::StringRef, FeatureExportFoundError>;

  static ResultExportFound childExport(llvm::StringRef name, llvm::StringRef path);
};

}
