#include "compiler/semantic/library/crypto/crypto_exports/crypto_exports_errors.h"
#include "compiler/semantic/library/builtin_modules/module_export_errors/module_export_errors.h"

namespace tz {

std::string ErrorTextCryptoExport::webcrypto(llvm::StringRef name, llvm::StringRef path) {
  return "'" + name.str() + "' of '" + path.str() +
         "' is the global crypto of Web Crypto, which needs no import: use "
         "'crypto.subtle', 'crypto.getRandomValues' and 'crypto.randomUUID'";
}

std::string ErrorTextCryptoExport::subtle(llvm::StringRef name, llvm::StringRef path) {
  return "'" + name.str() + "' of '" + path.str() +
         "' is the global crypto.subtle, which needs no import: use 'crypto.subtle'";
}

}
