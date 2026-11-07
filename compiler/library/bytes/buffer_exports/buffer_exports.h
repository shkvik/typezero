#pragma once

#include "compiler/library/bytes/buffer_exports/buffer_exports_errors.h"
#include "compiler/library/feature/feature_errors.h"
#include "compiler/result/result/result.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

class BufferExports {
public:
  using ResultExportFound = Result<llvm::StringRef, FeatureExportFoundError>;

  static ResultExportFound bufferExport(llvm::StringRef name, llvm::StringRef path);
};

}
