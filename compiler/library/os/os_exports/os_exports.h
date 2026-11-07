#pragma once

#include "compiler/library/feature/feature_errors.h"
#include "compiler/result/result/result.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

class OsExports {
public:
  using ResultExportFound = Result<llvm::StringRef, FeatureExportFoundError>;

  static ResultExportFound osExport(llvm::StringRef name, llvm::StringRef path);
};

}
