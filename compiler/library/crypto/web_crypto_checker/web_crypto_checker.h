#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Web Crypto: the intrinsics of webcrypto.tz.
class WebCryptoChecker {
public:
  explicit WebCryptoChecker(Checker &checker);

  // Web Crypto (sema_webcrypto.cpp): the intrinsics __tz_subtle_* of
  // library/crypto/webcrypto.tz, the calls of the program on its SubtleCrypto
  // (the algorithms, formats and usages it writes, checked here, and the
  // function of the algorithm's family they go to), `crypto.subtle` and the
  // members of a CryptoKey the program may read.
  Expr *checkSubtleIntrinsic(CallExpr *call, NameExpr *name);
  Expr *subtleObject(MemberExpr *member);
  bool isSubtleClass(const Type *type, llvm::StringRef name) const;

private:
  Checker &checker_;
};

} // namespace tz
