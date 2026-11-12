#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/builtin_modules/module_export_errors/module_export_errors.h"
#include "compiler/semantic/library/fs/fs_exports/fs_exports_errors.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

class FsExports {
public:
  using ResultExportFound = Result<llvm::StringRef, ModuleExportFoundError>;

  static ResultExportFound checkFsExport(llvm::StringRef name, llvm::StringRef path);
  static ResultExportFound checkTzIoExport(llvm::StringRef name, llvm::StringRef path);
  static ResultExportFound checkTzFsExport(llvm::StringRef name, llvm::StringRef path);
  static ResultExportFound checkPromisesExport(llvm::StringRef name, llvm::StringRef path);
  static ResultExportFound checkReadlineExport(llvm::StringRef name, llvm::StringRef path,
                                               bool hasPromises);
};

}
