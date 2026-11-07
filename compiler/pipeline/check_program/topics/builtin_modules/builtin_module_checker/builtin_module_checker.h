#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/builtin_modules/builtin_module_checker/builtin_module_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class BuiltinModuleChecker {
public:
  using ResultKnownModule = Result<NodeModule, BuiltinModuleCheckerKnownModuleError>;
  using ResultExportFound = Result<NodeModule, BuiltinModuleCheckerExportFoundError>;

  explicit BuiltinModuleChecker(CheckState &owner);

  static ResultKnownModule knownModule(llvm::StringRef path);
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

  static std::optional<NodeModule> moduleOf(llvm::StringRef path);
  static llvm::StringRef typicalName(NodeModule module);
  static bool isPathModule(NodeModule module);

  ResultExportFound exportFound(NodeModule module, llvm::StringRef name,
                                llvm::StringRef path) const;

  CheckState &state;
};

}
