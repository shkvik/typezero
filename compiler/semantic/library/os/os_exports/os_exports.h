#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/feature/feature_errors.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

class OsExports {
public:
  using ResultExportFound = Result<llvm::StringRef, FeatureExportFoundError>;

  static ResultExportFound osExport(llvm::StringRef name, llvm::StringRef path);
};

}
