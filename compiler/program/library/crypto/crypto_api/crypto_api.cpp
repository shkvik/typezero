#include "compiler/program/library/crypto/crypto_api/crypto_api.h"

namespace tz {

bool CryptoApi::isFullCryptoFunction(llvm::StringRef name) {
  return name.starts_with("tz_openssl_");
}

}
