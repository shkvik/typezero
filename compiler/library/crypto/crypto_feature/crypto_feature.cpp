#include "compiler/library/crypto/crypto_feature/crypto_feature.h"
#include "compiler/check/sema/sema.h"
#include "compiler/lower/codegen/codegen.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/library/crypto/crypto_checker/crypto_checker.h"
#include "compiler/library/crypto/crypto_method_checker/crypto_method_checker.h"
#include "compiler/library/crypto/web_crypto_checker/web_crypto_checker.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "compiler/library/crypto/subtle_call_checker/subtle_call_checker.h"
#include "compiler/library/crypto/crypto_lowerer/crypto_lowerer.h"

namespace tz {

Expr *CryptoFeature::checkStaticCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const {
  if (!sema.callChecker().isGlobalName(member->object, "crypto")) {
    return nullptr;
  }

  return sema.cryptoChecker().checkWebCryptoCall(call, member);
}

Expr *CryptoFeature::checkMethodCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const {
  const Type *receiver = member->object->type;
  if (!receiver->isCrypto()) {
    return nullptr;
  }

  return sema.cryptoMethodChecker().checkCryptoMethod(call, member);
}

Expr *CryptoFeature::checkClassMethodCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const {
  const Type *receiver = member->object->type;
  // crypto.subtle, a SubtleCrypto of tz:webcrypto: its algorithms are checked where they are
  // written, outside the module itself.
  if (!receiver->isClass() || !sema.webCryptoChecker().isSubtleClass(receiver, "SubtleCrypto") ||
      sema.libraryCalls().inLibrary(call->loc())) {
    return nullptr;
  }

  return sema.subtleCallChecker().checkSubtleCall(call, member, expected);
}

bool CryptoFeature::lowers(Builtin builtin) const {
  return isCryptoBuiltin(builtin);
}

llvm::Value *CryptoFeature::lowerCall(CodeGen &codegen, CallExpr *call) const {
  return codegen.cryptoLowerer().emitCryptoCall(call);
}

} // namespace tz
