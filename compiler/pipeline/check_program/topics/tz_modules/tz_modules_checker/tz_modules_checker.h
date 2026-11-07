#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/tz_modules/tz_modules_checker/tz_modules_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class TzModulesChecker {
public:
  explicit TzModulesChecker(CheckState &owner);

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

  CheckState &state;
};

}
