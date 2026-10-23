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

  // A field reached from a variable through fields: `q.limit`, `a.b.c`.
  struct FieldPath {
    const VarSymbol *var = nullptr;
    llvm::SmallVector<llvm::StringRef, 2> names;
    bool operator==(const FieldPath &other) const { return var == other.var && names == other.names; }
  };
  // What is known at the current point of the program: facts about ints and
  // lengths, and the `T | undefined` variables and fields that hold a value.
  struct Flow {
    Facts facts;
    llvm::SmallPtrSet<const VarSymbol *, 4> defined;
    std::vector<FieldPath> definedFields;
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
    // The fields assigned to, by name, of objects of any type.
    llvm::StringSet<> fieldWrites;
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
    if (!untyped_.empty())
      reportUntyped();
    scopes_.pop_back();
    typeScopes_.pop_back();
  }
  void declare(llvm::StringRef name, llvm::SMLoc loc, ScopeEntry entry);
  ScopeEntry lookup(llvm::StringRef name) const;
  VarSymbol *declareVar(llvm::StringRef name, llvm::SMLoc loc, const Type *type, bool isConst);

  const Type *resolveType(const TypeRef &ref, bool allowVoid);
  const Type *resolveTypeInner(const TypeRef &ref);
  // The fields of an object type written in the source.
  std::vector<Field> resolveFields(const TypeRef &ref);

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

  // Return types inferred from function bodies (sema_infer.cpp). A function
  // without a return type annotation returns what its `return` statements
  // give; Sema finds that the first time it needs it.
  const Type *returnType(FuncSymbol *fn);
  // The type of the function as a value.
  const Type *functionType(FuncSymbol *fn);
  void inferReturnType(FuncSymbol *fn);
  void resolveParams(FuncSymbol *fn);
  // Checks the body of a function without a return type and sets the type
  // from its returns.
  void checkAndInfer(FunctionDecl *decl);
  // Functions that call each other, directly or not, depend on each other's
  // types: their bodies are checked on copies until the types settle.
  void inferCycle(llvm::ArrayRef<FuncSymbol *> cycle);
  // Finds those cycles among the functions without a return type.
  void findCycles(llvm::ArrayRef<FunctionDecl *> functions);
  // The type of the values `returns` give, with undefined if the function
  // may also end without a value; null if none has a type (yet).
  const Type *unifyReturns(const FuncSymbol *fn, llvm::ArrayRef<ReturnStmt *> returns, bool fallsOff,
                           bool report);
  FunctionDecl *freshCopy(const FunctionDecl *fn);
  static bool returnsValue(const Stmt *stmt);
  // The variables the blocks being checked declare, for messages about
  // using one before its declaration.
  struct BlockNames {
    size_t functions; // functions_.size() in the block
    std::vector<llvm::StringRef> names;
  };
  // What checking a function body may change, so that one body can be
  // checked in the middle of another.
  struct BodyState {
    Flow flow;
    std::vector<llvm::StringMap<ScopeEntry>> scopes;
    std::vector<llvm::StringMap<Stmt *>> typeScopes;
    std::vector<FunctionDecl *> functions;
    std::vector<NestedGroup *> groups;
    std::vector<BlockNames> blockNames;
    FuncSymbol *function = nullptr;
    std::vector<JumpTarget> targets;
    llvm::SmallVector<llvm::StringRef, 1> pendingLabels;
    llvm::SmallPtrSet<const VarSymbol *, 4> otherCaseVars;
    bool chainShortCircuits = false;
    std::vector<ReturnStmt *> *returns = nullptr;
    bool trial = false;
    const FuncSymbol *early = nullptr;
  };
  // Saves the state and starts a fresh one for checking the body of `fn`:
  // only globals are visible to a top-level function, and to a closure what
  // is visible where it is declared.
  BodyState enterBody(const FunctionDecl *fn);
  void leaveBody(BodyState &saved);

  // Closures (sema_closure.cpp): arrows, function expressions and functions
  // declared inside blocks, the variables they capture, calls through
  // function values, and the array methods that take callbacks.
  //
  // What a function is expected to be where it is written: the arguments it
  // will get, and the result wanted (null: whatever the body returns). With
  // `pad`, it takes all of params even if it names fewer, so that its type is
  // the one expected; a callback of a built-in method takes just the
  // parameters it names.
  struct Signature {
    std::vector<const Type *> params;
    size_t minArgs = 0;
    const Type *ret = nullptr;
    bool voidResult = false; // what it returns is dropped (forEach)
    bool pad = true;
  };
  static Signature signatureOf(const Type *fnType);
  // `self`: the constant the function is assigned to (`const f = () => ...`),
  // through which it calls itself.
  Expr *checkFunctionExpr(FunctionExpr *fe, const Signature *expected);
  void checkClosureBody(FunctionDecl *fn);
  // `function f() {}` as an expression: f is visible in its body, in a scope
  // of its own.
  void declareOwnName(FunctionDecl *fn);
  // The statements of a block, with the functions it declares.
  void checkStatements(BlockStmt *block);
  NestedGroup *declareNested(BlockStmt *block);
  void finishGroup(NestedGroup *group);
  FunctionDecl *currentDecl() const { return functions_.empty() ? nullptr : functions_.back(); }
  // A use of a variable: the functions between its owner and here capture it.
  void noteUse(VarSymbol *var, llvm::SMLoc loc);
  Expr *checkValueCall(CallExpr *call, const Type *fnType);
  // An argument where a function of the signature is expected. `primary` is
  // the number of arguments a built-in callback is meant to use; a function
  // passed by name must not take the others (the index, the array) into
  // optional parameters, as parseInt would take the index as its radix.
  Expr *checkFunctionArg(Expr *arg, const Signature &expected, unsigned primary, llvm::StringRef method);
  // A built-in such as Math.sqrt or parseInt where a function is expected:
  // an arrow calling it, or null if `arg` names none.
  FunctionExpr *builtinFunctionValue(Expr *arg, const Signature &expected, unsigned primary,
                                      llvm::StringRef method);
  Expr *checkCallbackMethod(CallExpr *call, MemberExpr *member);
  // A function known by name (a declaration or a constant arrow) where a
  // function type that passes more or fewer arguments, or drops the result,
  // is expected: a conversion that CodeGen turns into a small adapter. Null
  // if that type does not fit.
  Expr *adaptFunction(Expr *expr, const Type *to);
  // The function a name refers to directly, if any.
  FunctionDecl *namedFunction(const Expr *expr) const;
  // A function, or an array or optional of them.
  static bool holdsFunctions(const Type *type) {
    while (type->isOptional() || type->isArray())
      type = type->inner();
    return type->isFunction();
  }
  // The same for objects.
  static bool holdsObjects(const Type *type) {
    while (type->isOptional() || type->isArray())
      type = type->inner();
    return type->isObject();
  }

  // Declarations and statements
  // `var` is the variable of a function declared in a block.
  void declareFunction(FunctionDecl *fn, VarSymbol *var = nullptr);
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
  // `let x` without a type or a value, and `let x = undefined`: reported when
  // the scope of x ends, with a type made from the values assigned to x up
  // to there as the hint.
  void noteUntyped(VarSymbol *var, bool initUndefined);
  void assignedUntyped(const VarSymbol *var, const Type *type);
  void reportUntyped();

  // Truthiness (sema_truth.cpp). As in JavaScript, a condition may test a
  // string or a value that may be undefined, but not a number; `&&` and `||`
  // give one of their operands. `use` says how a tested value is used, for
  // the hints of the errors.
  enum class TruthUse { Condition, Not, Or, And, OrAssign, AndAssign };
  // Checks a condition and computes the flow where it is true and where it is
  // false. The result is a boolean.
  Expr *checkCondition(Expr *cond, Flow &whenTrue, Flow &whenFalse);
  Expr *checkBranching(Expr *expr, Flow &whenTrue, Flow &whenFalse, TruthUse use = TruthUse::Condition);
  // An operand of `&&` or `||` whose value is the result when it decides.
  Expr *checkTestedValue(Expr *expr, const Type *expected, Flow &whenTrue, Flow &whenFalse, TruthUse use);
  // `a && b` and `a || b` as values. With `tested`, an enclosing `&&` or `||`
  // tests the result too.
  Expr *checkLogical(BinaryExpr *bin, const Type *expected, Flow &whenTrue, Flow &whenFalse, bool tested,
                     TruthUse use);
  // Reports a value whose truth cannot be tested; `other` is the right side
  // of `||` or the value of `||=`, for the hint.
  bool checkTruthy(const Expr *expr, TruthUse use, const Expr *other = nullptr);
  // `if (level = 0)`
  void reportAssignmentCondition(const AssignExpr *assign, TruthUse use);
  // `Boolean(x)`, which converts like a condition.
  Expr *checkBooleanCall(CallExpr *call, Flow &whenTrue, Flow &whenFalse);
  bool isBooleanCall(const Expr *expr) const;

  // Expressions. Each returns the expression to use in place of the argument,
  // which differs when a conversion was inserted. `wraps`: only the low bits
  // of the value matter, as in an operand of `|` (see foldBinary).
  Expr *checkExpr(Expr *expr, const Type *expected = nullptr, bool wraps = false);
  Expr *checkIntLit(IntLitExpr *lit, const Type *expected, bool negated);
  Expr *checkName(NameExpr *name);
  const Type *varType(VarSymbol *var);
  // Reports a use of a variable declared by an earlier case of a switch.
  void checkVisible(NameExpr *name, const VarSymbol *var);
  Expr *checkUnary(UnaryExpr *unary, const Type *expected, bool wraps);
  // `delete x[i]` and `key in x`: errors until objects come (see sema.cpp).
  void checkDelete(UnaryExpr *unary);
  Expr *checkIn(BinaryExpr *bin);
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
  // A call to a function known by name; `loc` is where to report the
  // arguments.
  Expr *checkDirectCall(CallExpr *call, FuncSymbol *func, llvm::SMLoc loc);
  Expr *checkArguments(CallExpr *call, llvm::ArrayRef<const Type *> params, size_t minArgs, bool hasRest,
                       llvm::SMLoc loc);
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
  // Objects (sema_object.cpp). A literal takes the type expected where it is
  // written, and its fields the types of that type's fields; without one it
  // has the types of its values.
  Expr *checkObjectLit(ObjectLitExpr *lit, const Type *expected);
  // `object.name` of an object, whose object is checked.
  Expr *checkField(MemberExpr *member);
  // `object.name(args)` of an object: a call of the function in the field.
  Expr *checkFieldCall(CallExpr *call, MemberExpr *member);
  const Type *checkFieldTarget(MemberExpr *member);
  // `o.f ??= v`: `o.f ?? (o.f = v)`, when the object can be read twice.
  // (`||=` and `&&=` on fields go through checkLogicalAssign.)
  Expr *checkFieldLogicalAssign(AssignExpr *assign);
  // Why `from` does not fit an object type `to`: the fields that differ.
  std::string objectMismatch(const Type *from, const Type *to) const;
  // Fields that hold a value (see Flow::definedFields). A narrowed field is
  // forgotten when any field of that name is assigned to, the variable it
  // starts from changes, or a function is called and some code of the
  // program assigns to a field of that name: any of those may change it.
  std::optional<FieldPath> fieldPath(const Expr *expr) const;
  bool isFieldDefined(const FieldPath &path) const;
  static void defineField(Flow &flow, const FieldPath &path);
  void fieldsAssigned(llvm::StringRef name);
  static void forgetFieldsOf(Flow &flow, const VarSymbol *var);
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
  // Reports a number or a boolean joined with a string by `+` in the chain
  // `a + b + c` that ends at `root`; `mixed` is the first `+` that does it.
  void reportMixedConcat(const BinaryExpr *root, const BinaryExpr *mixed);
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
  // Converts `expr` to `to` if that is allowed implicitly, else reports an
  // error; `what` names the place, as "field 'x'".
  Expr *coerce(Expr *expr, const Type *to, llvm::StringRef what = {});
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
  // A call to a user function may change globals, the length of any array,
  // and variables that closures assign to.
  void calledFunction();
  void forgetCallEffects(Flow &flow) const;
  bool changedByCalls(const VarSymbol *var) const;
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
  // The names of the fields the program assigns to anywhere: only those can
  // change during a call.
  llvm::StringSet<> assignedFields_;
  // The variables whose initializers are being checked, for the message
  // about one that uses itself: `const o = { f: () => o.n }`.
  llvm::SmallVector<llvm::StringRef, 2> initializing_;
  // Some link of the optional chain being checked may end it.
  bool chainShortCircuits_ = false;
  // The last `+` of the chain `a + b + c` whose left side is about to be
  // checked, and the first `+` of the chain being checked that joins a
  // number or a boolean with a string: the chain reports it once, with a
  // template made of all its parts.
  BinaryExpr *concatChain_ = nullptr;
  const BinaryExpr *mixedConcat_ = nullptr;
  // Inferring the return type of currentFunction_: its return statements.
  std::vector<ReturnStmt *> *returns_ = nullptr;
  // Checking a copy of a function of a cycle (see inferCycle): a call within
  // the cycle may not have a type yet.
  bool trial_ = false;
  // Top-level code is still being checked; then a function inferred early,
  // because top-level code calls it, cannot see the globals declared later.
  bool topLevelDone_ = false;
  const FuncSymbol *early_ = nullptr;
  llvm::StringSet<> topLevelNames_;
  std::vector<std::vector<FuncSymbol *>> cycles_;
  llvm::DenseMap<const FuncSymbol *, unsigned> cycleOf_;
  // The variables declared without a type that the scopes being checked hold
  // (see noteUntyped), with the types of the values assigned to them.
  struct Untyped {
    VarSymbol *var;
    bool initUndefined; // `let x = undefined`
    std::vector<const Type *> assigned;
  };
  std::vector<Untyped> untyped_;
  // The values of the integer constants computed so far: operator expressions
  // and `const` variables. In a `wraps` context a value may exceed its type
  // (see foldBinary).
  llvm::DenseMap<const Expr *, int64_t> constants_;
  llvm::DenseMap<const VarSymbol *, int64_t> constVars_;
  // The functions being checked, innermost last; empty in main's code.
  std::vector<FunctionDecl *> functions_;
  // The groups of the blocks being checked: the names their functions
  // assign, which a call may change.
  std::vector<NestedGroup *> groups_;
  unsigned seq_ = 0; // see VarSymbol::declSeq
  SourceFile *file_ = nullptr;
  std::vector<BlockNames> blockNames_;
  const BlockNames *declaredLater(llvm::StringRef name) const {
    for (const BlockNames &block : llvm::reverse(blockNames_))
      if (llvm::is_contained(block.names, name))
        return &block;
    return nullptr;
  }
};

// "2 arguments", "1-3 arguments", "1 or more arguments".
std::string argumentCount(unsigned min, unsigned max);

// Why a compiled program cannot have the global `name` of JavaScript (eval,
// Function, Proxy), or null for any other name.
const char *whyUnsupported(llvm::StringRef name);

// A function in messages: "'f'" and "function 'f'", or "the function" for an
// arrow without a name.
inline std::string quotedName(llvm::StringRef name) {
  return name.empty() ? "the function" : "'" + name.str() + "'";
}
inline std::string functionLabel(llvm::StringRef name) {
  return name.empty() ? "the function" : "function '" + name.str() + "'";
}

// The names a statement or an expression mentions. With `inFunction` unset,
// only those inside the functions it contains.
void collectNames(const Stmt *stmt, llvm::StringSet<> &names, bool inFunction = true);
void collectNames(const Expr *expr, llvm::StringSet<> &names, bool inFunction = true);

// Calls `visit` on every expression inside `stmt`, those in the functions it
// contains included.
void visitExprs(const Stmt *stmt, llvm::function_ref<void(const Expr *)> visit);
void visitExprs(const Expr *expr, llvm::function_ref<void(const Expr *)> visit);

} // namespace tz
