#include "compiler/library/crypto/crypto_exports/crypto_exports.h"
#include "compiler/library/crypto/crypto_names/crypto_names.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

CryptoExports::ResultExportFound CryptoExports::cryptoExport(llvm::StringRef name,
                                                             llvm::StringRef path) {
  if (llvm::is_contained(kFunctions, name) || llvm::is_contained(kClasses, name) ||
      name == "constants") {
    return ok(name);
  }
  if (name == "webcrypto") {
    std::string message = ErrorTextCryptoExport::webcrypto(name, path);

    return err(FeatureExportFoundError{
        .code = FeatureExportFoundErrorCode::ELSEWHERE,
        .message = message,
    });
  }
  if (name == "subtle") {
    std::string message = ErrorTextCryptoExport::subtle(name, path);

    return err(FeatureExportFoundError{
        .code = FeatureExportFoundErrorCode::ELSEWHERE,
        .message = message,
    });
  }
  if (llvm::is_contained(kExports, name)) {
    std::string message = ErrorTextModuleExport::notYet(name, path);

    return err(FeatureExportFoundError{
        .code = FeatureExportFoundErrorCode::NOT_YET,
        .message = message,
    });
  }
  std::string message = ErrorTextModuleExport::none(path, name);

  return err(FeatureExportFoundError{
      .code = FeatureExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

CryptoExports::ResultExportFound CryptoExports::tzCryptoExport(llvm::StringRef name,
                                                               llvm::StringRef path) {
  for (const auto &form : kTzCryptoAsyncForms) {
    if (name == form.first) {
      return ok(name);
    }
  }
  std::string message = ErrorTextModuleExport::none(path, name);

  return err(FeatureExportFoundError{
      .code = FeatureExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

}
