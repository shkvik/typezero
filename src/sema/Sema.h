#pragma once

#include "ast/Ast.h"
#include "sema/Facts.h"
#include "sema/Types.h"
#include "support/Diagnostics.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"

#include <optional>

namespace tz {

// Resolves names and checks types. Annotates the AST in place: every
// expression gets a type, names get their symbols, and implicit conversions
// become explicit CastExpr nodes, so code generation needs no type rules.
//
// Numeric rules: no implicit conversion loses information. int widens to long
// and double on its own; everything else needs `as`. Integer literals take the
// type the context expects (`let x: double = 1 / 2` divides floats).
//
// Sema also follows the flow of the program (see Flow): it narrows
// `T | undefined` after checks such as `x !== undefined`, and it proves
// indices in range. A read `a[i]` it can prove has type `T` and needs no check
// at run time; any other read has type `T | undefined`, as in JavaScript.
class Sema {
public:
  Sema(AstContext &ctx, Diagnostics &diags) : ctx_(ctx), diags_(diags) {}

  void check(SourceFile &file);

private:
  struct ScopeEntry {
    VarSymbol *var = nullptr;
    FuncSymbol *func = nullptr;
  };

  // What is known at the current point of the program: facts about ints and
  // lengths, and the `T | undefined` variables that hold a value.
  struct Flow {
    Facts facts;
    llvm::SmallPtrSet<const VarSymbol *, 4> defined;
    bool unreachable = false; // after return, break or continue
  };
  // What holds after control merges from `a` and `b`.
  static Flow join(const Flow &a, const Flow &b);

  // What a loop may change, found by scanning it before checking it (see
  // SemaFlow.cpp).
  struct LoopScan {
    struct Change {
      unsigned count = 0; // assignments and updates of the variable
      int64_t delta = 0;  // the constant step of `x++`, `x += 2`, `x--`..., or 0
      bool nested = false; // some are in a nested loop
    };
    llvm::StringMap<Change> changes; // by variable name
    bool callsFunctions = false;
    bool growsArrays = false;
    bool shrinksArrays = false;
    // Some element of some array may be removed or replaced (pop, shift,
    // `a[i] = x`, fill...), and so released.
    bool releasesElements = false;
    bool breaks = false;
  };

  // Scopes
  void pushScope() { scopes_.emplace_back(); }
  void popScope() { scopes_.pop_back(); }
  void declare(llvm::StringRef name, llvm::SMLoc loc, ScopeEntry entry);
  ScopeEntry lookup(llvm::StringRef name) const;
  VarSymbol *declareVar(llvm::StringRef name, llvm::SMLoc loc, const Type *type, bool isConst);

  const Type *resolveType(const TypeRef &ref, bool allowVoid);
  const Type *resolveTypeInner(const TypeRef &ref);

  // Declarations and statements
  void declareFunction(FunctionDecl *fn);
  void checkFunctionBody(FunctionDecl *fn);
  void checkStmt(Stmt *stmt);
  void checkVarDecl(VarDeclStmt *decl);
  void checkReturn(ReturnStmt *ret);
  void checkIf(IfStmt *stmt);
  void checkWhile(WhileStmt *stmt);
  void checkDoWhile(DoWhileStmt *stmt);
  void checkFor(ForStmt *stmt);
  void checkForOf(ForOfStmt *stmt);
  void checkLoopBody(Stmt *body);
  void checkBody(Stmt *body);
  // Checks a condition and computes the flow where it is true and where it is
  // false.
  Expr *checkCondition(Expr *cond, Flow &whenTrue, Flow &whenFalse);
  Expr *checkBranching(Expr *expr, Flow &whenTrue, Flow &whenFalse);

  // Expressions. Each returns the expression to use in place of the argument,
  // which differs when a conversion was inserted.
  Expr *checkExpr(Expr *expr, const Type *expected = nullptr);
  Expr *checkIntLit(IntLitExpr *lit, const Type *expected, bool negated);
  Expr *checkName(NameExpr *name);
  Expr *checkUnary(UnaryExpr *unary, const Type *expected);
  Expr *checkBinary(BinaryExpr *binary, const Type *expected);
  Expr *checkEquality(BinaryExpr *bin);
  Expr *checkNullish(BinaryExpr *bin, const Type *expected);
  Expr *checkAssign(AssignExpr *assign);
  Expr *checkUpdate(UpdateExpr *update);
  Expr *checkCall(CallExpr *call);
  Expr *checkConsoleLog(CallExpr *call);
  Expr *checkStringMethod(CallExpr *call, MemberExpr *member);
  Expr *checkArrayMethod(CallExpr *call, MemberExpr *member);
  Expr *checkNew(NewExpr *expr, const Type *expected);
  Expr *checkArrayLit(ArrayLitExpr *lit, const Type *expected);
  Expr *checkConditional(ConditionalExpr *cond, const Type *expected);
  Expr *checkCast(CastExpr *cast);
  Expr *checkTemplate(TemplateExpr *tmpl);
  Expr *checkMember(MemberExpr *member);
  Expr *checkIndex(IndexExpr *index);
  // A position or count argument: any integer, computed as long.
  Expr *checkPosition(Expr *expr);

  // Converts a value to a string for `+` and template literals.
  Expr *toStringOperand(Expr *expr);
  // Whether `expr` names a built-in global such as `console` that the program
  // does not shadow.
  bool isGlobalName(const Expr *expr, llvm::StringRef name) const;

  // Assignment targets: a mutable variable, an array element or the length of
  // an array. Returns the type the target holds.
  const Type *checkAssignTarget(Expr *target, bool compound);
  VarSymbol *checkVarTarget(NameExpr *name);
  // Wraps `expr` in an implicit conversion to `to` (no checks).
  Expr *convert(Expr *expr, const Type *to);
  // Converts `expr` to `to` if that is allowed implicitly, else reports an error.
  Expr *coerce(Expr *expr, const Type *to);
  void reportOperandTypes(llvm::SMLoc loc, const char *op, const Type *lhs, const Type *rhs);

  static bool isImplicitlyConvertible(const Type *from, const Type *to);
  static const Type *commonNumericType(const Type *a, const Type *b);
  // The type both `a` and `b` convert to (for `?:`, `??` and array
  // literals), or null.
  static const Type *unify(const Type *a, const Type *b);
  static bool alwaysReturns(const Stmt *stmt);

  // Flow: facts, narrowing and proofs (SemaFlow.cpp)
  // `expr` (checked, of an integer type) as `term + offset`, if it has that
  // form and its computation cannot overflow.
  std::optional<Linear> linear(const Expr *expr) const;
  // The length of a string or array expression as a linear term.
  std::optional<Linear> lengthOf(const Expr *object) const;
  // The length a string or array value is known to have when created.
  std::optional<Linear> knownLength(const Expr *value) const;
  // Whether 0 <= index < length(object), or <= length with `allowEnd`.
  bool provesInBounds(const Expr *object, const Expr *index, bool allowEnd) const;
  // The same for `at(index)`, where a negative index counts from the end.
  bool provesAtInBounds(const Expr *object, const Expr *index) const;
  bool provesNonNegative(const Expr *expr) const;
  bool provesNotEmpty(const Expr *object) const;
  void addConditionFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const;
  // Records `var = value` (or a declaration with that initializer); `value`
  // is the checked value before its conversion to the variable's type.
  void assigned(const VarSymbol *var, const Expr *value);
  // Records `var += delta`, where delta is a constant.
  void incremented(const VarSymbol *var, int64_t delta);
  // A call to a user function may change globals and the length of any array.
  void calledFunction();
  static void forgetCallEffects(Flow &flow);
  // Arrays of type `type` (any of which may be the one changed) may have
  // grown or shrunk; `except` is an array whose facts were already updated.
  void arrayResized(const Type *type, bool grew, bool shrank, const VarSymbol *except = nullptr);
  bool isDefined(const VarSymbol *var) const { return flow_.defined.contains(var); }
  void setDefined(const VarSymbol *var, bool defined);
  LoopScan scanLoop(llvm::ArrayRef<const Stmt *> stmts, llvm::ArrayRef<const Expr *> exprs) const;
  void scanStmt(const Stmt *stmt, LoopScan &scan, unsigned depth) const;
  void scanExpr(const Expr *expr, LoopScan &scan, unsigned depth) const;
  // The state at the head of a loop: what holds on entry and stays true
  // across iterations, given what the loop changes.
  Flow loopHead(const LoopScan &scan, const Expr *cond, bool induction) const;
  std::optional<int64_t> condBoundsAbove(const Expr *cond, const VarSymbol *var,
                                         const LoopScan &scan, int64_t step) const;
  bool condBoundsBelow(const Expr *cond, const VarSymbol *var, const LoopScan &scan,
                       int64_t step) const;
  bool boundedAbove(const Expr *expr, int64_t limit, const LoopScan &scan) const;
  bool boundedBelow(const Expr *expr, int64_t limit, const LoopScan &scan) const;
  // The range of an int expression judged by its form alone, before Sema
  // checks it: literals, int variables and lengths.
  std::optional<std::pair<int64_t, int64_t>> syntacticIntRange(const Expr *expr) const;

  AstContext &ctx_;
  Diagnostics &diags_;
  std::vector<llvm::StringMap<ScopeEntry>> scopes_;
  FuncSymbol *currentFunction_ = nullptr; // null in top-level code
  unsigned loopDepth_ = 0;
  Flow flow_;
  llvm::StringSet<> namesInFunctions_;
};

} // namespace tz
