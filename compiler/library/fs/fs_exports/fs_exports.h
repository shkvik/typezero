#pragma once

#include "compiler/library/feature/feature_errors.h"
#include "compiler/library/fs/fs_exports/fs_exports_errors.h"
#include "compiler/result/result/result.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

class FsExports {
public:
  using ResultExportFound = Result<llvm::StringRef, FeatureExportFoundError>;

  static ResultExportFound fsExport(llvm::StringRef name, llvm::StringRef path);
  static ResultExportFound tzIoExport(llvm::StringRef name, llvm::StringRef path);
  static ResultExportFound tzFsExport(llvm::StringRef name, llvm::StringRef path);
  static ResultExportFound promisesExport(llvm::StringRef name, llvm::StringRef path);
  static ResultExportFound readlineExport(llvm::StringRef name, llvm::StringRef path,
                                          bool hasPromises);
};

}
