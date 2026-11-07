#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class BuiltinModuleChecker {
public:
  explicit BuiltinModuleChecker(Checker &owner);

  static std::string moduleProblem(llvm::StringRef path);
  void importBuiltinModule(Module *module, const ImportDecl &decl);
  const Checker::NodeName *nodeName(llvm::StringRef name) const;
  Expr *checkNodeCall(CallExpr *call, const Type *expected = nullptr);
  Expr *checkNodeName(NameExpr *name);
  Expr *checkNodeMember(MemberExpr *member);
  Expr *processAlias(Expr *expr);
  Expr *nodeValue(const Checker::NodeName &n, Expr *expr);
  Expr *checkTimersPromises(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc,
                            const Type *expected);

private:
  static MemberExpr *processMember(AstContext &ctx, llvm::SMLoc loc, llvm::StringRef name);

  static std::optional<NodeModule> moduleOf(llvm::StringRef path);
  static std::string missingModule(llvm::StringRef path);
  static llvm::StringRef typicalName(NodeModule module);
  static bool isPathModule(NodeModule module);

  std::string exportProblem(NodeModule module, llvm::StringRef name, llvm::StringRef path) const;

  Checker &checker;
};

}
