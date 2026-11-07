#include "compiler/library/crypto/crypto_feature/crypto_feature.h"
#include "compiler/library/crypto/crypto_lowerer/crypto_lowerer.h"
#include "compiler/library/crypto/crypto_names/crypto_names.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/crypto/crypto_checker/crypto_checker.h"
#include "compiler/pipeline/check_program/topics/crypto/crypto_error_types/crypto_error_types.h"
#include "compiler/pipeline/check_program/topics/crypto/crypto_function_checker/crypto_function_checker.h"
#include "compiler/pipeline/check_program/topics/crypto/crypto_method_checker/crypto_method_checker.h"
#include "compiler/pipeline/check_program/topics/crypto/subtle_call_checker/subtle_call_checker.h"
#include "compiler/pipeline/check_program/topics/crypto/web_crypto_checker/web_crypto_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/tz_modules/library_calls/library_calls.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

void CryptoFeature::declare(CheckState &state) const {
  CryptoErrorTypes(state).declare();
}

Expr *CryptoFeature::checkStaticCall(CheckState &state, CallExpr *call, MemberExpr *member,
                                     const Type *) const {
  if (CryptoChecker(state).isX509Class(member->object)) {
    return CryptoChecker(state).checkX509Parse(call, member);
  }
  if (!CallChecker(state).isGlobalName(member->object, "crypto")) {
    return nullptr;
  }

  return CryptoChecker(state).checkWebCryptoCall(call, member);
}

Expr *CryptoFeature::checkMethodCall(CheckState &state, CallExpr *call, MemberExpr *member,
                                     const Type *) const {
  const Type *receiver = member->object->type;
  if (!receiver->isCrypto()) {
    return nullptr;
  }

  return CryptoMethodChecker(state).checkCryptoMethod(call, member);
}

Expr *CryptoFeature::checkClassMethodCall(CheckState &state, CallExpr *call, MemberExpr *member,
                                          const Type *expected) const {
  const Type *receiver = member->object->type;
  if (!receiver->isClass() || !WebCryptoChecker(state).isSubtleClass(receiver, "SubtleCrypto") ||
      LibraryCalls(state).inLibrary(call->loc())) {
    return nullptr;
  }

  return SubtleCallChecker(state).checkSubtleCall(call, member, expected);
}

std::optional<std::string> CryptoFeature::exportProblem(NodeModule module,
                                                        llvm::StringRef exportName,
                                                        llvm::StringRef path) const {
  if (module == NodeModule::TzCrypto) {
    return tzCryptoExportProblem(exportName, path);
  }
  if (module != NodeModule::Crypto) {
    return std::nullopt;
  }

  return cryptoExportProblem(exportName, path);
}

Expr *CryptoFeature::checkModuleCall(CheckState &state, CallExpr *call, NodeModule module,
                                     llvm::StringRef fn, llvm::SMLoc loc, const Type *) const {
  if (module != NodeModule::Crypto && module != NodeModule::TzCrypto) {
    return nullptr;
  }

  return CryptoFunctionChecker(state).checkCryptoCall(call, fn, loc);
}

Expr *CryptoFeature::checkModuleValue(CheckState &state, NodeModule module,
                                      llvm::StringRef exportName, Expr *expr) const {
  if (module != NodeModule::Crypto) {
    return nullptr;
  }

  return CryptoChecker(state).checkCryptoValue(exportName, expr);
}

Expr *CryptoFeature::checkModuleMember(CheckState &state, MemberExpr *member) const {
  return CryptoChecker(state).checkCryptoModuleMember(member);
}

bool CryptoFeature::lowers(Builtin builtin) const { return isCryptoBuiltin(builtin); }

llvm::Value *CryptoFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.cryptoLowerer().emitCryptoCall(call);
}

}
