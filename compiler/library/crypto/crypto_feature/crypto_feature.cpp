#include "compiler/library/crypto/crypto_feature/crypto_feature.h"
#include "compiler/check/checker/checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/library/crypto/crypto_checker/crypto_checker.h"
#include "compiler/library/crypto/crypto_lowerer/crypto_lowerer.h"
#include "compiler/library/crypto/crypto_method_checker/crypto_method_checker.h"
#include "compiler/library/crypto/subtle_call_checker/subtle_call_checker.h"
#include "compiler/library/crypto/web_crypto_checker/web_crypto_checker.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "compiler/lower/lowering/lowering.h"

namespace tz {

Expr *CryptoFeature::checkStaticCall(Checker &checker, CallExpr *call, MemberExpr *member,
                                     const Type *) const {
  if (!checker.callChecker().isGlobalName(member->object, "crypto")) {
    return nullptr;
  }

  return checker.cryptoChecker().checkWebCryptoCall(call, member);
}

Expr *CryptoFeature::checkMethodCall(Checker &checker, CallExpr *call, MemberExpr *member,
                                     const Type *) const {
  const Type *receiver = member->object->type;
  if (!receiver->isCrypto()) {
    return nullptr;
  }

  return checker.cryptoMethodChecker().checkCryptoMethod(call, member);
}

Expr *CryptoFeature::checkClassMethodCall(Checker &checker, CallExpr *call, MemberExpr *member,
                                          const Type *expected) const {
  const Type *receiver = member->object->type;
  if (!receiver->isClass() || !checker.webCryptoChecker().isSubtleClass(receiver, "SubtleCrypto") ||
      checker.libraryCalls().inLibrary(call->loc())) {
    return nullptr;
  }

  return checker.subtleCallChecker().checkSubtleCall(call, member, expected);
}

bool CryptoFeature::lowers(Builtin builtin) const { return isCryptoBuiltin(builtin); }

llvm::Value *CryptoFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.cryptoLowerer().emitCryptoCall(call);
}

}
