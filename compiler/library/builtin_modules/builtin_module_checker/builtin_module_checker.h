#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class BuiltinModuleChecker {
public:
  explicit BuiltinModuleChecker(Checker &checker);

  void importBuiltinModule(Module *module, const ImportDecl &decl);
  const Checker::NodeName *nodeName(llvm::StringRef name) const;
  Expr *checkNodeCall(CallExpr *call, const Type *expected = nullptr);
  Expr *checkNodeName(NameExpr *name);
  Expr *checkNodeMember(MemberExpr *member);
  Expr *processAlias(Expr *expr);
  Expr *nodeValue(const Checker::NodeName &n, Expr *expr);

private:
  std::string exportProblem(NodeModule module, llvm::StringRef name, llvm::StringRef path) const;

  Checker &checker_;
};

}
