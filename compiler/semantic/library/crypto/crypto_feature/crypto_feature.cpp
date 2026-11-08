#include "compiler/semantic/library/crypto/crypto_feature/crypto_feature.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/library/crypto/crypto_checker/crypto_checker.h"
#include "compiler/semantic/library/crypto/crypto_errors/crypto_errors.h"
#include "compiler/semantic/library/crypto/crypto_exports/crypto_exports.h"
#include "compiler/semantic/library/crypto/crypto_function_checker/crypto_function_checker.h"
#include "compiler/semantic/library/crypto/crypto_method_checker/crypto_method_checker.h"
#include "compiler/semantic/library/crypto/crypto_names/crypto_names.h"
#include "compiler/semantic/library/crypto/subtle_call_checker/subtle_call_checker.h"
#include "compiler/semantic/library/crypto/web_crypto_checker/web_crypto_checker.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

void CryptoFeature::declare(SemanticState &state) const {
  CryptoErrors(state).declare();
}

Expr *CryptoFeature::checkStaticCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                                     const Type *) const {
  if (CryptoChecker(state).isX509Class(member->object)) {
    return CryptoChecker(state).checkX509Parse(call, member);
  }
  if (!CallChecker(state).isGlobalName(member->object, "crypto")) {
    return nullptr;
  }

  return CryptoChecker(state).checkWebCryptoCall(call, member);
}

Expr *CryptoFeature::checkMethodCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                                     const Type *) const {
  const Type *receiver = member->object->type;
  if (!receiver->isCrypto()) {
    return nullptr;
  }

  return CryptoMethodChecker(state).checkCryptoMethod(call, member);
}

Expr *CryptoFeature::checkClassMethodCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                                          const Type *expected) const {
  const Type *receiver = member->object->type;
  if (!receiver->isClass() || !WebCryptoChecker(state).isSubtleClass(receiver, "SubtleCrypto") ||
      LibraryCalls(state).inLibrary(call->loc())) {
    return nullptr;
  }

  return SubtleCallChecker(state).checkSubtleCall(call, member, expected);
}

CryptoFeature::ResultExportFound CryptoFeature::exportFound(NodeModule module,
                                                            llvm::StringRef exportName,
                                                            llvm::StringRef path) const {
  if (module == NodeModule::TzCrypto) {
    return CryptoExports::tzCryptoExport(exportName, path);
  }
  if (module == NodeModule::Crypto) {
    return CryptoExports::cryptoExport(exportName, path);
  }

  return err(FeatureExportFoundError{
      .code = FeatureExportFoundErrorCode::NOT_MINE,
      .message = "",
  });
}

Expr *CryptoFeature::checkModuleCall(SemanticState &state, CallExpr *call, NodeModule module,
                                     llvm::StringRef fn, llvm::SMLoc loc, const Type *) const {
  if (module != NodeModule::Crypto && module != NodeModule::TzCrypto) {
    return nullptr;
  }

  return CryptoFunctionChecker(state).checkCryptoCall(call, fn, loc);
}

Expr *CryptoFeature::checkModuleValue(SemanticState &state, NodeModule module,
                                      llvm::StringRef exportName, Expr *expr) const {
  if (module != NodeModule::Crypto) {
    return nullptr;
  }

  return CryptoChecker(state).checkCryptoValue(exportName, expr);
}

Expr *CryptoFeature::checkModuleMember(SemanticState &state, MemberExpr *member) const {
  return CryptoChecker(state).checkCryptoModuleMember(member);
}

}
