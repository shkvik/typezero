#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/stmt/var_checker/var_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class VarChecker {
public:
  explicit VarChecker(CheckState &owner);

  static std::vector<const Stmt *> followingStatements(llvm::ArrayRef<Stmt *> stmts, size_t i);

  void checkVarDecl(VarDeclStmt *decl);
  void noteUntyped(VarSymbol *var, bool initUndefined);
  void assignedUntyped(const VarSymbol *var, const Type *type);
  void reportUntyped();
  void reportUncheckedResults();

private:
  using ResultInitHasType = Result<const Type *, VarCheckerInitHasTypeError>;
  using ResultConstValued = Result<const VarDeclarator *, VarCheckerConstValueError>;
  using ResultDestructure = Result<const Type *, VarCheckerDestructurableError>;
  using ResultResultCheck = Result<const VarSymbol *, VarCheckerResultCheckedError>;
  using ResultVarHasTypes = Result<const Type *, VarCheckerVarTypedError>;

  static ResultInitHasType initHasType(const VarDeclStmt *decl, const VarDeclarator &d,
                                       const Type *type);
  static ResultConstValued constValue(const VarDeclStmt *decl, const VarDeclarator &d);
  static ResultDestructure destructurable(const VarDeclarator &d, const Type *type);
  static ResultResultCheck resultChecked(const VarSymbol *var);
  static ResultVarHasTypes varTyped(const Untyped &u, const Type *type);
  static bool isTextChoice(const Expr *expr);

  bool exported(llvm::StringRef name) const;
  CheckState &state;
};

}
