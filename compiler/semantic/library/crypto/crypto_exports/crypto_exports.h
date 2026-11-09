#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/builtin_modules/module_export_errors/module_export_errors.h"
#include "compiler/semantic/library/crypto/crypto_exports/crypto_exports_errors.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

class CryptoExports {
public:
  using ResultExportFound = Result<llvm::StringRef, ModuleExportFoundError>;

  static ResultExportFound cryptoExport(llvm::StringRef name, llvm::StringRef path);
  static ResultExportFound tzCryptoExport(llvm::StringRef name, llvm::StringRef path);
};

}
