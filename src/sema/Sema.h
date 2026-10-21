#pragma once

#include "ast/Ast.h"
#include "sema/Types.h"
#include "support/Diagnostics.h"

#include "llvm/ADT/StringMap.h"

namespace tz {

// Resolves names and checks types. Annotates the AST in place: every
// expression gets a type, names get their symbols, and implicit conversions
// become explicit CastExpr nodes, so code generation needs no type rules.
//
// Numeric rules: no implicit conversion loses information. int widens to long
// and double on its own; everything else needs `as`. Integer literals take the
// type the context expects (`let x: double = 1 / 2` divides floats).
class Sema {
public:
  Sema(AstContext &ctx, Diagnostics &diags) : ctx_(ctx), diags_(diags) {}

  void check(SourceFile &file);

private:
  struct ScopeEntry {
    VarSymbol *var = nullptr;
    FuncSymbol *func = nullptr;
  };

  // Scopes
  void pushScope() { scopes_.emplace_back(); }
  void popScope() { scopes_.pop_back(); }
  void declare(llvm::StringRef name, llvm::SMLoc loc, ScopeEntry entry);
  ScopeEntry lookup(llvm::StringRef name) const;

  const Type *resolveType(const TypeRef &ref, bool allowVoid);

  // Declarations and statements
  void declareFunction(FunctionDecl *fn);
  void checkFunctionBody(FunctionDecl *fn);
  void checkStmt(Stmt *stmt);
  void checkVarDecl(VarDeclStmt *decl);
  void checkReturn(ReturnStmt *ret);
  void checkLoopBody(Stmt *body);
  Expr *checkCondition(Expr *cond);

  // Expressions. Each returns the expression to use in place of the argument,
  // which differs when a conversion was inserted.
  Expr *checkExpr(Expr *expr, const Type *expected = nullptr);
  Expr *checkIntLit(IntLitExpr *lit, const Type *expected, bool negated);
  Expr *checkName(NameExpr *name);
  Expr *checkUnary(UnaryExpr *unary, const Type *expected);
  Expr *checkBinary(BinaryExpr *binary, const Type *expected);
  Expr *checkAssign(AssignExpr *assign);
  Expr *checkUpdate(UpdateExpr *update);
  Expr *checkCall(CallExpr *call);
  Expr *checkConsoleLog(CallExpr *call);
  Expr *checkConditional(ConditionalExpr *cond, const Type *expected);
  Expr *checkCast(CastExpr *cast);

  // Resolves the target of an assignment or `++`/`--` to a mutable variable.
  VarSymbol *checkAssignTarget(Expr *target);
  // Wraps `expr` in an implicit conversion to `to` (no checks).
  Expr *convert(Expr *expr, const Type *to);
  // Converts `expr` to `to` if that is allowed implicitly, else reports an error.
  Expr *coerce(Expr *expr, const Type *to);
  void reportOperandTypes(llvm::SMLoc loc, const char *op, const Type *lhs, const Type *rhs);

  static bool isImplicitlyConvertible(const Type *from, const Type *to);
  static const Type *commonNumericType(const Type *a, const Type *b);
  static bool alwaysReturns(const Stmt *stmt);
  static bool containsBreak(const Stmt *stmt);

  AstContext &ctx_;
  Diagnostics &diags_;
  std::vector<llvm::StringMap<ScopeEntry>> scopes_;
  FuncSymbol *currentFunction_ = nullptr; // null in top-level code
  unsigned loopDepth_ = 0;
};

} // namespace tz
