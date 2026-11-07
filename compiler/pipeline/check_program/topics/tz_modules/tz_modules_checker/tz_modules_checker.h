#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

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
  static bool isExternalReference(const Type *type);
  static bool isExternalCallback(const Type *type);
  static bool isExternalValue(const Type *type);

  CheckState &state;
};

}
