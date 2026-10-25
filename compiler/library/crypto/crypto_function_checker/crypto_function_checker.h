#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The functions of node:crypto (docs/crypto.md).
class CryptoFunctionChecker {
public:
  explicit CryptoFunctionChecker(Sema &sema);

  // node:crypto (sema_crypto.cpp, docs/crypto.md): the calls of its functions,
  // the methods and properties of its objects, `new X509Certificate`, the
  // global crypto of Web Crypto, and its classes as types. Each becomes
  // Builtin::Crypto (see crypto_api.h), or Builtin::CryptoKeyPair for
  // generateKeyPairSync.
  Expr *checkCryptoCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);

private:
  Sema &sema_;
};

} // namespace tz
