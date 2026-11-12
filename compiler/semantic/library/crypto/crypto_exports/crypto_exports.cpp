#include "compiler/semantic/library/crypto/crypto_exports/crypto_exports.h"
#include "compiler/program/library/crypto/crypto_async_function_table/crypto_async_function_table.h"
#include "compiler/program/library/crypto/crypto_class_table/crypto_class_table.h"
#include "compiler/program/library/crypto/crypto_export_table/crypto_export_table.h"
#include "compiler/program/library/crypto/crypto_sync_function_table/crypto_sync_function_table.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

CryptoExports::ResultExportFound CryptoExports::checkCryptoExport(llvm::StringRef name,
                                                                  llvm::StringRef path) {
  const CryptoAsyncFunctionInfo *async = CryptoAsyncFunctionTable::find(name);
  bool isFunction = CryptoSyncFunctionTable::find(name).has_value() ||
                    (async != nullptr && async->module == NodeModule::Crypto);
  if (isFunction || CryptoClassTable::find(name).has_value()) {
    return ok(name);
  }
  auto exported = CryptoExportTable::find(name);
  if (exported == CryptoExport::WEBCRYPTO) {
    auto message = ErrorTextCryptoExport::webcrypto(name, path);

    return err(ModuleExportFoundError{
        .code = ModuleExportFoundErrorCode::ELSEWHERE,
        .message = message,
    });
  }
  if (exported == CryptoExport::SUBTLE) {
    auto message = ErrorTextCryptoExport::subtle(name, path);

    return err(ModuleExportFoundError{
        .code = ModuleExportFoundErrorCode::ELSEWHERE,
        .message = message,
    });
  }
  if (exported == CryptoExport::NOT_YET) {
    auto message = ErrorTextModuleExport::notYet(name, path);

    return err(ModuleExportFoundError{
        .code = ModuleExportFoundErrorCode::NOT_YET,
        .message = message,
    });
  }
  auto message = ErrorTextModuleExport::none(path, name);

  return err(ModuleExportFoundError{
      .code = ModuleExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

CryptoExports::ResultExportFound CryptoExports::checkTzCryptoExport(llvm::StringRef name,
                                                                    llvm::StringRef path) {
  const CryptoAsyncFunctionInfo *async = CryptoAsyncFunctionTable::find(name);
  if (async != nullptr && async->module == NodeModule::TzCrypto) {
    return ok(name);
  }
  auto message = ErrorTextModuleExport::none(path, name);

  return err(ModuleExportFoundError{
      .code = ModuleExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

}
