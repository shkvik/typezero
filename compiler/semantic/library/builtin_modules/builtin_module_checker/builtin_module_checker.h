#pragma once

#include "compiler/program/library/builtin_modules/known_modules/known_modules.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_checker/builtin_module_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class BuiltinModuleChecker {
public:
  using ResultExportFound = Result<NodeModule, BuiltinModuleCheckerExportFoundError>;

  explicit BuiltinModuleChecker(SemanticState &owner);

  void importBuiltinModule(Module *module, const ImportDecl &decl);
  const NodeName *nodeName(llvm::StringRef name) const;
  Expr *checkNodeCall(CallExpr *call, const Type *expected = nullptr);
  Expr *checkNodeName(NameExpr *name);
  Expr *checkNodeMember(MemberExpr *member);
  Expr *processAlias(Expr *expr);
  Expr *nodeValue(const NodeName &n, Expr *expr);
  Expr *checkTimersPromises(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc,
                            const Type *expected);

private:
  static MemberExpr *processMember(AstContext &ctx, llvm::SMLoc loc, llvm::StringRef name);

  static llvm::StringRef typicalName(NodeModule module);
  static bool isPathModule(NodeModule module);

  ResultExportFound exportFound(NodeModule module, llvm::StringRef name,
                                llvm::StringRef path) const;

  SemanticState &state;
};

}
