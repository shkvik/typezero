#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/tz_modules/tz_modules_checker/tz_modules_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class LibraryCalls;
class ReturnInferrer;

class TzModulesChecker {
public:
  TzModulesChecker(SemanticState &owner, SemanticParts &all);

  Expr *adaptResult(Expr *expr, const Type *to);
  void checkExternal(FunctionDecl *fn);
  void noteHttpsCall(CallExpr *call, const FunctionDecl *fn);
  void noteServer(const NewExpr *made);
  void noteServeOptions(const Expr *options);
  void noteHttpsValue(const FunctionDecl *fn, llvm::SMLoc loc);
  void noteTlsMethod(const MemberExpr *member, const ClassDecl *cls);
  std::string knownPrefix(const Expr *expr) const;

private:
  static const Expr *uncast(const Expr *expr);
  static bool plainTransport(const Expr *transport);
  using ResultExternalParams = Result<const Type *, TzModulesCheckerExternalParamError>;
  using ResultExternalReturn = Result<const Type *, TzModulesCheckerExternalReturnError>;
  using ResultPlainSignature = Result<const FunctionDecl *, TzModulesCheckerPlainSignatureError>;

  static ResultPlainSignature plainSignature(const FunctionDecl *fn);
  static ResultExternalParams externalParam(const Param &param, const Type *type);
  static ResultExternalReturn externalReturn(const Type *ret, const Type *declared);

  static bool isExternalReference(const Type *type);
  static bool isExternalCallback(const Type *type);
  static bool isExternalValue(const Type *type);

  SemanticState &state;
  LibraryCalls *libraryCalls = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
};

}
