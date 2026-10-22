#pragma once

#include "ast/ast.h"
#include "sema/facts.h"
#include "sema/types.h"
#include "support/diagnostics.h"

#include "llvm/ADT/DenseMap.h"
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
// and double on its own, long to double. A double becomes an int through a
// bitwise operator (`x | 0`, JavaScript's ToInt32) or through Math.round and
// the like where an integer is expected. There is no `as`. Integer literals
// take the type the context expects (`let x: double = 1 / 2` divides floats).
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
    EnumDecl *enumDecl = nullptr;
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
  // sema_flow.cpp).
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
    bool breaks = false; // a `break` may leave the loop
    // While scanning: the switches around the current statement inside the
    // loop, and the labels declared inside it.
    unsigned switches = 0;
    llvm::SmallVector<llvm::StringRef, 2> innerLabels;
  };

  // A statement that `break` (and for loops `continue`) can leave.
  struct JumpTarget {
    enum class Kind { Loop, Switch, Block };
    Kind kind;
    llvm::SmallVector<llvm::StringRef, 1> labels;
    // What holds where a `break` leaves a switch or a labeled block. Loops do
    // without: their head state holds at every `break` (see loopHead).
    Flow breaks;
  };

  // Scopes
  void pushScope() {
    scopes_.emplace_back();
    typeScopes_.emplace_back();
  }
  void popScope() {
    scopes_.pop_back();
    typeScopes_.pop_back();
  }
  void declare(llvm::StringRef name, llvm::SMLoc loc, ScopeEntry entry);
  ScopeEntry lookup(llvm::StringRef name) const;
  VarSymbol *declareVar(llvm::StringRef name, llvm::SMLoc loc, const Type *type, bool isConst);

  const Type *resolveType(const TypeRef &ref, bool allowVoid);
  const Type *resolveTypeInner(const TypeRef &ref);

  // Type aliases and enums (sema_decls.cpp). They are visible in the whole
  // block that declares them, so each block declares them before anything
  // else.
  void declareTypes(llvm::ArrayRef<Stmt *> stmts);
  Stmt *lookupType(llvm::StringRef name) const;
  const Type *resolveAlias(TypeAliasDecl *alias);
  void resolveEnum(EnumDecl *decl);
  // The value of an enum member's initializer.
  struct Constant {
    bool isString = false;
    double number = 0;
    std::u16string text;
  };
  std::optional<Constant> evalConstant(const Expr *expr, EnumDecl *self, size_t defined);
  Expr *checkEnumMember(EnumDecl *decl, MemberExpr *member, const Type *expected);
  Expr *checkEnumIndex(EnumDecl *decl, IndexExpr *index);

  // Integer constants (sema_decls.cpp): expressions of literals, `const`
  // variables holding constants, and operators. Computing them reports what
  // would go wrong at run time: a division by zero, or a value that does not
  // fit in its type and would wrap around where JavaScript keeps it.
  std::optional<int64_t> intConstant(const Expr *expr) const;
  void foldUnary(UnaryExpr *unary, bool wraps);
  void foldBinary(BinaryExpr *bin, bool wraps);
  // `x / 0` and `x % 0` of integers: an error even when x is not constant.
  void checkDivisor(BinaryOp op, const Expr *divisor);
  void reportOverflow(const Expr *expr, const Type *type, const std::string &value, int64_t wrapped,
                      bool wraps);

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
  void checkSwitch(SwitchStmt *stmt);
  void checkLabeled(LabeledStmt *stmt);
  void checkJump(Stmt *stmt);
  void checkLoopBody(Stmt *body);
  void checkBody(Stmt *body);
  // Enters a loop, switch or labeled block; a loop or switch takes the
  // labels in pendingLabels_.
  void pushTarget(JumpTarget::Kind kind);
  // Checks a condition and computes the flow where it is true and where it is
  // false.
  Expr *checkCondition(Expr *cond, Flow &whenTrue, Flow &whenFalse);
  Expr *checkBranching(Expr *expr, Flow &whenTrue, Flow &whenFalse);

  // Expressions. Each returns the expression to use in place of the argument,
  // which differs when a conversion was inserted. `wraps`: only the low bits
  // of the value matter, as in an operand of `|` (see foldBinary).
  Expr *checkExpr(Expr *expr, const Type *expected = nullptr, bool wraps = false);
  Expr *checkIntLit(IntLitExpr *lit, const Type *expected, bool negated);
  Expr *checkName(NameExpr *name);
  // Reports a use of a variable declared by an earlier case of a switch.
  void checkVisible(NameExpr *name, const VarSymbol *var);
  Expr *checkUnary(UnaryExpr *unary, const Type *expected, bool wraps);
  Expr *checkBinary(BinaryExpr *binary, const Type *expected, bool wraps);
  Expr *checkEquality(BinaryExpr *bin);
  Expr *checkNullish(BinaryExpr *bin, const Type *expected);
  Expr *checkAssign(AssignExpr *assign);
  Expr *checkLogicalAssign(AssignExpr *assign);
  Expr *checkUpdate(UpdateExpr *update);
  Expr *checkSequence(SequenceExpr *seq, const Type *expected, bool wraps);
  Expr *checkOptionalChain(OptionalChainExpr *chain);
  // The object of a link `object?.x` of an optional chain: if it may be
  // undefined, a conversion to its value that ends the chain when it is
  // undefined (see OptionalChainExpr).
  Expr *chainLink(Expr *object);
  Expr *checkCall(CallExpr *call, const Type *expected);
  Expr *checkUserCall(CallExpr *call, NameExpr *name);
  Expr *checkConsoleLog(CallExpr *call);
  // String(x), Number(x), parseInt... (sema_builtins.cpp); null if `name` is
  // not one of them.
  Expr *checkGlobalCall(CallExpr *call, NameExpr *name, const Type *expected);
  Expr *checkGlobalFunction(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc,
                            const Type *expected);
  // String.fromCharCode, Number.isNaN...
  Expr *checkStaticCall(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkMathCall(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkMinMaxSpread(CallExpr *call, const Type *hint);
  Expr *checkNumberMethod(CallExpr *call, MemberExpr *member);
  Expr *checkStringMethod(CallExpr *call, MemberExpr *member);
  Expr *checkArrayMethod(CallExpr *call, MemberExpr *member);
  Expr *checkNew(NewExpr *expr, const Type *expected);
  Expr *checkArrayLit(ArrayLitExpr *lit, const Type *expected);
  Expr *checkConditional(ConditionalExpr *cond, const Type *expected);
  Expr *checkCast(CastExpr *cast);
  Expr *checkTemplate(TemplateExpr *tmpl);
  Expr *checkMember(MemberExpr *member, const Type *expected);
  // Math.PI, Number.MAX_SAFE_INTEGER, NaN...: null if `name` is not one.
  Expr *globalConstant(llvm::StringRef object, llvm::StringRef name, llvm::SMLoc loc);
  Expr *checkIndex(IndexExpr *index);
  // A position or count argument: any integer, computed as long.
  Expr *checkPosition(Expr *expr);
  // An argument that must be a number; reports anything else.
  Expr *numericArg(Expr *arg, const Type *hint = nullptr);
  // An argument converted the way a parameter of type `to` takes it.
  Expr *checkArg(Expr *arg, const Type *to);
  // Reports a wrong number of arguments; checks them loosely then.
  bool checkArity(CallExpr *call, llvm::SMLoc loc, unsigned min, unsigned max);
  // Reports spread arguments where the callee takes none (only rest
  // parameters, Math.min, Math.max and push do); checks the arguments loosely
  // then.
  bool rejectSpread(CallExpr *call);

  // Converts a value to a string for `+` and template literals.
  Expr *toStringOperand(Expr *expr);
  // An operand of a bitwise operator as an int, the way JavaScript's ToInt32
  // makes one: a double loses its fraction and wraps around, NaN and the
  // infinities become 0; a long keeps its low 32 bits.
  Expr *toInt32(Expr *expr);
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
  // The type in which `===` compares values of types `a` and `b` (neither
  // undefined), or null if they cannot be compared.
  static const Type *comparisonType(const Type *a, const Type *b);
  // Whether control can never reach the end of `stmt`; `labels` are its own.
  static bool alwaysReturns(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels = {});

  // Flow: facts, narrowing and proofs (sema_flow.cpp)
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
  std::vector<llvm::StringMap<Stmt *>> typeScopes_; // TypeAliasDecl or EnumDecl
  FuncSymbol *currentFunction_ = nullptr;           // null in top-level code
  std::vector<JumpTarget> targets_;
  llvm::SmallVector<llvm::StringRef, 1> pendingLabels_;
  // Variables declared by the earlier cases of the switches being checked:
  // the case being checked must not use them (see checkSwitch).
  llvm::SmallPtrSet<const VarSymbol *, 4> otherCaseVars_;
  Flow flow_;
  llvm::StringSet<> namesInFunctions_;
  // Some link of the optional chain being checked may end it.
  bool chainShortCircuits_ = false;
  // The values of the integer constants computed so far: operator expressions
  // and `const` variables. In a `wraps` context a value may exceed its type
  // (see foldBinary).
  llvm::DenseMap<const Expr *, int64_t> constants_;
  llvm::DenseMap<const VarSymbol *, int64_t> constVars_;
};

// "2 arguments", "1-3 arguments", "1 or more arguments".
std::string argumentCount(unsigned min, unsigned max);

} // namespace tz
