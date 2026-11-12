#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/tz_modules/tz_modules_checker/tz_modules_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class LibraryCalls;

class TzModulesChecker {
public:
  TzModulesChecker(SemanticState &owner, SemanticParts &all);

  Expr *adaptResult(Expr *expr, const Type *to);
  void walkExternal(FunctionDecl *functionDecl);
  void noteHttpsCall(CallExpr *call, const FunctionDecl *functionDecl);
  void noteServer(const NewExpr *made);
  void noteServeOptions(const Expr *options);
  void noteHttpsValue(const FunctionDecl *functionDecl, llvm::SMLoc loc);
  void noteTlsMethod(const MemberExpr *member, const ClassDecl *classDecl);
  std::string knownPrefix(const Expr *expr) const;

private:
  const Expr *uncast(const Expr *expr) const;
  bool isPlainTransport(const Expr *transport) const;
  bool isInsecureValue(const Expr *value) const;
  bool isSecuredProperty(const ObjectLitExpr::Property &property) const;
  const Expr *prefixInner(const Expr *expr) const;
  const Expr *addressArgument(const NewExpr *made) const;
  std::string asciiPrefix(const std::u16string &text) const;
  std::string ownPrefix(const Expr *expr) const;
  using ResultExternalParams = Result<const Type *, TzModulesCheckerExternalParamError>;
  using ResultExternalReturn = Result<const Type *, TzModulesCheckerExternalReturnError>;
  using ResultPlainSignature = Result<const FunctionDecl *, TzModulesCheckerPlainSignatureError>;

  ResultPlainSignature checkPlainSignature(const FunctionDecl *functionDecl) const;
  ResultExternalParams checkExternalParam(const Param &param, const Type *type) const;
  ResultExternalReturn checkExternalReturn(const Type *returnType, const Type *declared) const;

  bool isExternalReference(const Type *type) const;
  bool isExternalCallback(const Type *type) const;
  bool isPlainCallbackParam(const Type *param) const;
  bool isExternalValue(const Type *type) const;

  SemanticState &state;
  LibraryCalls *libraryCalls = nullptr;
};

}
