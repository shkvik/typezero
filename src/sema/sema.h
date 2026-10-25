#pragma once

#include "ast/ast.h"
#include "sema/crypto_api.h"
#include "sema/facts.h"
#include "sema/types.h"
#include "support/decimal_literal.h"
#include "support/diagnostics.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"

#include <deque>
#include <map>
#include <optional>

namespace tz {

// The modules built into tz that a program imports names from
// (sema_node.cpp): node:fs, node:path (and its posix and win32 styles),
// node:os, node:process, node:buffer, node:child_process, node:crypto and tz:io.
enum class NodeModule {
  Fs,
  FsPromises,
  Path,
  PathPosix,
  PathWin32,
  Os,
  Process,
  Buffer,
  TzIo,
  TimersPromises,
  Readline,
  ReadlinePromises,
  ChildProcess,
  Crypto,
};

// What is wrong with importing `name` from node:crypto, written `path`, or nothing
// (sema_crypto.cpp).
std::string cryptoExportProblem(llvm::StringRef name, llvm::StringRef path);

// Why the built-in module `path` (see isBuiltinModule) cannot be imported:
// it is not there yet, or there is none; empty for the modules tz has. The
// loader reports it.
std::string builtinModuleProblem(llvm::StringRef path);

// The values an integer expression may have, as far as Sema knows: the
// bounds it found (see Sema::valueRange).
struct ValueRange {
  std::optional<int64_t> lo, hi;
};

// The event maps of the emitters whose types do not give them, `new
// EventEmitter()` and `class Server extends EventEmitter`, found from the
// emit() calls of the whole program (docs/events.md, section 2). The driver
// checks such a program in rounds, each with a syntax tree of its own: a
// round records what the emits it checks pass, and gives the listeners of
// the events known from the rounds before their types; once a round finds
// nothing new, a last one checks the program with the maps as they are,
// for real (see Sema::inferredEventMap).
struct EventInference {
  // A type as the next round makes it again: a class (and so a type that
  // has one in it) by its file and name, as each round has declarations
  // of its own; any other type as it is, the same in every round.
  struct Portable {
    enum class Kind {
      Same,
      Class,    // name in file, of the type arguments `parts`
      ClassRef, // the class itself as a value
      Optional, // parts[0] | undefined
      Array,    // parts[0][]
      Tuple,    // the elements, `min` of them needed, a rest last if `rest`
      Function, // the parameters then the result
      Union,
      Object,   // the fields, named by `names`
      Promise,
      Dict,     // `dict` (Type::DictKind), the key and the value
      Iterator, // yielded, returned, sent
      Result,   // ok, err
      Bad,      // what the next round cannot make: a literal with methods
    };
    Kind kind = Kind::Same;
    const Type *same = nullptr;
    std::string file, name;
    std::vector<Portable> parts;
    std::vector<std::string> names;
    std::vector<bool> readonly;
    size_t min = 0;
    bool rest = false;
    int dict = 0;
  };
  struct Event {
    std::string name;
    Portable args; // a tuple
    std::string where; // of the first emit: "main.ts:12:5"
  };
  // The events found so far, in the order found, by the place of the
  // emitter: `new EventEmitter()`, or the class that extends EventEmitter.
  std::map<std::string, std::vector<Event>> maps;
  bool open = false;    // set by Sema: some emitter's map comes from here
  bool changed = false; // set by Sema: the round found more
  bool final = false;   // set by the driver: the maps are complete
};

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
  // What `await` of a value of `type` gives (see awaitedType), for CodeGen.
  static const Type *awaitedTypeOf(const Type *type) { return awaitedType(type); }
  // Where the event maps found from emit() calls are kept between rounds
  // (see EventInference); without it they are errors.
  void setEventInference(EventInference *inference) { inference_ = inference; }

private:
  struct ScopeEntry {
    VarSymbol *var = nullptr;
    FuncSymbol *func = nullptr;
    EnumDecl *enumDecl = nullptr;
    ClassDecl *cls = nullptr;
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
    // Fields of a union narrowed by checks: `o.shape` is a Circle inside
    // `if (o.shape.kind == "circle")`. Forgotten as definedFields are.
    std::vector<std::pair<FieldPath, const Type *>> narrowedFields;
    // Variables of a type parameter whose type a check showed: `x` of type
    // T is a string inside `if (typeof x == "string")`.
    std::vector<std::pair<const VarSymbol *, const Type *>> narrowed;
    // Result variables known to hold an Ok, or an Err (after `r.isOk()`).
    llvm::SmallPtrSet<const VarSymbol *, 2> okResults, errResults;
    // In a constructor, or the initializers of the fields: the fields of
    // `this` set so far (see sema_class.cpp). In the constructor of a class
    // that extends another, `this` does not exist before `super(...)`.
    std::vector<llvm::StringRef> setFields;
    bool beforeSuper = false;
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
  // The fields of an object type written in the source; with `overloads`,
  // its methods with several signatures go there (only an interface has
  // them).
  std::vector<Field> resolveFields(const TypeRef &ref, std::vector<Overload> *overloads = nullptr);

  // Type aliases and enums (sema_decls.cpp). They are visible in the whole
  // block that declares them, so each block declares them before anything
  // else. Returns those declared; without `resolve` they are resolved later
  // with resolveTypes, as the types of a file may come from other files.
  std::vector<Stmt *> declareTypes(llvm::ArrayRef<Stmt *> stmts, bool resolve = true);
  void resolveTypes(llvm::ArrayRef<Stmt *> declared);
  Stmt *lookupType(llvm::StringRef name) const;
  const Type *resolveAlias(TypeAliasDecl *alias);

  // Generics (sema_generic.cpp). The body of a generic function is checked
  // once, with opaque type parameters (Type::Kind::Param) that allow what
  // TypeScript allows on any type: assigning, passing, returning, `==`,
  // `typeof`, printing, and turning into a string. A call infers the type
  // arguments and calls an instance: a copy of the function whose body is
  // checked again with those types and every rule of the language, so that
  // what our rules reject there (an object turned into a string, `<` on
  // objects) is reported at the call. Type arguments that look the same in
  // memory share one instance (see representation).
  struct Signature;
  struct Instance {
    FunctionDecl *decl = nullptr; // the copy, which CodeGen emits
    FuncSymbol *generic = nullptr;
    // An instance of a generic class instead (see instantiateClass): the
    // class made, whose bodies are checked as those of a copy of a function.
    ClassDecl *cls = nullptr;
    std::vector<const Type *> args;     // representations
    std::vector<const Type *> concrete; // as the call that made it gave them
    std::vector<const Type *> exact;    // see exactClassArgs
    llvm::SMLoc callLoc;
    const Instance *requester = nullptr; // the instance whose body needs it
    bool checking = false, checked = false, failed = false;
    // The first error in its body, where it is, and the instance that error
    // comes from (itself, or one that it calls).
    std::string message;
    llvm::SMLoc errorLoc;
  };
  // The type parameters of a generic function or alias: new opaque types
  // with their constraints, visible by name in the innermost type scope.
  std::vector<const Type *> declareTypeParams(llvm::ArrayRef<TypeParamDecl> decls,
                                              std::vector<const Type *> *defaults);
  // Makes the names of `decls` stand for `types` in the innermost type scope.
  void bindTypeParams(llvm::ArrayRef<TypeParamDecl> decls, llvm::ArrayRef<const Type *> types);
  // `type` with `params` replaced by `args`.
  const Type *substitute(const Type *type, llvm::ArrayRef<const Type *> params,
                         llvm::ArrayRef<const Type *> args);
  // What instances of a generic function are made for: types whose values
  // look the same in memory, and to code that cannot reach their fields, map
  // to one. All objects are a pointer (released through their descriptor),
  // all functions a code and an environment.
  static const Type *representation(const Type *type);
  // Whether `arg` may stand for type parameter `param`.
  // A constraint that mentions the type parameters `params`, `K extends
  // keyof T`, is checked with the types `args` they stand for.
  bool satisfies(const Type *arg, const Type *param, llvm::ArrayRef<const Type *> params = {},
                 llvm::ArrayRef<const Type *> args = {});
  // `T extends { name: string }`: whether `arg` has at least the members of
  // the object type `constraint`, with the same types (a class may have a
  // method for one that holds a function); or is that type, or one declared
  // to be it.
  bool hasMembersOf(const Type *arg, const Type *constraint);
  // Whether the copies of `generic` are made for each type its type
  // parameter `k` stands for rather than for each representation: the body
  // reads the fields of the type (an object constraint), or its signature
  // has a generic class of it, whose instances are classes of their own.
  bool exactTypeParam(FuncSymbol *generic, size_t k);
  // Calls of generic functions (and of the constructors of generic
  // classes): infers or resolves the type arguments into `bound` and checks
  // the arguments with them; false after an error.
  bool bindGenericCall(CallExpr *call, FuncSymbol *generic, llvm::SMLoc loc, const Type *expected,
                       llvm::ArrayRef<TypeRef> typeArgs, std::vector<const Type *> &bound);

  // Generic classes (sema_generic_class.cpp, docs/generics.md, section 5).
  // The bodies of a generic class are checked once, with its type
  // parameters opaque, as those of a generic function are. Each list of
  // type arguments makes an instance: a class of its own, parsed again from
  // the text of the generic class, whose fields and signatures are those of
  // the generic class with the types substituted, and whose bodies are
  // checked with those types, as copies of generic functions are (an error
  // there is reported where the instance is first used). CodeGen emits the
  // instances as classes; of the generic class itself, only its static
  // members, which all instances share.
  //
  // The opaque type parameters of a generic class, declared once.
  void declareClassTypeParams(ClassDecl *cls);
  // Makes the type parameters of a class (for an instance, the types they
  // stand for) visible by name in the innermost type scope.
  void bindClassTypeParams(const ClassDecl *cls);
  // `Box<int>`: the instance of `generic` for these type arguments (those
  // left out take their defaults), or the generic class itself for its own
  // type parameters. Null after an error, reported at `loc`.
  ClassDecl *instantiateClass(ClassDecl *generic, std::vector<const Type *> args, llvm::SMLoc loc);
  // Gives an instance the bases, fields and method signatures of its generic
  // class, which must have them by then; and once the classes are linked,
  // what linkClasses gives the others.
  void fillInstance(ClassDecl *inst);
  void linkInstance(ClassDecl *inst);
  // The FuncSymbol of `fn`, a member of an instance, from that of `generic`,
  // the member of the generic class it is a copy of.
  void declareInstanceMember(ClassDecl *inst, FunctionDecl *fn, FunctionDecl *generic);
  // `type` of the generic class of `inst` with the types of the instance.
  const Type *instanceType(const ClassDecl *inst, const Type *type);
  // Checks the bodies of an instance, as checkInstance does those of a copy
  // of a generic function.
  void checkClassInstance(Instance *instance);
  void reportClassInstance(Instance *instance, llvm::SMLoc loc);
  // `new Box(1)`: the type arguments from those of the constructor, and from
  // the type expected, as for a call of a generic function.
  Expr *checkGenericNew(NewExpr *expr, ClassDecl *generic, const Type *expected);
  // Whether two classes are one for access to private members: the same
  // class, or instances of one generic class (and the generic class).
  static bool sameClass(const ClassDecl *a, const ClassDecl *b) {
    return a && b && a->staticOwner() == b->staticOwner();
  }
  // The methods of an instance that are copies of `fn`, a method of a
  // generic class, and `fn` itself, marked as overridden by a class down
  // the chain: one instance's subclass makes the calls of each instance go
  // through its method table.
  void markOverridden(FunctionDecl *fn);
  // Numbers the classes for instanceof, so that a class is followed by the
  // classes down its chain; again at the end, with the instances made late.
  void numberClasses();
  // The vtable of `cls`, after that of the class it extends.
  void buildVtable(ClassDecl *cls);
  // The type whose operations a value of `type` has: the constraint of a
  // type parameter, `T extends number` is a number; `type` itself otherwise.
  static const Type *apparentType(const Type *type);
  // `expr` converted to its apparent type, so that `x * 2` with `x` of type
  // `T extends number` multiplies doubles.
  Expr *apparent(Expr *expr);
  FuncSymbol *genericNamed(const NameExpr *name) const;
  Expr *checkGenericCall(CallExpr *call, FuncSymbol *generic, llvm::SMLoc loc, const Type *expected,
                         llvm::ArrayRef<TypeRef> typeArgs);
  // A generic function as a value: its type arguments come from the
  // function type expected there, or are written (`first<int>`).
  Expr *checkGenericValue(NameExpr *name, FuncSymbol *generic, const Signature *expected);
  // Whether `ret`, the return type of `generic`, holds the type of an object
  // literal with methods, which each instance makes anew: such a result is
  // not supported yet (reported once).
  bool returnsOwnClass(const FuncSymbol *generic, const Type *ret);
  // The type arguments of a call (or a value with `call` unset), those
  // given or inferred, completed with the defaults. Reports those still
  // missing and those that do not satisfy their constraints; false then.
  bool finishTypeArgs(FuncSymbol *generic, std::vector<const Type *> &bound, llvm::SMLoc loc, bool call,
                      bool quiet = false);
  // The type an argument written as a literal (`1`, `[1, 2.5]`, `{ x: 1 }`)
  // has without an expected type, found before it is checked: it is checked
  // with the type its parameter gets, as `pair(1, 2.5)` makes 1 a double.
  // Null for other expressions.
  static const Type *literalShape(const Expr *expr);
  // The instance of `generic` for `args` (not mentioning type parameters).
  // `exact`, by parameter, the classes known when compiling that the call
  // gives where `Constructor<T>` is taken (null where none): the copy takes
  // each as the class it is (see exactClassArgs).
  FuncSymbol *instantiate(FuncSymbol *generic, llvm::ArrayRef<const Type *> args, llvm::SMLoc loc,
                          llvm::ArrayRef<const Type *> exact = {});
  // A class known when compiling, `typeof C`, given to a generic function
  // where it takes `Constructor<T>`: its copy for C takes C itself, so that
  // `new cls()` and `x instanceof cls` in it are those of C, without
  // reflection (docs/decorators.md, plan step 5). The arguments become the
  // classes; the types they are, by parameter.
  std::vector<const Type *> exactClassArgs(CallExpr *call, const FuncSymbol *generic);
  // A copy of `generic` with its type parameters standing for `args`: the
  // signature from the generic one, the body parsed again.
  FunctionDecl *instanceDecl(FuncSymbol *generic, llvm::ArrayRef<const Type *> args,
                             llvm::ArrayRef<const Type *> exact = {});
  void checkInstance(Instance *instance);
  // Reports an instance whose body has an error at the call that needs it.
  void reportInstance(Instance *instance, llvm::SMLoc callLoc, llvm::ArrayRef<const Type *> concrete);
  void checkInstances();
  // `Pair<int>`: the type a generic alias names for these type arguments.
  // Those left out take their defaults.
  const Type *instantiateAlias(TypeAliasDecl *alias, std::vector<const Type *> args, llvm::SMLoc loc);
  // The type parameters of a generic alias, made when first needed.
  std::vector<const Type *> aliasTypeParams(TypeAliasDecl *alias);
  // Generic code decides how a value of a type parameter is converted (to
  // its constraint for arithmetic, to a string next to one); an instance
  // converts the value of the same expression the same way, whatever its
  // type there.
  void noteForced(const Expr *expr, const Type *to);
  Expr *forceConversion(Expr *expr, const Type *to);
  // In an instance: the value of a condition that its types decide, such as
  // `typeof x == "string"` with x a number. The branch that cannot run is
  // dropped unchecked: it was written for other types.
  std::optional<bool> constantCondition(const Expr *cond) const;
  const Type *narrowedType(const VarSymbol *var) const;
  // `typeof x == "number"` for x of type parameter `param`: a type parameter
  // that is a number (a double for an unconstrained one).
  const Type *numberParam(const Type *param) const;
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
  // Finds those cycles among the functions without a return type. A name in
  // a body may call the function of that name among them, or the one
  // `resolve` gives for the function the body is in.
  void
  findCycles(llvm::ArrayRef<FunctionDecl *> functions,
             llvm::function_ref<FunctionDecl *(FunctionDecl *, llvm::StringRef)> resolve = nullptr);
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
    std::vector<YieldExpr *> *yields = nullptr;
    bool trial = false;
    const FuncSymbol *early = nullptr;
    Module *module = nullptr;
    Instance *instance = nullptr;
    std::vector<Diagnostics::Captured> *capture = nullptr;
  };
  // Saves the state and starts a fresh one for checking the body of `fn`:
  // only the globals of its file are visible to a top-level function, and to
  // a closure what is visible where it is declared.
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
    // With voidResult: an async function may go here, and its Promise is
    // dropped (the callback of a timer); elsewhere that is an error.
    bool asyncVoid = false;
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
  // An async function where the method uses what the function returns, for
  // which JavaScript would take the Promise: an error, and true.
  bool asyncCallbackProblem(const Expr *arg, const Signature &expected, llvm::StringRef method);
  // A built-in such as Math.sqrt or parseInt where a function is expected:
  // an arrow calling it, or null if `arg` names none.
  FunctionExpr *builtinFunctionValue(Expr *arg, const Signature &expected, unsigned primary,
                                      llvm::StringRef method);
  // `expected` is the type wanted of the result: an array of objects that
  // map's callback returns, `xs.map(x => ({ x, y: 0 }))`, gives the objects
  // their type.
  Expr *checkCallbackMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  // Whether an arrow given to filter returns true only for a first argument
  // that holds a value: `x => x != undefined`, `x => !!x && x.length > 0`.
  bool keepsDefined(const Expr *callback) const;
  // A function known by name (a declaration or a constant arrow) where a
  // function type that passes more or fewer arguments, or drops the result,
  // is expected: a conversion that CodeGen turns into a small adapter. Null
  // if that type does not fit.
  Expr *adaptFunction(Expr *expr, const Type *to);
  // Any function value where a function type with the same parameters and
  // a result its own converts to is expected, as TypeScript allows: an
  // async handler `async req => ...` where `(req: Request) => Response |
  // Promise<Response>` is (sema_library.cpp). Null if the types differ
  // otherwise.
  Expr *adaptResult(Expr *expr, const Type *to);
  // The function a name refers to directly, if any.
  FunctionDecl *namedFunction(const Expr *expr) const;
  // A function, or an array or optional of them.
  static bool holdsFunctions(const Type *type) {
    while (type->isOptional() || type->isArray())
      type = type->inner();
    return type->isFunction();
  }
  // The same for objects, dictionaries among them.
  static bool holdsObjects(const Type *type) {
    while (type->isOptional() || type->isArray())
      type = type->inner();
    return type->isObject() || type->isDict();
  }
  // Dates, which join() writes as toString() does.
  static bool holdsDates(const Type *type) {
    while (type->isOptional() || type->isArray())
      type = type->inner();
    return type->isDate();
  }

  // Generators and iterators (sema_iter.cpp). A generator function returns
  // `Generator<T, R, N>`: declared, or found from its `yield` and `return`
  // statements as a return type is. Iterators are consumed by `for...of`,
  // spreads, destructuring, Array.from and the helpers of ES2025; a chain of
  // helpers written where it is consumed runs as one loop (see markFused).
  //
  // `Generator<...>` and `IterableIterator<T>` in a type annotation.
  const Type *resolveIteratorType(const TypeRef &ref);
  // Checks the declared return type of a generator function.
  const Type *generatorReturnType(const FunctionDecl *fn, const Type *declared);
  // The type of a generator found from its body: what its yields give and
  // what its returns give; null if nothing is known yet (in a trial).
  const Type *inferGeneratorType(const FuncSymbol *fn, llvm::ArrayRef<YieldExpr *> yields,
                                 llvm::ArrayRef<ReturnStmt *> returns, bool fallsOff, bool report);
  Expr *checkYield(YieldExpr *yield);
  // The values iterating a value of `type` gives: the elements of an array,
  // the code points of a string, the values of an iterator; null otherwise.
  static const Type *iteratedType(const Type *type);
  // Iterating an iterator sends nothing to its `yield`: reports one whose
  // `yield` needs a value. `what` says who iterates.
  bool checkSendsNothing(const Type *iterator, llvm::SMLoc loc, llvm::StringRef what);
  // Marks the stages of the chain `expr` before it is checked: the consumer
  // that takes it apart runs it as one loop.
  void markFused(Expr *expr) const;
  // The methods of an iterator: next() and the helpers.
  Expr *checkIteratorMethod(CallExpr *call, MemberExpr *member);
  // keys(), values() and entries() of an array.
  Expr *checkArrayIterator(CallExpr *call, MemberExpr *member);
  // Iterator.from(x) and Array.from(x, f?).
  Expr *checkIteratorFrom(CallExpr *call, MemberExpr *member);
  Expr *checkArrayFrom(CallExpr *call, MemberExpr *member);
  // `const [a, b] = it`: the first values of the iterator, as an array.
  Expr *destructuredIterator(Expr *init, const VarDeclarator &d);

  // Collections of the program (sema_iterable.cpp, docs/iterators.md,
  // section 2): an object whose class (or literal) has a method
  // `[Symbol.iterator]()` is iterated through it, as in JavaScript; an
  // iterator written by hand, an object with next() that gives `{ done,
  // value }`, through a generator that calls next() (see iterateFunction),
  // which consumers emit inline. A parameter `xs: Iterable<int>` makes its
  // function generic: a copy for each kind of iterable it is called with.
  //
  // `x[Symbol.iterator]()` of a checked `x` whose class has the method, as
  // an iterator; null if it has none.
  Expr *userIterator(Expr *expr);
  // `x[Symbol.asyncIterator]()` of an object whose class (or literal) has
  // that method, an async generator; null for any other.
  Expr *asyncUserIterator(Expr *expr);
  // The async iterator `[Symbol.asyncIterator]()` of an object of `type`
  // gives, for the inference of `AsyncIterable<T>`; null without one.
  const Type *asyncIterableType(const Type *type);
  // `it`, an iterator written by hand (or an iterator already), as an
  // iterator; null if it has no next() that gives `{ done, value }`.
  Expr *iteratorOf(Expr *it);
  // The values next() of an object of `type` gives when it is not done, or
  // null if it has no such next().
  const Type *nextValueType(const Type *type);
  // The values iterating a value of `type` gives, whatever it is (an array,
  // a Map, an object with `[Symbol.iterator]()`...); null if it is not
  // iterable.
  const Type *elementTypeOf(const Type *type);
  // The generic generator that iterates an iterator written by hand,
  // `iterate<I, V>(it: I): Generator<V>`, made once.
  FuncSymbol *iterateFunction();
  // `IteratorResult<T, R>`: `{ done: false; value: T } | { done: true;
  // value: R }`, as in TypeScript.
  static const Type *iteratorResultType(const Type *value, const Type *returned);
  // `xs: Iterable<int>` in the signature of `fn`: a type parameter of its
  // own; null if `ref` is not that.
  const Type *iterableParam(FunctionDecl *fn, FuncSymbol *sym, const TypeRef &ref);
  // Whether `ref` is `Iterable<T>`, which makes a function generic.
  bool isIterableRef(const TypeRef &ref) const;
  FuncSymbol *iterate_ = nullptr;

  // Declarations and statements
  // `var` is the variable of a function declared in a block. Without
  // `scoped`, a method: its name is not declared in the scope.
  void declareFunction(FunctionDecl *fn, VarSymbol *var = nullptr, bool scoped = true);
  void checkFunctionBody(FunctionDecl *fn);
  void checkStmt(Stmt *stmt);
  void checkVarDecl(VarDeclStmt *decl);
  void checkReturn(ReturnStmt *ret);
  Expr *checkReturnValue(ReturnStmt *ret, const Type *hint, bool tested);
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

  // Classes (sema_class.cpp). A class is a type that is only itself; its
  // methods are functions that take the object as `this`. Classes are
  // declared with the types of their file, get their fields and the
  // signatures of their methods once types are known, and their bodies are
  // checked after top-level code, as functions are.
  void declareClass(ClassDecl *cls);
  void resolveClass(ClassDecl *cls);
  // The types of the fields that come from their initializers, found by
  // checking those (the method `init`); then the type has its fields.
  void completeClass(ClassDecl *cls);
  // FuncSymbols for the methods and constructors; and the reports about
  // variants of one name that a call cannot tell apart.
  void declareMethods(ClassDecl *cls);
  // A method whose parameters and result come from the function type
  // `declared`, where they are not written: an arrow in a field with a type,
  // or a method of a literal that goes where an interface is expected.
  void declareTyped(FunctionDecl *fn, const Type *declared);
  void checkVariants(ClassDecl *cls);
  // Reports variants `a` and `b` (declared after a) that no call can tell
  // apart.
  void checkVariantPair(FunctionDecl *a, FunctionDecl *b);
  void checkClassBodies(ClassDecl *cls);
  // The declaration in top-level code: the static fields get their values.
  void checkClassStmt(ClassDecl *cls);
  static bool hasDecorators(const ClassDecl *cls);
  // The class a type is an instance of, or null.
  static ClassDecl *classOf(const Type *type);
  // The methods of `cls` called `name`: its variants.
  static std::vector<FunctionDecl *> methodsNamed(const ClassDecl *cls, llvm::StringRef name,
                                                  bool isStatic);
  // `this`, the object of the method being checked (arrows see the one of
  // the method around them), a constant that checkFunctionBody declares.
  Expr *checkThis(NameExpr *name);
  VarSymbol *declareThis(FunctionDecl *fn);
  // Reports a private or protected member used outside its class.
  bool checkAccess(const ClassDecl *cls, Access access, llvm::StringRef name, llvm::SMLoc loc);
  // `o.x` and `o.m` of an instance: a field, or a method bound to o.
  Expr *checkClassMember(MemberExpr *member, const Type *expected);
  // `C.x` and `C.m`: static fields are globals, static methods functions.
  Expr *checkStaticMember(MemberExpr *member, ClassDecl *cls, const Type *expected);
  // An assignment to `C.x`: the global it is.
  const Type *checkStaticTarget(Expr *&target, ClassDecl *cls);
  Expr *checkMethodCall(CallExpr *call, MemberExpr *member, const Type *expected = nullptr);
  Expr *checkClassStaticCall(CallExpr *call, MemberExpr *member, ClassDecl *cls,
                             const Type *expected = nullptr);
  Expr *checkClassNew(NewExpr *expr, ClassDecl *cls, const Type *expected = nullptr);
  Expr *checkInstanceOf(BinaryExpr *bin);
  // An object literal with methods: an instance of a class of its own. Where
  // an interface is expected, its fields and methods take their types from
  // it, and the class implements it.
  Expr *checkMethodLiteral(ObjectLitExpr *lit, const Type *target);
  bool implementsLiteral(ObjectLitExpr *lit, const Type *target);
  // Overloads (docs/overloads.md). The variant of `variants` a call with
  // `args` chooses, or null after an error. With several variants that take
  // that many arguments, the arguments are checked here and `checked` is
  // set: the caller only converts them.
  // A generic variant (docs/overloads.md, plan step 3) is chosen with the
  // type arguments that the arguments give it, which go to `bound`; at equal
  // fit, a variant without type parameters wins, as in C#.
  FuncSymbol *chooseVariant(llvm::ArrayRef<FunctionDecl *> variants, std::vector<Expr *> &args,
                            const std::vector<bool> &spread, llvm::SMLoc loc, bool &checked,
                            std::vector<const Type *> *bound = nullptr);
  // The type arguments of a call of `generic` with these checked arguments,
  // by inference alone; false if some cannot be found or do not fit, with
  // nothing reported: a variant that does not fit is left out.
  bool inferFromArguments(FuncSymbol *generic, llvm::ArrayRef<Expr *> args, std::vector<const Type *> &bound);
  // A call of `sym`, a generic method (with `method`) or static method that
  // chooseVariant chose, whose arguments are checked, with type arguments
  // `bound`.
  Expr *callGenericVariant(CallExpr *call, FuncSymbol *sym, llvm::ArrayRef<const Type *> bound,
                           llvm::SMLoc loc, bool method);
  // A method as a value: the variant whose type `expected` asks for.
  FuncSymbol *variantForType(llvm::ArrayRef<FunctionDecl *> variants, const Type *expected,
                             const MemberExpr *member);
  // The rules for `this` while the fields are set: in a constructor (or the
  // initializers), until every field has a value, `this` may only be used to
  // set fields and to read those set already.
  bool thisReady(const Flow &flow) const;
  std::string firstUnset(const Flow &flow) const;
  // Whether `expr` is the `this` of the constructor being checked, used
  // directly in it.
  bool isInitThis(const Expr *expr) const;
  void checkFieldsSet(llvm::SMLoc loc);

  // Inheritance and interfaces (sema_inherit.cpp). A class extends one class
  // and implements interfaces; an interface extends interfaces. Those are
  // the only subtypes (docs/objects.md, section 2): a value of a type is one
  // of each type up its chain. A call of a method that some class down the
  // chain overrides goes through the method table of the object's class;
  // any other call is direct.
  //
  // `extends` and `implements` of a class, as it is resolved.
  void resolveClassBases(ClassDecl *cls);
  // The fields `interface X extends A, B` gets from A and B, followed by its
  // own; the bases go to `bases`.
  std::vector<Field> interfaceFields(TypeAliasDecl *alias, std::vector<const Type *> &bases,
                                     std::vector<Overload> &overloads);
  // Records that values of `type` are values of `base` too.
  void addBase(const Type *type, const Type *base);
  // Once every class has its methods: which method overrides which, the
  // methods that classes down the chain override, the numbers of the classes
  // for instanceof and the slots of their method tables.
  void linkClasses();
  void linkOverrides(ClassDecl *cls);
  // What `cls` must implement: the abstract methods up its chain, and the
  // members of the interfaces it implements.
  void checkAbstract(ClassDecl *cls);
  void checkImplements(ClassDecl *cls);
  // Reports a method that a constructor calls on `this` (directly or through
  // other methods) and that a class down the chain overrides: it would run
  // before that class has set its fields.
  void checkConstructorCalls();
  void scanConstructorCalls(const FunctionDecl *fn, const ClassDecl *cls, const CallExpr *origin,
                            const FunctionDecl *ctor,
                            llvm::SmallPtrSetImpl<const FunctionDecl *> &seen);
  // A field of the instances of `cls`, its own or of a class up its chain
  // (the nearest declaration), and the class that declares it.
  static const ClassField *findField(const ClassDecl *cls, llvm::StringRef name,
                                     const ClassDecl **owner = nullptr);
  // The constructors `new cls(...)` chooses from: its own, or those of the
  // nearest class up its chain that has any.
  static std::vector<FunctionDecl *> constructorsOf(const ClassDecl *cls);
  // The instance methods a class declares itself: its methods, and the
  // arrows in its fields that are compiled as methods.
  static std::vector<FunctionDecl *> ownMethods(const ClassDecl *cls);
  // Whether `fn` replaces `base`, directly or through other overrides.
  static bool overridesTransitively(const FunctionDecl *fn, const FunctionDecl *base);
  // The one method with a body that a call of `variant` on an object of
  // class `cls` (or of a class down its chain) may run, or null if there are
  // several (or none): then the call goes through the method table.
  static FunctionDecl *soleImplementation(const ClassDecl *cls, FunctionDecl *variant);
  // The return type of a method that overrides another one, or implements a
  // method of an interface, and declares none: the type the other one
  // returns. Null for any other function.
  const Type *inheritedReturnType(FuncSymbol *fn);
  // After an error in a return of `fn`: where its return type comes from,
  // if it overrides a method or implements one of an interface.
  void noteInheritedReturn(const FunctionDecl *fn);
  // `super(args)` in a constructor and `super.m(args)` in a method.
  Expr *checkSuperCall(CallExpr *call);
  Expr *checkSuperMethodCall(CallExpr *call, MemberExpr *member);
  // The nearest class that both classes are, or null.
  static const Type *commonClass(const Type *a, const Type *b);

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
  Expr *checkExprKind(Expr *expr, const Type *expected, bool wraps);
  Expr *checkIntLit(IntLitExpr *lit, const Type *expected, bool negated);
  Expr *checkName(NameExpr *name);
  const Type *varType(VarSymbol *var);
  // Reports a use of a variable declared by an earlier case of a switch.
  void checkVisible(NameExpr *name, const VarSymbol *var);
  Expr *checkUnary(UnaryExpr *unary, const Type *expected, bool wraps);
  // `delete x[i]` and `key in x`: errors but for dictionaries (see sema.cpp).
  Expr *checkDelete(UnaryExpr *unary);
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
  Expr *checkDirectCall(CallExpr *call, FuncSymbol *func, llvm::SMLoc loc, const Type *expected = nullptr);
  // `checked` marks the arguments checked already (a generic call infers
  // its type arguments from them first, chooseVariant checks them to choose
  // a variant): those are only converted.
  Expr *checkArguments(CallExpr *call, llvm::ArrayRef<const Type *> params, size_t minArgs, bool hasRest,
                       llvm::SMLoc loc, const std::vector<bool> *checked = nullptr);
  Expr *checkConsoleLog(CallExpr *call);
  // process, Date.now() and performance.now() (sema_process.cpp). Each
  // returns null if what it is given is not about them.
  Expr *checkProcessCall(CallExpr *call, MemberExpr *member);
  Expr *checkProcessMember(MemberExpr *member);
  Expr *checkProcessEnvIndex(IndexExpr *index);
  // Assignments to the properties of process, of which only
  // `process.exitCode = code` is allowed.
  Expr *checkProcessAssign(AssignExpr *assign);
  // Whether `expr` is `process.name` of the global process.
  bool isProcessMember(const Expr *expr, llvm::StringRef name) const;
  // The code of `process.exit(code)` or `process.exitCode = code`: an int,
  // or undefined for none. The result is `int | undefined`.
  Expr *checkExitCode(Expr *code);
  // A call of a built-in at `loc` in place of a read such as `process.argv`;
  // `callee` is what it reads.
  Expr *builtinCall(llvm::SMLoc loc, Expr *callee, Builtin builtin, std::vector<Expr *> args,
                    const Type *type);

  // Built-in modules (sema_node.cpp, docs/io.md): node:fs, node:path,
  // node:os and node:process, also without "node:", and tz:io. A name a file
  // imports from one stands for a function, a constant or a class of it, or
  // for a namespace: `posix` of node:path, or `fs` of `import fs from "fs"`
  // (an error, but its uses are checked as `readFileSync` would be).
  struct NodeName {
    NodeModule module;
    llvm::StringRef name; // empty for the namespace
  };
  // `import { ... } from "node:fs"` and the like in `module`.
  void importBuiltinModule(Module *module, const ImportDecl &decl);
  // What `name` stands for where it is used, if a built-in module gave it.
  const NodeName *nodeName(llvm::StringRef name) const;
  // The classes Stats and Dirent of node:fs.
  void declareFsClasses();
  // Calls, reads and assignments that are about built-in modules; null if
  // `call`, `name` or `member` is not one. checkNodeCall may instead change
  // the callee to what it stands for (`process.exit` for `exit` of
  // node:process) and return null.
  Expr *checkNodeCall(CallExpr *call, const Type *expected = nullptr);
  Expr *checkNodeName(NameExpr *name);
  Expr *checkNodeMember(MemberExpr *member);
  // A name of node:process, `argv`, as what it is: `process.argv`; other
  // expressions as they are.
  Expr *processAlias(Expr *expr);
  // The calls of the functions of each module, by name.
  Expr *checkPathCall(CallExpr *call, NodeModule module, llvm::StringRef fn, llvm::SMLoc loc);
  Expr *checkOsCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);
  Expr *checkFsCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);
  // spawnSync, execFileSync and execSync of node:child_process
  // (sema_child.cpp).
  Expr *checkChildCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);
  // The class ExecSyncError, which execSync and execFileSync throw.
  void declareExecSyncError();
  // The value of a name of a module that is not called: a constant, or an
  // error.
  Expr *nodeValue(const NodeName &n, Expr *expr);
  // The properties of the literal of options at `call->args[index]` of the
  // function `fn`, which must be among `allowed`, by name; false after an
  // error.
  bool nodeOptions(CallExpr *call, size_t index, llvm::StringRef fn,
                   llvm::ArrayRef<llvm::StringRef> allowed, llvm::StringMap<Expr *> &options);
  // An encoding given to `fn`, which must be the literal "utf8"; false after
  // an error.
  // The encoding of a function of node:fs, a literal: its number (see
  // checkBufferEncoding), or null after an error. Only readFileSync and the
  // writes take the encodings of Buffer (`any`); the others give names in
  // utf8.
  Expr *checkEncoding(Expr *encoding, llvm::StringRef fn, bool any);
  // node:fs/promises, node:timers/promises and node:readline
  // (sema_async_io.cpp): the calls of their functions, by name.
  Expr *checkFsPromisesCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);
  Expr *checkTimersPromisesCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc,
                                const Type *expected = nullptr);
  // `scheduler.wait(ms)` and `scheduler.yield()`; the object of `member` is
  // the name of scheduler.
  Expr *checkSchedulerCall(CallExpr *call, MemberExpr *member);
  Expr *checkReadlineCall(CallExpr *call, NodeModule module, llvm::StringRef fn, llvm::SMLoc loc);
  // While checkFsCall checks a function of node:fs/promises: it takes the
  // name without Sync and its options, and no fd.
  bool fsAsync_ = false;
  // The option `signal` of a call, an AbortSignal of the library
  // (src/lib/web.ts) or undefined, or undefined for none: what the runtime
  // gets of it, its side of the signal (Builtin::LibNativeSignal).
  Expr *signalOption(Expr *value, llvm::SMLoc loc);
  // The properties, methods and assignments of the objects of the runtime
  // (Type::Runtime); none of them can be assigned to.
  Expr *checkRuntimeMember(MemberExpr *member);
  Expr *checkRuntimeMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  const Type *checkRuntimeTarget(MemberExpr *member, bool compound);
  static std::string runtimeMembers(const Type *type);
  // isFile() and the like of Stats and Dirent; null for other methods.
  Expr *checkFsClassMethod(CallExpr *call, MemberExpr *member, ClassDecl *cls);
  Expr *checkStatsDate(MemberExpr *member, ClassDecl *cls);
  // Stats, Dirent and SystemError: objects only the runtime makes.
  bool isRuntimeClass(const ClassDecl *cls) const;

  // node:crypto (sema_crypto.cpp, docs/crypto.md): the calls of its functions,
  // the methods and properties of its objects, `new X509Certificate`, the
  // global crypto of Web Crypto, and its classes as types. Each becomes
  // Builtin::Crypto (see crypto_api.h), or Builtin::CryptoKeyPair for
  // generateKeyPairSync.
  Expr *checkCryptoCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);
  Expr *checkCryptoMethod(CallExpr *call, MemberExpr *member);
  Expr *checkCryptoMember(MemberExpr *member);
  Expr *checkCryptoNew(NewExpr *expr, llvm::StringRef cls);
  Expr *checkWebCryptoCall(CallExpr *call, MemberExpr *member);
  Expr *checkWebCryptoMember(MemberExpr *member);
  Expr *checkGenerateKeyPair(CallExpr *call, llvm::SMLoc loc);
  Expr *cryptoConstant(MemberExpr *member);
  // The class of node:crypto that `name` names in a type, or null.
  const Type *cryptoTypeNamed(llvm::StringRef name) const;
  // The parts of those calls: the call of the runtime function `fn` with
  // `args`, literals for the arguments Sema fills in, and the checks of the
  // arguments (an error leaves the type error, or gives null or false).
  Expr *cryptoCall(CallExpr *call, CryptoFn fn, std::vector<Expr *> args, const Type *type);
  Expr *cryptoInt(llvm::SMLoc loc, int64_t value);
  Expr *cryptoDouble(llvm::SMLoc loc, double value);
  Expr *cryptoBool(llvm::SMLoc loc, bool value);
  Expr *cryptoString(llvm::SMLoc loc, llvm::StringRef text);
  Expr *cryptoUndefined(llvm::SMLoc loc);
  Expr *cryptoData(Expr *arg, llvm::StringRef what, bool strings, bool keys, bool buffers);
  Expr *cryptoEncoding(CallExpr *call, size_t index, llvm::StringRef fn, int fallback);
  Expr *cryptoText(Expr *made, Expr *encoding);
  bool cryptoOptions(Expr *arg, llvm::StringRef fn, llvm::ArrayRef<llvm::StringRef> allowed,
                     llvm::StringMap<Expr *> &options);
  int cryptoChoice(Expr *arg, llvm::StringRef what, llvm::ArrayRef<llvm::StringRef> values);
  Expr *cryptoMd(Expr *arg, bool hmac);
  Expr *cryptoCipher(Expr *arg);
  Expr *cryptoNumber(Expr *arg);
  Expr *cryptoKey(Expr *arg, llvm::StringRef what);
  Expr *cryptoKeyObject(Expr *arg, llvm::StringRef what);
  bool cryptoSignKey(Expr *arg, llvm::StringRef fn, bool priv, std::vector<Expr *> &args);
  bool rejectCryptoCallback(CallExpr *call, size_t index, llvm::StringRef fn,
                            llvm::StringRef hint);
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
  Expr *checkArrayMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkNew(NewExpr *expr, const Type *expected);
  Expr *checkArrayLit(ArrayLitExpr *lit, const Type *expected);
  // Objects (sema_object.cpp). A literal takes the type expected where it is
  // written, and its fields the types of that type's fields; without one it
  // has the types of its values.
  Expr *checkObjectLit(ObjectLitExpr *lit, const Type *expected);
  // A literal with spreads, `{ ...a, x: 1 }`: the fields of all its parts.
  // `named`: whether each property names a field (a key in brackets that is
  // not a constant does not).
  Expr *checkSpreadLit(ObjectLitExpr *lit, const Type *target, llvm::ArrayRef<bool> named, bool ok);
  // Keys in brackets: the field a constant key names (a string literal, a
  // template without values, an enum member or a constant holding one of
  // those), or nothing.
  std::optional<std::string> constantKey(const Expr *key) const;
  // The fields `o[key]` may stand for, judged by the form of the unchecked
  // key alone (see SourceFile::stringConstants).
  void keyNames(const Expr *key, llvm::function_ref<void(llvm::StringRef)> each) const;
  // `o[key]` of an object or a tuple, whose object is checked: the field it
  // names, as `o.name`, or an error. With `write`, as an assignment target.
  Expr *checkKeyedField(IndexExpr *index, bool write);
  // The type of the fields of `type` that a key of `keyType`, string literals
  // only, may name, when they are all of one type; null if it is no such key.
  static const Type *keyedFieldType(const Type *type, const Type *keyType);
  // Tuples: `t.length`, and the methods of arrays, which tuples lack.
  Expr *checkTupleMember(MemberExpr *member);
  Expr *checkTupleMethod(CallExpr *call, MemberExpr *member);
  Expr *checkTupleLit(ArrayLitExpr *lit, const Type *tuple);
  // The type one array holds the elements of a tuple in, or null.
  static const Type *tupleElement(const Type *tuple);
  // Object.keys, Object.values and Object.entries, and for dictionaries
  // Object.fromEntries and Object.groupBy.
  Expr *checkObjectCall(CallExpr *call, MemberExpr *member, const Type *expected);
  // `for (const k in o)`: the loop over Object.keys(o).
  void checkForIn(ForOfStmt *stmt);
  // `object.name` of an object, whose object is checked.
  Expr *checkField(MemberExpr *member);
  // `object.name(args)` of an object: a call of the function in the field.
  Expr *checkFieldCall(CallExpr *call, MemberExpr *member);
  // A method of an interface with several signatures (sema_inherit.cpp):
  // called, the signature its arguments choose; as a value, the one the
  // place it goes asks for. A variant of a method for each signature, which
  // the choice goes through.
  Expr *checkOverloadCall(CallExpr *call, MemberExpr *member);
  Expr *checkOverloadMember(MemberExpr *member, const Type *expected);
  std::vector<FunctionDecl *> signatureVariants(const Type *iface, const Overload &overload);
  // The field `o.x = v` assigns to; for an instance of a class, null when
  // its type comes from this very assignment, in the initializers.
  const Type *checkFieldTarget(MemberExpr *member, bool compound = false);
  const Type *checkObjectFieldTarget(MemberExpr *member);
  // `o.f ??= v`: `o.f ?? (o.f = v)`, when the object can be read twice.
  // (`||=` and `&&=` on fields go through checkLogicalAssign.)
  Expr *checkFieldLogicalAssign(AssignExpr *assign);
  // Why `from` does not fit an object type `to`: the fields that differ.
  // `conflict`: the object also goes where a field has another type, so
  // giving it a type would not help.
  std::string objectMismatch(const Type *from, const Type *to, bool conflict = false) const;
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

  // Literals whose type comes from where the object goes
  // (sema_destination.cpp). `const p = { x: 1, y: 2 }; len(p)` with
  // `len(p: Point)` checks the literal as if the declaration said
  // `const p: Point`, so x and y are doubles. The places are found by
  // scanning the code that follows the declaration in its block: arguments of
  // functions known by name, typed variables and fields, returns, push...
  struct DestinationScan;
  // The type for the literal that initializes `d`, or null. `later` are the
  // declarators after it in the same declaration.
  const Type *literalDestination(const VarDeclarator &d, llvm::ArrayRef<VarDeclarator> later);
  // A literal whose fields may take their types from where it goes: an object
  // or array literal, `?:` with one, or map's callback returning one.
  static bool literalShaped(const Expr *expr);
  // Whether the literal names the fields of `type`, judged before it is
  // checked; a type that does not fit leaves the error to the place where
  // the object goes.
  bool literalFits(const Expr *expr, const Type *type) const;
  void scanDestinations(const Stmt *stmt, DestinationScan &scan);
  // Returns false once a declaration hides the variable for the rest.
  bool scanDestinations(llvm::ArrayRef<Stmt *> stmts, DestinationScan &scan);
  void scanDestinations(const Expr *expr, const Type *sink, DestinationScan &scan);
  void scanFunctionDestinations(const FunctionDecl *fn, DestinationScan &scan);
  // The type of a simple expression (a variable, its fields and elements)
  // without checking it, or null.
  const Type *peekType(const Expr *expr, const DestinationScan &scan) const;
  const Type *peekName(llvm::StringRef name, const DestinationScan &scan) const;
  // The type of a function declared in a block the scan enters, from its
  // annotations: what its arguments go to.
  const Type *declaredFunctionType(const FunctionDecl *fn);
  // What the arguments of a call go to, by position.
  std::vector<const Type *> calleeParams(const CallExpr *call, const DestinationScan &scan);

  // Result of neverthrow (sema_result.cpp). The names of neverthrow are
  // built in: what `name` stands for where it is used, "ok" or "Result" (also
  // through `import { ok as good } from "neverthrow"`), or empty if the
  // program declares that name itself.
  llvm::StringRef neverthrowName(llvm::StringRef name) const;
  llvm::StringRef neverthrowTypeName(llvm::StringRef name) const;
  // `import { ... } from "neverthrow"` in `module`.
  void importNeverthrow(Module *module, const ImportDecl &decl);
  // `Result<T, E>`, `Ok<T, E>` (Result<T, never>) and `Err<T, E>`.
  const Type *resolveResultType(const TypeRef &ref, llvm::StringRef name);
  // ok(v), err(e) and safeTry(...); null if `name` is none of them.
  Expr *checkResultCall(CallExpr *call, NameExpr *name, const Type *expected);
  // Result.combine and Result.combineWithAllErrors; null if `member` is not
  // a member of Result.
  Expr *checkResultStatic(CallExpr *call, MemberExpr *member);
  // `r.map(f)` and the other methods, whose receiver is checked.
  Expr *checkResultMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  // `r.value` and `r.error`.
  Expr *checkResultMember(MemberExpr *member);
  Expr *checkSafeTry(CallExpr *call, const Type *expected);
  // ok(v) and err(e).
  Expr *checkOkErr(CallExpr *call, NameExpr *name, bool isOk, const Type *expected);
  // ResultAsync (sema_async.cpp): okAsync, errAsync, fromPromise,
  // fromSafePromise and fromAsyncThrowable, called by the name `which`
  // stands for; the functions of ResultAsync (null if `member` is not one);
  // the methods of a ResultAsync, and asyncMap and asyncAndThen of a Result
  // (`name`, which the method is then).
  Expr *checkResultAsyncCall(CallExpr *call, NameExpr *name, llvm::StringRef which,
                             const Type *expected);
  Expr *checkResultAsyncStatic(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkResultFromPromise(CallExpr *call, llvm::SMLoc loc, bool safe, const Type *expected);
  Expr *checkResultAsyncThrowable(CallExpr *call, llvm::SMLoc loc);
  Expr *checkResultAsyncMethod(CallExpr *call, MemberExpr *member, llvm::StringRef name);
  // `yield* r` in the generator of safeTry.
  Expr *checkSafeTryYield(YieldExpr *yield);
  // The Result type of the generator `fn`: what it returns, `returned`, with
  // the errors its `yield*` may return.
  const Type *generatorResult(const FunctionDecl *fn, const Type *returned);
  // Reports each `yield*` of a generator that may return while a value
  // computed earlier in its statement is still held (see sema_result.cpp).
  void checkYieldPlacement(const Stmt *stmt);
  void checkYieldPlacement(const Expr *expr, bool safe);
  // The type that holds both halves `a` and `b` of Results, or null.
  static const Type *unifyHalf(const Type *a, const Type *b);
  // An operand of type never (the parameter of a function for the half of a
  // Result that cannot be there) takes the type of the other one, as in
  // TypeScript: `err("x").map(v => v + 1)` is fine.
  void neverOperands(BinaryExpr *bin);
  // A Result, or an array or optional of them.
  static bool holdsResults(const Type *type) {
    while (type->isOptional() || type->isArray())
      type = type->inner();
    return type->isResult();
  }
  // Reports a value that is computed and dropped if it is a Result: its
  // error would go unchecked. `callback`: what a function returns that its
  // caller drops. `at` is where its statement starts, for hints.
  void checkDiscarded(const Expr *expr, bool callback = false, llvm::SMLoc at = {});
  // `r.isOk()` and `r.isErr()` of a Result variable: what holds when it is
  // true and when it is false.
  void addResultFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const;

  // Exceptions (sema_error.cpp, docs/errors.md). Error is a class of the
  // language that every file sees, declared before them (declareErrorClass).
  // TypeError, RangeError and SyntaxError are, until classes inherit, that
  // class too, with a name and a class number of their own (kErrorKinds).
  void declareErrorClass();
  // An Error, of any kind: what `throw` takes and `catch` gives. With
  // inheritance, also an instance of a class derived from Error.
  bool isErrorType(const Type *type) const;
  void checkThrow(ThrowStmt *stmt);
  void checkTry(TryStmt *stmt);
  // `e.stack` of an Error, which is made from the path of the error.
  Expr *checkErrorStack(MemberExpr *member);
  // `e.toString()`; null if `member` is not that.
  Expr *checkErrorMethod(CallExpr *call, MemberExpr *member);
  // Result.fromThrowable(f, toError?) and neverthrow's fromThrowable
  // (sema_result.cpp); `loc` is where the callee is.
  Expr *checkFromThrowable(CallExpr *call, llvm::SMLoc loc);
  // `flow` without what code `scan` scanned may change.
  Flow withoutChanges(const Flow &flow, const LoopScan &scan);

  Expr *checkCast(CastExpr *cast);
  // A template literal; where a template literal type is expected, it has
  // the type its parts make (`` `on${string}` ``).
  Expr *checkTemplate(TemplateExpr *tmpl, const Type *expected = nullptr);
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

  // decimal (sema_decimal.cpp). A number literal, or an expression made only
  // of literals, read where a decimal is expected or next to a decimal
  // operand takes its exact value from the text of the program. Literals have
  // no effects, so checking the other operand first changes nothing else.
  static bool isLiteralTree(const Expr *expr);
  // The hint for a literal tree next to `other`, which is checked: decimal if
  // `other` is one.
  static const Type *literalHint(const Expr *other, const Type *hint);
  // Checks both operands of a comparison, in that order.
  void checkComparisonOperands(BinaryExpr *bin);
  // The exact value of a checked number literal or its negation, if it is one.
  static std::optional<DecimalLiteral> decimalLiteral(const Expr *expr);
  void checkDecimalLiteralRange(const Expr *lit);
  // Arithmetic and comparisons where a side is a decimal, both sides checked:
  // an int or a long becomes a decimal, a double is an error.
  Expr *checkDecimalArithmetic(BinaryExpr *bin);
  Expr *checkDecimalComparison(BinaryExpr *bin);
  // Reports the sides of `bin`, one a decimal, which do not go together.
  void reportDecimalMix(const BinaryExpr *bin);
  // `x op= v` on a decimal x, with v checked.
  void checkDecimalAssign(AssignExpr *assign, BinaryOp op);
  // A decimal divided by a constant zero.
  void checkDecimalDivisor(BinaryOp op, const Expr *divisor);
  Expr *checkDecimalCall(CallExpr *call, llvm::SMLoc loc);
  Expr *checkDecimalStatic(CallExpr *call, MemberExpr *member);
  Expr *checkDecimalMethod(CallExpr *call, MemberExpr *member);
  // A rounding mode argument: a string, checked now if it is a literal.
  Expr *checkRoundingMode(Expr *arg);
  // An integer argument of a decimal method, computed as long.
  Expr *checkDecimalCount(Expr *arg, const char *what);
  // `BigInt(x)`, the type bigint and the like, which do not exist.
  void reportBigInt(llvm::SMLoc loc, llvm::StringRef what);

  // Regular expressions (sema_regex.cpp). A literal is compiled here, so its
  // syntax errors are compile errors, and its groups and flags are known;
  // so are those of a variable that holds a literal all along. A regex from
  // anywhere else (a parameter, a field, `new RegExp(s)`) is of a kind known
  // only when the program runs.
  Expr *checkRegexLit(RegexLitExpr *lit);
  // The literal `expr` surely holds, or null.
  static const RegexLitExpr *knownRegex(const Expr *expr);
  // The type of a match of `regex`; null for a regex not known.
  const Type *matchType(const RegexLitExpr *regex);
  // Notes a variable that holds the literal of its initializer all along;
  // `block` and `index` are where it is declared.
  void noteRegexVariable(VarSymbol *var, const Expr *init, bool isConst,
                         llvm::ArrayRef<Stmt *> block, size_t index);
  // re.test(s), re.exec(s), re.toString() and the properties of a regex.
  Expr *checkRegexMethod(CallExpr *call, MemberExpr *member);
  Expr *checkRegexMember(MemberExpr *member);
  // The methods of strings that take a regex: match, matchAll, search,
  // replace, replaceAll and split. Null for other methods.
  Expr *checkStringRegexMethod(CallExpr *call, MemberExpr *member);
  Expr *checkReplacement(CallExpr *call, MemberExpr *member, const RegexLitExpr *regex);
  // `new RegExp(pattern, flags)` and `RegExp(...)`; RegExp.escape(s), and
  // the static properties of RegExp, which are errors.
  Expr *checkRegExpConstructor(llvm::SMLoc loc, std::vector<Expr *> &args);
  Expr *checkRegExpStatic(CallExpr *call, MemberExpr *member);
  Expr *checkRegExpStaticMember(MemberExpr *member);
  // A match: index, input, groups and length; m[k]; and the match as the
  // array of its captures, for its methods, for...of, spreading and
  // `m ?? []` (of a `T | undefined` match, an optional array).
  Expr *checkMatchMember(MemberExpr *member);
  Expr *checkMatchIndex(IndexExpr *index);
  Expr *matchItems(Expr *match);
  // String(re) and `${re}`: "/source/flags".
  Expr *regexToString(Expr *regex);
  // Json (sema_json.cpp, docs/modules.md section 5). A Json is read as
  // JavaScript reads what JSON.parse gives: `j.name`, `j["name"]` and `j[0]`
  // are `Json | undefined` (from undefined or null the program stops, where
  // JavaScript throws), and checks narrow a Json variable: `typeof j ==
  // "string"`, `Array.isArray(j)`, `j != null`. A string, a number, a boolean
  // and a Json[] become a Json where one is expected, and so do object and
  // array literals; `null` exists only there.
  //
  // A Json, or an optional or an array of them.
  static bool holdsJson(const Type *type);
  // The literal `null`, before or after it is checked.
  static bool isNullName(const Expr *expr);
  // `null` where a Json is expected; null for anything else.
  Expr *checkNull(NameExpr *name, const Type *expected);
  // `j.name`, `j[key]` and `j[i]` of a Json, whose object is checked.
  Expr *checkJsonMember(MemberExpr *member);
  Expr *checkJsonIndex(IndexExpr *index);
  // `j.name = v` and `j[key] = v`: the type assigned, `Json | undefined`
  // (undefined removes the key).
  const Type *checkJsonTarget(Expr *target);
  // `j.map(f)`: a Json has no methods before it is narrowed.
  Expr *reportJsonMethod(CallExpr *call, MemberExpr *member);
  // `{ ... }` and `[...]` where a Json is expected.
  Expr *checkJsonObjectLit(ObjectLitExpr *lit);
  Expr *checkJsonArrayLit(ArrayLitExpr *lit);
  // `x == null`, before its sides are checked; null if neither is `null`.
  Expr *checkNullEquality(BinaryExpr *bin);
  // `==` and `!=` with a Json side, both sides checked.
  Expr *checkJsonEquality(BinaryExpr *bin);
  // What `typeof j == "string"`, `Array.isArray(j)` and `j != null` say
  // about a Json variable j.
  void addJsonFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const;
  // JSON.parse, JSON.parse<T> and JSON.stringify.
  Expr *checkJsonStatic(CallExpr *call, MemberExpr *member);
  // embedJson, embedText, embedBytes and fromJson<T>; null if `name` is none
  // of them.
  Expr *checkJsonGlobal(CallExpr *call, NameExpr *name);
  Expr *checkArrayIsArray(CallExpr *call, MemberExpr *member);
  // Reads the file the argument of embedJson or embedText names, relative to
  // the file of the call; false after reporting an error. `shown` is the
  // path as written.
  bool readEmbedded(CallExpr *call, llvm::StringRef fn, std::string &bytes, std::string &shown,
                    std::string &full);
  // The type T of JSON.parse<T> and fromJson<T>, which the compiler can
  // build from a Json; reports the part of it that it cannot.
  bool checkFromJsonType(const Type *type, llvm::SMLoc loc, llvm::StringRef fn);
  // Why a value of `type` cannot be built from a Json, or empty; a class
  // through `classProblem`, or not at all without it.
  using ClassProblem = llvm::function_ref<std::string(const ClassDecl *)>;
  static std::string jsonProblem(const Type *type, ClassProblem classProblem);

  // Dates (sema_date.cpp, docs/date.md). A date is a reference to an object
  // that holds its time value, as in JavaScript. What goes wrong in
  // JavaScript and is visible in the code is an error: a date string
  // JavaScript cannot read or reads as UTC, February 31, month 12, getYear.
  //
  // `new Date(...)`, and `Date()`, which gives the text of now.
  Expr *checkDateNew(llvm::SMLoc loc, std::vector<Expr *> &args);
  Expr *checkDateFunction(CallExpr *call, NameExpr *name);
  // Date.UTC and Date.parse (Date.now is in checkProcessCall).
  Expr *checkDateStatic(CallExpr *call, MemberExpr *member);
  Expr *checkDateStaticMember(MemberExpr *member);
  Expr *checkDateMethod(CallExpr *call, MemberExpr *member);
  Expr *checkDateMember(MemberExpr *member);
  // `a - b` and `a < b` of dates, whose sides are checked: their times. Null
  // if neither side is a date.
  Expr *checkDateOperands(BinaryExpr *bin);
  // getTime() and toString() of a checked date: for `+d`, Number(d), and
  // `${d}` and String(d).
  Expr *dateTime(Expr *date);
  Expr *dateToString(Expr *date);
  // Reports a date string written in the code that JavaScript cannot read,
  // or reads as UTC where a time would make it local; `arg` is not checked.
  void checkDateLiteral(const Expr *arg);
  // The numbers of `new Date(y, m, d...)` and Date.UTC, checked and made
  // doubles, with the errors of those written as constants.
  void checkDateFields(std::vector<Expr *> &args);
  // The locale and the options of toLocaleString and the like and of
  // Intl.DateTimeFormat (`kind` is TZ_DATE_LOCALE_*); false after an error.
  bool checkLocaleArgs(std::vector<Expr *> &args, int kind);
  // Intl.DateTimeFormat: made with or without `new`, its methods, and the
  // types of what formatToParts and resolvedOptions give.
  Expr *checkDateFormatNew(llvm::SMLoc loc, std::vector<Expr *> &args);
  Expr *checkIntlCall(CallExpr *call, MemberExpr *member);
  Expr *checkDateFormatMethod(CallExpr *call, MemberExpr *member);
  static const Type *dateFormatPartType();
  static const Type *resolvedDateOptionsType();
  // The rest of Intl (sema_intl.cpp, docs/intl.md): Intl.NumberFormat,
  // Collator, PluralRules, RelativeTimeFormat, ListFormat, DisplayNames and
  // Segmenter, made with `new` (NumberFormat and Collator also without), and
  // their methods; the values `nf.format`, `collator.compare` and
  // `dtf.format`, bound to their objects; toLocaleString of numbers and
  // decimals; localeCompare and normalize of strings; getCanonicalLocales and
  // supportedLocalesOf.
  Expr *checkIntlNew(llvm::SMLoc loc, llvm::StringRef name, std::vector<Expr *> &args,
                     bool withNew);
  // `Intl.X(...)` other than DateTimeFormat, and Intl.getCanonicalLocales.
  Expr *checkIntlFunction(CallExpr *call, MemberExpr *member);
  // `Intl.X.supportedLocalesOf(...)`: the callee's object is `Intl.X`.
  Expr *checkIntlStatic(CallExpr *call, MemberExpr *member);
  Expr *checkIntlMethod(CallExpr *call, MemberExpr *member);
  Expr *checkIntlMember(MemberExpr *member, const Type *expected);
  Expr *checkNumberLocale(CallExpr *call, MemberExpr *member);
  Expr *checkStringLocale(CallExpr *call, MemberExpr *member);
  // The locale and the options at args[from] and after (see sema_intl.cpp);
  // `kind` None for those of supportedLocalesOf. False after an error.
  bool checkIntlArgs(std::vector<Expr *> &args, size_t from, IntlKind kind,
                     llvm::StringRef what);
  Expr *checkLocaleList(Expr *arg, llvm::StringRef what);
  Expr *checkIntlNumber(Expr *arg, llvm::StringRef method);
  // The segments of Intl.Segmenter's segment(), where they are iterated: an
  // array of their data.
  Expr *segmentsArray(Expr *segments);
  static const Type *segmentDataType();
  static const Type *resolvedIntlOptionsType(IntlKind kind);
  // The type `Intl.<name>` names in an annotation (`Intl.NumberFormat`,
  // `Intl.NumberFormatOptions`...), or null.
  static const Type *intlTypeNamed(llvm::StringRef name);
  // Intl.DateTimeFormatOptions: every option, `T | undefined`.
  static const Type *dateOptionsType();
  // Dictionaries (sema_dict.cpp, docs/objects.md section 9): Record<string,
  // V>, also written `{ [key: string]: V }`, read and written as an object
  // is, and Map<K, V> and Set<T> with the methods of JavaScript. Their
  // operations become calls of built-ins (Builtin::MapGet...).
  //
  // `Record<K, V>`, `Map<K, V>`, `Set<T>` and the read-only names in a type
  // annotation; null if `ref` names none of them.
  const Type *resolveDictType(const TypeRef &ref);
  // WeakRef (sema_weak.cpp, docs/objects.md section 11): `WeakRef<T>` of an
  // object type T, `new WeakRef(x)` and `w.deref()`.
  const Type *resolveWeakRefType(const TypeRef &ref);
  bool checkWeakTarget(const Type *target, llvm::SMLoc loc);
  Expr *checkWeakRefNew(NewExpr *expr, const Type *expected);
  Expr *checkWeakRefMethod(CallExpr *call, MemberExpr *member);
  // Whether values of `type` can be keys; reports a type that cannot.
  bool checkKeyType(const Type *type, llvm::SMLoc loc);
  // A call of `builtin` on `receiver` (checked), as `receiver.name(args)`.
  Expr *dictCall(llvm::SMLoc loc, Expr *receiver, llvm::StringRef name, Builtin builtin,
                 std::vector<Expr *> args, const Type *type);
  // Where values are iterated: a Map as its entries(), a Set as its
  // values(), and anything else as it is.
  Expr *iterableOf(Expr *expr);
  // What `for await` over a value of `type` gives (docs/iterators.md, step
  // 4): the values of an async iterator, the lines of readline, or the
  // elements of an array or an iterator, awaited; null for none.
  const Type *asyncIteratedType(const Type *type);
  void checkForAwait(ForOfStmt *stmt);
  Expr *checkRecordKey(Expr *key);
  // The key `name` of `r.name`, a string literal.
  Expr *keyLiteral(llvm::SMLoc loc, llvm::StringRef name);
  // `m.size` and `r.key`, `r[key]`, whose object is a checked dictionary.
  Expr *checkDictMember(MemberExpr *member);
  Expr *checkDictIndex(IndexExpr *index);
  // `r[key] = v` and `r.key = v`, whose object is a checked dictionary: the
  // type assigned. recordAssign makes the checked assignment a call; null
  // if it is not to a Record.
  const Type *checkDictTarget(Expr *target, bool compound);
  Expr *recordAssign(AssignExpr *assign);
  // `r[key] ??= v`; null, with the object checked, if it is no Record.
  Expr *checkRecordNullishAssign(AssignExpr *assign, IndexExpr *index);
  // `delete r[key]` (`keyExpr`) and `delete r.key` (`name`), and `key in r`,
  // whose dictionary is checked.
  Expr *checkDictDelete(UnaryExpr *unary, Expr *object, Expr *keyExpr, llvm::StringRef name);
  Expr *checkDictIn(BinaryExpr *bin);
  // The methods of Map and Set, whose receiver is checked.
  Expr *checkDictMethod(CallExpr *call, MemberExpr *member);
  // `new Map(...)`, `new Set(...)`, Object.fromEntries, Map.groupBy and
  // Object.groupBy.
  bool inferPairs(Expr *arg, const Type *&key, const Type *&value);
  Expr *checkDictSource(Expr *arg, bool pairs, const Type *&element, llvm::StringRef what);
  Expr *checkDictNew(NewExpr *expr, const Type *expected);
  Expr *checkFromEntries(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkGroupBy(CallExpr *call, MemberExpr *member, bool map);
  Expr *checkMapStatic(CallExpr *call, MemberExpr *member);
  // Object.keys, Object.values and Object.entries of a dictionary, whose
  // argument is checked.
  Expr *checkDictObjectCall(CallExpr *call, llvm::StringRef fn);
  // An object literal where a Record is expected, and one whose spreads
  // make it a Record (`{ ...a, ...b }` of Records).
  Expr *checkRecordLit(ObjectLitExpr *lit, const Type *record);
  bool checkRecordSpread(ObjectLitExpr::Property &prop, const Type *value);
  Expr *recordFromSpreads(ObjectLitExpr *lit);
  // Unions and literal types (sema_union.cpp, docs/objects.md section 7).
  //
  // The member of the union `to` that a value of `from` (not a union) goes
  // into: the same type, its base for a literal, or else the first member
  // it converts to; null if none.
  static const Type *unionMemberFor(const Type *from, const Type *to);
  // What unify gives, or else the union of both where one can hold them
  // (`flag ? 1 : "one"`); null if neither.
  static const Type *unionUnify(const Type *a, const Type *b);
  // The literal type of a literal written where `expected` is wanted, if
  // literal types of its kind are expected there (`const d: Dir = "up"`);
  // null where it has its base type.
  const Type *stringLiteralFor(const Type *expected, const std::u16string &text);
  const Type *boolLiteralFor(const Type *expected, bool value);
  const Type *numberLiteralFor(const Type *expected, double value, bool fraction);
  // The number type a number literal takes where the union `expected` is
  // wanted: its member int, long, double or decimal; null if none.
  static const Type *numberMemberOf(const Type *expected, bool fraction);
  // The member of the union `target` that an object literal is written for:
  // the object type with the fields it names, told apart by the tags it
  // gives (`kind: "circle"`); null if none or several fit.
  const Type *literalMember(const ObjectLitExpr *lit, const Type *target) const;
  // The type of `var` in `flow`: narrowed, or holding a value, or declared.
  const Type *typeIn(const Flow &flow, const VarSymbol *var) const;
  // Narrows `var` to `type` in `flow`; null for no type left (never).
  static void narrowVariable(Flow &flow, const VarSymbol *var, const Type *type);
  // What a check narrows: a variable, or a field read through fields from
  // one, of a union (or a literal type, or boolean), declared as `declared`.
  struct NarrowTarget {
    const VarSymbol *var = nullptr;
    std::optional<FieldPath> path;
    const Type *declared = nullptr;
  };
  std::optional<NarrowTarget> narrowTarget(const Expr *expr) const;
  const Type *targetType(const Flow &flow, const NarrowTarget &target) const;
  void narrowTargetTo(Flow &flow, const NarrowTarget &target, const Type *type) const;
  // What a check says about a variable or a field of a union: its tag,
  // `x == "up"`, `typeof`, `in`, `instanceof`, `Array.isArray`.
  void addUnionFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const;
  // `o.shape` narrowed by a check: as its narrowed type; or `member` itself.
  Expr *narrowedField(MemberExpr *member);
  // `o.name` of a union of objects, whose object is checked: a field every
  // member has, of the union of their types.
  Expr *checkUnionMember(MemberExpr *member);
  // `s.label = v` of a union: a field of one type in every member, which
  // may be assigned to there.
  const Type *checkUnionFieldTarget(MemberExpr *member);
  // `s.area()` of a union of objects that no common type gives the method:
  // the method of each member, which must have one type.
  Expr *checkUnionMethodCall(CallExpr *call, MemberExpr *member);
  // A type that every member of the union `type` is declared to be and that
  // has a member `name` (a class they extend, an interface they implement):
  // the member is read through it. Null if none.
  const Type *commonBaseWith(const Type *type, llvm::StringRef name) const;
  // The values of the literal type a switch tests that no case handles;
  // nothing if it does not test one, or has a default.
  std::optional<std::vector<const Type *>> unhandledCases(const SwitchStmt *stmt) const;

  // Functions that test (sema_predicate.cpp, docs/narrowing.md): what holds
  // about the parameters of a function when it returns true, and when it
  // returns false, by their positions (-1 is `this`); the facts are about
  // the parameters' symbols, `params`.
  struct PredicateSide {
    bool unreachable = false;
    std::vector<std::pair<int, const Type *>> narrowed;
    std::vector<int> defined;
    std::vector<std::pair<int, llvm::SmallVector<llvm::StringRef, 2>>> fields;
    std::vector<Facts::Fact> facts;
  };
  struct Predicate {
    PredicateSide whenTrue, whenFalse;
    std::vector<const VarSymbol *> params;
    const VarSymbol *self = nullptr;
  };
  // The return type of `fn`, written: a predicate `x is T` is a boolean
  // (void with asserts), which `sym` records.
  const Type *resolveReturnType(FunctionDecl *fn, FuncSymbol *sym);
  // An expression whose value is surely a boolean, by its form.
  static bool looksBoolean(const Expr *expr);
  // Around the check of a body: the summary of its returns.
  void beginPredicate(FunctionDecl *fn);
  void notePredicateExit(const Flow &whenTrue, const Flow &whenFalse, llvm::SMLoc loc);
  void endPredicate(FunctionDecl *fn);
  // The summary of `fn`, from its body; null if it has none.
  const Predicate *predicateOf(const FuncSymbol *fn) const;
  // What a call of a function that tests says, in a condition.
  void applyPredicate(const CallExpr *call, Flow &whenTrue, Flow &whenFalse) const;
  // The element type that `filter` with the callback keeps, narrower than
  // `element`; null if none.
  const Type *filteredType(const Expr *callback, const Type *element) const;
  struct PredicateBuild {
    const FuncSymbol *fn; // null when the function returns no boolean
    const FunctionDecl *decl;
    Flow whenTrue, whenFalse;
  };
  std::vector<PredicateBuild> predicateBuilds_;
  mutable llvm::DenseMap<const FuncSymbol *, Predicate> predicates_;
  mutable llvm::SmallPtrSet<const FuncSymbol *, 8> summarizing_;

  // Async (sema_async.cpp, docs/async.md). `Promise<T>` in an annotation, or
  // null for another name.
  const Type *resolvePromiseType(const TypeRef &ref);
  // Checks the declared return type of an async function: a Promise.
  const Type *asyncReturnType(const FunctionDecl *fn, const Type *declared);
  // The type of an async function from its returns, whose Promises give
  // their values; null if nothing is known yet (in a trial).
  const Type *inferAsyncType(const FuncSymbol *fn, llvm::ArrayRef<ReturnStmt *> returns,
                             bool fallsOff, bool report);
  // Once the type is known: each `return` gives a value of it, or a Promise
  // of it.
  void finishAsyncReturns(FuncSymbol *fn, llvm::ArrayRef<ReturnStmt *> returns);
  Expr *checkAwait(AwaitExpr *await, const Type *expected);
  // What `await` of a value of `type` gives: the value of a Promise, of each
  // Promise a union may hold with the other members, or the value itself;
  // null when those make no type.
  static const Type *awaitedType(const Type *type);
  // A Promise whose rejection something handles: `p.catch(f)`, `p.then(f,
  // g)`, and finally() after those.
  bool isHandledPromise(const Expr *expr) const;
  void checkDiscardedPromise(const Expr *expr, bool callback, llvm::SMLoc at);
  static bool holdsPromises(const Type *type) {
    while (type->isOptional() || type->isArray())
      type = type->inner();
    return type->isPromise();
  }
  // What Promise.allSettled gives for each: `{ status, value?, reason? }`.
  const Type *settledResultType(const Type *value);
  // The functions `new Promise` gives its executor: `(value: T) => void`
  // (`() => void` for void) and `(reason: Error) => void`.
  static const Type *resolverType(const Type *value);
  const Type *rejecterType();
  const Type *executorValueType(const Expr *executor);
  Expr *checkPromiseNew(NewExpr *expr, const Type *expected);
  Expr *checkPromiseMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  bool checkPromiseList(Expr *&arg, const Type *wantedElement, const Type *&element,
                        std::vector<const Type *> &tuple, llvm::StringRef fn);
  // Promise.resolve and the others; Promise is the object of `member`.
  Expr *checkPromiseStatic(CallExpr *call, MemberExpr *member, const Type *expected);
  // The callback of a timer or a queue: `() => void`.
  Expr *checkTaskCallback(Expr *arg, llvm::StringRef fn);
  // setTimeout and the other globals of timers and queueMicrotask; null if
  // `name` is none of them.
  Expr *checkAsyncGlobal(CallExpr *call, NameExpr *name);
  Expr *checkNextTick(CallExpr *call, MemberExpr *member);
  // The type of what `init`, not checked yet, gives if it calls
  // setTimeout, setInterval or setImmediate; null otherwise.
  const Type *timerCallType(const Expr *init) const;
  Expr *checkTimerMethod(CallExpr *call, MemberExpr *member);

  // Binary data (sema_bytes.cpp, docs/bytes.md): typed arrays, Buffer,
  // ArrayBuffer, DataView, TextEncoder and TextDecoder, objects the runtime
  // makes. An element is read as an element of an array is: `u[i]` proven
  // in range is the element, otherwise it may be undefined. A write needs
  // that proof, and every value stored in an element (by index, fill, set,
  // a constructor, DataView.set…, Buffer.write…) must be proven to fit in
  // it: literals, masks, checks above, elements of a narrower kind.
  //
  // The type a name in an annotation or after `new` stands for, or null.
  static const Type *bytesTypeNamed(llvm::StringRef name);
  // `x instanceof Uint8Array` (or another type of binary data, `bytes`).
  Expr *checkBytesInstanceOf(BinaryExpr *bin, const Type *bytes);
  Expr *checkBytesNew(NewExpr *expr, const Type *type);
  // The values of `new Uint8Array([1, 2])` and Uint8Array.of(1, 2), each
  // checked to fit; with spreads, an array of them first.
  Expr *checkBytesValues(CallExpr *call, ArrayLitExpr *lit, const Type *type);
  // A checked array, iterator or typed array whose elements a typed array of
  // `target` copies: reports them when they may not fit.
  bool checkBytesSource(Expr *source, const Type *target);
  // `Uint8Array.from(xs, f)`, whose source and callback are args.
  Expr *checkBytesFromWith(CallExpr *call, const Type *type, const std::string &what);
  // An encoding of Buffer: a literal one becomes its number (an int
  // literal), another string is checked when the program runs.
  Expr *checkBufferEncoding(Expr *arg);
  Expr *checkTypedArrayMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  // The methods node gives Buffer; null for those of Uint8Array.
  Expr *checkBufferMethod(CallExpr *call, MemberExpr *member);
  Expr *checkDataViewMethod(CallExpr *call, MemberExpr *member);
  Expr *checkTextMethod(CallExpr *call, MemberExpr *member);
  Expr *checkBufferStatic(CallExpr *call, MemberExpr *member);
  Expr *checkBytesIndex(IndexExpr *index);
  // `u[i] = v`, `u[i] op= v` and `u[i]++` once the value is checked: the
  // index must be in range and the result must fit.
  void checkBytesStore(IndexExpr *index, const Expr *value, std::optional<BinaryOp> op,
                       llvm::SMLoc loc);
  Expr *checkBytesMember(MemberExpr *member);
  Expr *checkBytesMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  // Uint8Array.from, Buffer.alloc...; null if `member` is no static member
  // of binary data.
  Expr *checkBytesStatic(CallExpr *call, MemberExpr *member);
  Expr *checkBytesStaticMember(MemberExpr *member);
  Expr *checkEmbedBytes(CallExpr *call, NameExpr *name);
  // String(u) and `${u}`: the elements joined, or the text of a Buffer.
  Expr *bytesToString(Expr *expr);
  // What values an integer expression, checked, may have: from its form,
  // the elements it reads and the facts about the variables in it.
  ValueRange valueRange(const Expr *expr) const;
  bool provesFits(const Expr *expr, int64_t lo, int64_t hi) const;
  // Reports `value` (checked) stored in an element of the typed array type
  // `target` when it may not fit; `what` says where it goes.
  void checkFits(const Expr *value, const Type *target, llvm::StringRef what);
  // Whether 0 <= offset and offset + size <= the length of `object` (of a
  // DataView, an ArrayBuffer or a typed array).
  bool provesWithin(const Expr *object, const Expr *offset, int64_t size) const;
  // The callbacks of the methods of typed arrays, which are checked knowing
  // the range of the element they get, and, for map, the range their result
  // must fit in.
  struct ByteCallback {
    unsigned param = 0; // the element's
    int64_t lo = 0, hi = 0;
    const Type *target = nullptr; // map: the typed array the result goes to
  };
  // In the body of such a callback: the facts about its element, and the
  // check of what it returns.
  void addByteCallbackFacts(const FunctionDecl *fn);
  void checkByteCallbackResult(const FunctionDecl *fn, const Expr *value);

  // unknown and object (sema_unknown.cpp, docs/decorators.md section 4).
  // An object of the language (an instance, a literal, a date, a
  // dictionary, an iterator, a Promise, a class itself, a union of objects):
  // a reference with a layout.
  static bool isObjectReference(const Type *type);
  // An unknown, or an optional or an array of them.
  static bool holdsUnknown(const Type *type);
  // "; check what it holds first, e.g. ...": the end of a message about an
  // unknown or an object `expr` used as a `to`.
  std::string unknownHint(const Expr *expr, const Type *to);
  // Reports a use of `what` ("property 'x'"; empty for indexing) of an
  // unknown or an object, which needs a check first.
  void reportUnknownUse(const Expr *object, const std::string &what);
  // `x.name` and `x.m(...)` of an unknown, an object or a class as a value,
  // whose object is checked; checkUnknownMethod gives null for other
  // receivers.
  Expr *checkUnknownMember(MemberExpr *member);
  Expr *checkUnknownMethod(CallExpr *call, MemberExpr *member);
  // What `typeof x == "string"`, `x instanceof C` and `Array.isArray(x)` say
  // about an unknown or object variable x.
  void addUnknownFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const;

  // Decorators (sema_decorate.cpp, docs/decorators.md), and what they need:
  // functions with a `this` parameter, called with `f.call(obj, ...)` and
  // `f.apply(obj, args)`, whose arguments are an argument list
  // (sema_arglist.cpp); classes as values.
  Expr *checkThisCall(CallExpr *call, MemberExpr *member);
  // `C.name` and the like of a class as a value, whose object is checked.
  Expr *checkClassRefMember(MemberExpr *member);

  // Classes as values (sema_classvalue.cpp, docs/decorators.md sections 5
  // and 6). `typeof C` is the class C itself, known when compiling: `new`
  // of it makes a C, and its static members are C's. `Constructor<T>` is
  // some class whose objects are T, known only when the program runs: `new`
  // of it goes through the description of the class (see reflection). A
  // class expression makes a class when Sema checks it, once for each copy
  // of a generic function that has it; such a function is a template, whose
  // body is checked only in its copies (see isTemplate).
  //
  // What objects a class as a value makes: those of its class, or T of a
  // `Constructor<T>`; null for a class of JavaScript (`Number`).
  static const Type *constructedBy(const Type *classValue);
  // `typeof C` of the class the name C means here (see finalClass).
  const Type *classValueType(ClassDecl *cls);
  // What reading the getter `name` of the class gives; null without one.
  const Type *getterType(ClassDecl *cls, llvm::StringRef name);
  // `typeof x` of a name a file imports, before its imports are bound.
  const Type *importedValueType(Module *from, llvm::StringRef name);
  // The class that a decorator gave in place of `cls`, once the
  // declaration of cls has run; cls itself otherwise.
  static ClassDecl *finalClass(ClassDecl *cls);
  // `Constructor<T>` in an annotation, and `ConstructorArgs`: the list of
  // the parameters of the constructor of the class that declaringClass_
  // extends.
  const Type *resolveConstructorType(const TypeRef &ref);
  const Type *constructorArgs(llvm::SMLoc loc);
  // `new v(...)` of a class as a value, `expr->callee`.
  Expr *checkValueNew(NewExpr *expr, const Type *expected);
  // `class extends Base { ... }` as an expression: the class it makes.
  Expr *checkClassExpr(ClassExpr *expr, const Type *expected);
  // Makes the class of a class expression, or of a declaration whose base
  // is a value (`class A extends Mixin(B)`): its base, fields, methods and
  // place among the classes; its bodies are checked with the others at the
  // end (lateClasses_).
  void makeLateClass(ClassDecl *cls);
  // The static fields of `cls` get their values where its declaration (or
  // expression) is: checked there, in order.
  void checkStaticFields(ClassDecl *cls);
  // The bodies of the classes made while code was checked.
  void checkLateClasses();
  // Methods that a class made later may override (a class expression may
  // extend any class): a call of one goes through the method table.
  void markLateOverrides(SourceFile &program);
  void markLateOverrides(ClassDecl *cls);
  // The type parameters of the copy of a generic function whose body has
  // the class expression that made `cls`.
  void bindOuterTypeParams(const ClassDecl *cls);
  // Whether `generic`, a generic function, has a class expression in its
  // body: its copies make classes of their own, so only they are checked,
  // and each finds its own return type.
  bool isTemplate(const FuncSymbol *generic);
  // Whether the body of `fn` has a class expression.
  bool hasClassExpr(const FunctionDecl *fn);
  // Checks the body of a copy of a template right away, finding its return
  // type; an error is the copy's, reported where it is called.
  void checkTemplateInstance(Instance *instance);
  // A class decorator gave `replacement`, a class that extends `cls`, in
  // its place (docs/decorators.md, section 5).
  void replaceClass(ClassDecl *cls, ClassDecl *replacement, llvm::SMLoc loc);

  // Reflection (sema_reflect.cpp, docs/decorators.md section 6).
  // Reflect.get, Reflect.apply and Reflect.construct, with unknowns for the
  // values, checked when the program runs; and fromJson(cls, j).
  Expr *checkReflection(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkReflectArgs(Expr *args);
  Expr *checkFromJsonClass(CallExpr *call, NameExpr *name);
  // Why fromJson cannot make an object of `cls` (its constructor takes what
  // no Json gives...), or empty; finds the fields the Json sets after the
  // constructor runs (ClassDecl::jsonFields).
  std::string classJsonProblem(const ClassDecl *cls);
  // A value of `from` goes where one of `to` is: the classes reflection may
  // reach through it (see ClassDecl::reflectNew).
  void noteReflectedConversion(const Type *from, const Type *to);
  // At the end: which classes reflection reaches.
  void finishReflection();
  // The classes whose objects become objects or unknowns, those whose objects
  // Reflect.get looks into by their type, and whether it looks into any.
  llvm::SmallPtrSet<ClassDecl *, 8> escaped_;
  llvm::SmallPtrSet<ClassDecl *, 8> reflectedObjects_;
  bool reflectGetAny_ = false;
  // fromJson(cls, j) of a class known only when the program runs.
  bool jsonReflect_ = false;
  llvm::DenseMap<const ClassDecl *, std::string> jsonProblems_;
  // The class being declared, whose constructors may take
  // `...args: ConstructorArgs`.
  ClassDecl *declaringClass_ = nullptr;
  std::vector<ClassDecl *> lateClasses_;
  size_t lateChecked_ = 0;
  // The copies of templates whose bodies made late classes: an error in the
  // code of such a class is the copy's, reported where it is called.
  llvm::DenseMap<const ClassDecl *, Instance *> lateOwners_;
  std::map<Instance *, std::vector<Diagnostics::Captured>> lateErrors_;
  // The program uses reflection: `new` of a Constructor, Reflect.get...
  bool reflectionUsed_ = false;
  llvm::StringSet<> lateMethodNames_;
  llvm::DenseMap<const FunctionDecl *, bool> templates_;
  // The name of a class used as a value: `typeof C`.
  Expr *checkClassValue(NameExpr *name, ClassDecl *cls);
  // A new, checked expression of the class `cls` as a value.
  Expr *classValueAt(ClassDecl *cls, llvm::SMLoc loc);
  // `MethodDescriptor<A, R>`: `{ value: (this: unknown, ...args: A) => R;
  // writable, enumerable, configurable: boolean }`, what a decorator of a
  // method gets (docs/decorators.md, section 2). The types that made each,
  // for substitute.
  const Type *methodDescriptorType(const Type *args, const Type *ret);
  llvm::DenseMap<const Type *, std::pair<const Type *, const Type *>> descriptorOrigins_;
  // Applies the decorators of `cls` and of its members where its declaration
  // runs, after the static fields get their values (checkClassStmt): makes
  // and checks the statements that do it, cls->decorations.
  void decorateClass(ClassDecl *cls);
  // A decorator to apply: of the declaration itself, or (with `param` set) of
  // a parameter of `owner`, a method or a constructor.
  struct DecoratorUse {
    const Decorator *decorator;
    int param = -1;
    const FunctionDecl *owner = nullptr;
  };
  // The decorators of one declaration of `cls`: the class (`field` and
  // `method` null), a field or a method, with those of the parameters.
  void decorate(ClassDecl *cls, ClassField *field, FunctionDecl *method,
                llvm::ArrayRef<DecoratorUse> uses);
  // The parameters of the decorator `callee`, if they are known: false for
  // a rest parameter or a callee known only when checked.
  bool decoratorParams(Expr *callee, std::vector<const Type *> &params);
  // Reports two decorated members of one name: their records would be one.
  void checkDecoratedNames(ClassDecl *cls);

  // Metadata (docs/decorators.md, section 3): Reflect.defineMetadata and the
  // others, whose records by an object key (a decorator, a class, a
  // constant) have the type of the first one written, and by a string key
  // are unknown; and the records `design:*` the compiler writes.
  Expr *checkReflectCall(CallExpr *call, MemberExpr *member, const Type *expected);
  // The declaration an object key names (a function, a class, a constant);
  // null for anything else, which is a string key.
  const void *metadataKey(const Expr *key, std::string &label);
  // What the records of the object key `key` hold: known from a record
  // checked already, or from the functions that write one (metaWriters_),
  // checked now; or else from `expected`, where the read goes.
  const Type *metadataType(const void *key, llvm::StringRef label, llvm::SMLoc loc,
                           const Type *expected);
  struct MetaKeyType {
    const Type *type;
    llvm::SMLoc loc; // the record that gave it
  };
  llvm::DenseMap<const void *, MetaKeyType> metaTypes_;
  // The functions that write records, by the name of the key they write.
  llvm::StringMap<std::vector<FunctionDecl *>> metaWriters_;
  void findMetadataWriters(SourceFile &program);
  // Checks the body of `fn` now, out of turn, if it is not checked yet.
  void checkBodyEarly(FunctionDecl *fn);
  // The class a record `design:*` holds for a value of `type`: Number,
  // String, Array, Function, Object... or the class itself.
  Expr *designClass(const Type *type, llvm::SMLoc loc);
  ClassDecl *designConstructor(const Type *type);
  // Number, String, Boolean, Object, Array, Function, Date, RegExp, Map,
  // Set, Promise and the types of binary data as values: classes of
  // JavaScript, which only `design:*` records and comparisons use; null for
  // any other name.
  ClassDecl *builtinConstructor(llvm::StringRef name);
  llvm::StringMap<ClassDecl *> builtinConstructors_;
  // The records design:type, design:paramtypes and design:returntype of a
  // decorated field or method (or of the class: of its constructor `ctor`),
  // added to cls->decorations.
  void writeDesignRecords(ClassDecl *cls, ClassField *field, FunctionDecl *method,
                          const FunctionDecl *ctor, llvm::SMLoc loc);
  void writeDesignRecord(ClassDecl *cls, const char16_t *key, Expr *value, llvm::StringRef member,
                         llvm::SMLoc loc);
  // `args.length` and `args.join(sep)` of a list of parameters in generic
  // code; `t.join(sep)` of a tuple.
  Expr *checkListMethod(CallExpr *call, MemberExpr *member);
  Expr *checkTupleJoin(CallExpr *call, MemberExpr *member);

  // Events (sema_events.cpp, docs/events.md): node:events and the events of
  // the web platform, which tz has written in tz (src/lib), and what the
  // compiler adds to that code.
  //
  // Marks the classes of the built-in modules written in tz
  // (Module::builtin) and their fields `@__tz_hidden`; notes where their
  // code is, which alone may call the intrinsics `__tz_*`.
  void prepareLibraries(SourceFile &program);
  bool inLibrary(llvm::SMLoc loc) const;
  ClassDecl *libraryClass(llvm::StringRef name) const { return libClasses_.lookup(name); }
  // `__tz_listener<F>(u)` and the other intrinsics (Builtin::LibListener...);
  // null if `name` names none of them.
  Expr *checkLibCall(CallExpr *call, NameExpr *name);
  // `signal.onabort = handler` of the program, with both checked: a call of
  // setOnabort (src/lib/web.ts), as node's event handlers listen from when
  // they are first set; `aborted` and `reason`, an error; null for any other
  // assignment.
  // `logical`: the assignment of `o.x ??= v`, `||=` or `&&=`.
  Expr *onabortAssign(AssignExpr *assign, MemberExpr *member, bool logical);
  bool logicalAssign_ = false; // the next assignment checked is that of `o.x ??= v`
  // An emitter: an object of EventEmitter of node:events or of a class
  // that extends it.
  bool isEmitterClass(const ClassDecl *cls) const;
  // The event map of an emitter of `type`: the type argument of the
  // EventEmitter up its chain; null for any other type.
  const Type *eventMapOf(const Type *type) const;
  // Reports `events`, the type argument of EventEmitter, that is no event
  // map: an object type of tuples, or a Record of them.
  bool checkEventMap(const Type *events, llvm::SMLoc loc);
  // The arguments the listeners of the event `name` get, a tuple; `name`,
  // an argument of `method`, is checked: a constant, unless the map takes
  // any name. `label` gets the name for messages. Null after an error.
  const Type *eventArgs(const Type *events, Expr *&name, llvm::StringRef method,
                        std::string *label = nullptr);
  // The function a listener of events with these arguments is, returning
  // `ret`.
  static const Type *listenerType(const Type *args, const Type *ret);
  // A listener of the event `event` whose listeners get `args`: a function
  // of them that returns nothing, an async one, or one that takes `this`
  // (`kind`, as src/lib/events.ts numbers them), converted to an unknown, as
  // the emitter keeps it. `receiver` is the emitter, the `this` it gets.
  enum class ListenerKind { Plain, Async, This };
  Expr *checkListener(Expr *arg, const Type *args, llvm::StringRef event, const Type *receiver,
                      ListenerKind &kind);
  // A call of the program of a method of node's EventEmitter on an emitter
  // of class `cls` (with `super`, `super.m()` in a class down its chain):
  // checked against the event map, as a call of the emitter's code. Null if
  // the method is none of those.
  Expr *checkEmitterCall(CallExpr *call, MemberExpr *member, ClassDecl *cls, bool super);
  // `call` of the method `name` of the emitter's code (generic with
  // `bound`), whose arguments `given` marks as checked.
  Expr *callLibraryMethod(CallExpr *call, MemberExpr *member, ClassDecl *cls, llvm::StringRef name,
                          llvm::ArrayRef<const Type *> bound, const std::vector<bool> *given,
                          bool super);
  void reportEmitterMethodValue(MemberExpr *member);
  // "; ...": why a name of node:events that the module does not export is
  // not there, for the error of its import.
  static std::string libraryExportHint(llvm::StringRef name);
  // addEventListener() and removeEventListener() of an EventTarget of the
  // program: the kind of event the listener takes, from its annotation; null
  // for the other methods.
  Expr *checkEventTargetCall(CallExpr *call, MemberExpr *member, ClassDecl *cls);
  // once(emitter, name), on(), getEventListeners() and listenerCount() of
  // node:events, also as `EventEmitter.once(...)`: `fn` is the function's
  // name there. Null for the other functions of the module.
  Expr *checkEventsFunction(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);
  // `call` of the function `name` of the emitter's code, generic with
  // `bound`, whose arguments `given` marks as checked.
  Expr *callLibraryFunction(CallExpr *call, llvm::StringRef name,
                            llvm::ArrayRef<const Type *> bound, const std::vector<bool> &given,
                            llvm::SMLoc loc);
  // Event maps found from emit() calls (see EventInference). The map of the
  // emitter at `site` ("new main.ts:3:13", "class main.ts:Server"): in a
  // round that looks for them an opaque type of its own, which the calls on
  // its emitters record into; in the last round the object type of the
  // events found.
  const Type *inferredEventMap(const std::string &site);
  // `new EventEmitter()` without a map: the one the place it goes gives, or
  // the one found from emits (sema_generic_class.cpp calls it).
  Expr *checkEmitterNew(NewExpr *expr, ClassDecl *generic, const Type *expected);
  // The type of the object `create` makes, before it is checked: an
  // instance of a class (of EventEmitter with the map found for its place);
  // null if that is not known from its form.
  const Type *createdType(const NewExpr *create);
  // The place of `loc` in the program, "main.ts:3:13".
  std::string placeOf(llvm::SMLoc loc) const;
  // What a round looking for maps does with a call on an emitter of the
  // opaque map `events` of `site`: records the arguments of an emit, gives
  // a listener of a known event its type.
  Expr *collectEmitterCall(CallExpr *call, MemberExpr *member, const std::string &site);
  EventInference::Portable portable(const Type *type, llvm::SmallPtrSetImpl<const Type *> &seen);
  const Type *fromPortable(const EventInference::Portable &p, llvm::SMLoc loc);
  EventInference *inference_ = nullptr;
  std::map<std::string, const Type *> siteMaps_;
  llvm::DenseMap<const Type *, std::string> openMaps_, inferredMaps_;
  llvm::StringMap<ClassDecl *> libClasses_;
  llvm::StringMap<FunctionDecl *> libFunctions_;
  llvm::StringMap<TypeAliasDecl *> libTypes_;
  std::vector<std::pair<const char *, const char *>> libraryRanges_;

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
  // A target `o[key]` becomes the field it names.
  const Type *checkAssignTarget(Expr *&target, bool compound);
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
  // A checked `process.exit()` statement, after which nothing runs.
  static bool isExit(const Stmt *stmt);

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
  bool namesIterator(const Expr *expr) const;
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

  // Modules (sema_modules.cpp). Each file of a program has a top-level scope
  // of its own, with its own declarations and what it imports. The scope of
  // the file being checked (module_) is scopes_[0] and typeScopes_[0]; those
  // of the others wait in their ModuleState.
  //
  // A top-level declaration of some file, which an import or an export names
  // in the end, through any re-exports.
  struct Binding {
    Module *module = nullptr;
    llvm::StringRef name;
    bool operator==(const Binding &other) const {
      return module == other.module && name == other.name;
    }
  };
  struct Export {
    Binding target;
    llvm::SMLoc loc;
    const ExportDecl *all = nullptr; // `export * from`, which gave it
  };
  struct Import {
    Binding target;
    llvm::SMLoc loc;
  };
  struct ModuleState {
    llvm::StringMap<ScopeEntry> values;
    llvm::StringMap<Stmt *> types;
    // The names the file declares at the top level, also those not checked
    // yet, and its `let` and `const`.
    llvm::StringMap<llvm::SMLoc> declared;
    llvm::StringSet<> topLevelNames;
    // Its imports, by the names they get in it; and those already reported
    // as wrong.
    llvm::StringMap<Import> imports;
    llvm::StringSet<> badImports;
    llvm::SmallPtrSet<const VarSymbol *, 4> importedVars;
    // What it imports from neverthrow: the names they get in it, and the
    // names of neverthrow they stand for.
    llvm::StringMap<llvm::StringRef> neverthrow;
    // What it imports from built-in modules, by the names they get in it.
    llvm::StringMap<NodeName> nodeNames;
    llvm::StringMap<Export> exports;
    bool exportsDone = false, exportsBusy = false;
    // Its top-level code: the next statement to check, and what holds
    // there when the check stopped early (see loadEarly).
    size_t next = 0;
    std::optional<Flow> flow;
    bool busy = false;   // being checked
    bool loaded = false; // all checked
  };
  ModuleState &state(const Module *module) { return moduleStates_[module->order]; }
  // Makes `module` the file being checked; only at the top level, as scopes_
  // holds just the file's scope there.
  void useModule(Module *module);
  // Declares the types and functions of every file, and binds what files
  // import. Returns the top-level functions.
  std::vector<FunctionDecl *> declareModules(SourceFile &program);
  const llvm::StringMap<Export> &exportsOf(Module *module);
  // What `from`, imported by `path`, exports as `name.name`; reports a name it
  // does not export.
  std::optional<Binding> findExport(Module *from, llvm::StringRef path, const ModuleName &name);
  // What `name` means in the top-level scope of `module`: its own
  // declaration, or what it imports by that name.
  std::optional<Binding> resolveLocal(Module *module, llvm::StringRef name);
  void resolveImports(Module *module);
  // Binds the imports of module_ whose declarations are there: types and
  // functions at once, the variables of a file once its code is checked.
  void bindImports();
  // The variables some function of the importing file uses through its
  // imports must be globals.
  void noteImportsInFunctions(SourceFile &program);
  // The top-level function of some file that `name` calls in the body of
  // `fn` (see findCycles).
  FunctionDecl *cycleCallee(FunctionDecl *fn, llvm::StringRef name);
  // Checks the top-level code of `module` from where it stopped: to the end,
  // or to the declaration of the global `until`.
  void checkTopLevel(Module *module, llvm::StringRef until = {});
  // A value by its name; if it is a global of a file that loads later, in
  // an import cycle, that file is checked up to its declaration first, out
  // of turn, when the code here does not run as the file loads: an arrow
  // that uses it, or a function whose return type is needed.
  ScopeEntry lookupValue(llvm::StringRef name);
  // The file that declares `name` if it is an import of module_ or a global
  // of module_ while another file loads, and its name there.
  Module *laterSource(llvm::StringRef name, llvm::StringRef &declared);
  bool loadEarly(Module *source, llvm::StringRef name);
  // Reports a name of a file that is checked later, in an import cycle: one
  // that is imported, or a global of a file whose function is checked early.
  // False if `name` is no such name.
  bool reportNotLoaded(const NameExpr *name);
  // `reader` reads `what`, declared as `declared` in `source`, which loads
  // after it.
  static std::string readBeforeLoad(const Module *reader, const std::string &what,
                                    const Module *source, llvm::StringRef declared);
  bool reportImportAssign(const NameExpr *name, const VarSymbol *var);
  // "main.ts -> a.ts -> b.ts -> (a.ts already started)": how `reader`
  // came to load before `source`.
  static std::string loadChain(const Module *reader, const Module *source);
  // Reads of variables before the code that sets them runs: top-level code
  // calls a function that uses a global declared later, in its file or in a
  // file that loads later.
  void checkLoadOrder(SourceFile &program);
  // The functions of imported files that the program does not use.
  void findUnused(SourceFile &program);

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
  // The statements of the block being checked, and the position in them of
  // the one being checked: where a literal it declares may go is in the
  // statements around it (see literalDestination and followingStatements).
  llvm::ArrayRef<Stmt *> block_;
  size_t blockIndex_ = 0;
  // Those statements, for literalDestination.
  std::vector<const Stmt *> following_;
  // Variables initialized by literals that go to several places whose types
  // disagree, with those places: for the error at one of them.
  llvm::DenseMap<const VarSymbol *, std::vector<std::pair<const Type *, llvm::SMLoc>>>
      destinations_;
  // Set by literalDestination when it finds such places.
  std::vector<std::pair<const Type *, llvm::SMLoc>> destinationConflict_;
  // The values of the string constants, the keys `o[key]` may use.
  llvm::DenseMap<const VarSymbol *, std::string> constKeys_;
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
  // Inferring the return type of currentFunction_: its return statements,
  // and for a generator its yields.
  std::vector<ReturnStmt *> *returns_ = nullptr;
  std::vector<YieldExpr *> *yields_ = nullptr;
  // Checking a copy of a function of a cycle (see inferCycle): a call within
  // the cycle may not have a type yet.
  bool trial_ = false;
  // Top-level code is still being checked; then a function inferred early,
  // because top-level code calls it, cannot see the globals declared later.
  bool topLevelDone_ = false;
  const FuncSymbol *early_ = nullptr;
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
  // Generics: the instances, by generic function and representations; those
  // waiting for their bodies to be checked; the one being checked.
  std::map<std::pair<const FuncSymbol *, std::vector<const Type *>>, Instance *> instances_;
  // Generic arrows that top-level constants name (see FunctionDecl::lifted).
  std::vector<FunctionDecl *> lifted_;
  std::deque<Instance> instanceStore_;
  std::vector<Instance *> pendingInstances_;
  Instance *checkingInstance_ = nullptr;
  // Generic functions whose own body has errors: no instance of them is
  // checked, which would only repeat those.
  llvm::SmallPtrSet<const FuncSymbol *, 4> brokenGenerics_;
  // Generic functions reported by returnsOwnClass.
  llvm::SmallPtrSet<const FuncSymbol *, 2> ownClassReturns_;
  // See exactTypeParam: by generic function, for each type parameter.
  llvm::DenseMap<const FuncSymbol *, std::vector<bool>> exactParams_;
  // Generic classes: the instances by generic class and type arguments (the
  // generic class for its own type parameters), the defaults of the type
  // parameters, the instances made before the generic classes had their
  // methods, which wait for them, and the instances being filled, innermost
  // last, for those that need ever bigger ones.
  std::map<std::pair<const ClassDecl *, std::vector<const Type *>>, ClassDecl *> classInstances_;
  llvm::DenseMap<const ClassDecl *, std::vector<const Type *>> classDefaults_;
  std::vector<ClassDecl *> unfilled_;
  std::vector<const ClassDecl *> filling_;
  llvm::SmallPtrSet<const ClassDecl *, 2> endless_; // reported as such
  // Argument lists (sema_generic.cpp, docs/generics.md section 9). A rest
  // parameter whose type is an argument list (`...args: [int, string]`, what
  // `A` stands for in a copy) becomes the parameters of the list; `args`
  // spread is them, one by one, and `args` used as a value a tuple made of
  // them when the function starts.
  void expandArgumentList(FunctionDecl *fn, FuncSymbol *sym);
  // `args` in the body of such a function, as it starts.
  void declareArgumentList(FunctionDecl *fn);
  // `args.length` and `args[1]` of a list that is not made a tuple: the
  // count, and the parameter; null for anything else.
  Expr *listPart(Expr *expr);
  // `...args` in the arguments of a call or the elements of an array
  // literal, of such a list: its parameters in its place.
  void expandListSpreads(std::vector<Expr *> &args, std::vector<bool> &spread);
  // In generic code, the arguments of a call for a rest parameter of a list
  // of types (`...args: A`, `[...A, ...B]`): spreads of those lists, in
  // order, and values for the other elements; reports anything else.
  bool checkListArguments(CallExpr *call, size_t fixed, const Type *list, llvm::SMLoc loc,
                          const std::vector<bool> *checked);
  // The type of `args[i]` and of the values of `for (const x of args)` in
  // generic code: some type, as a type parameter is.
  const Type *listElement(const Type *list);
  // Computations over types (sema_typeops.cpp, docs/generics.md section
  // 10). The utilities written as mapped types, made once.
  void declarePrelude();
  llvm::StringMap<TypeAliasDecl *> builtinAliases_;
  // `keyof type`: the names of its fields, as a union of string literal
  // types; null if it is no object type.
  const Type *keysOf(const Type *type);
  // The computation `op` (Type::typeOp) with these operands: a type if they
  // are known, or the computation again.
  const Type *evalTypeOp(const Type *op, llvm::ArrayRef<const Type *> operands);
  // `keyof T`, `T[K]`, `typeof x` and mapped types in a type annotation;
  // null for any other.
  const Type *resolveTypeOp(const TypeRef &ref);
  // Exclude, Extract, NonNullable, ReturnType and Parameters; null for any
  // other name.
  const Type *resolveUtilityType(const TypeRef &ref);
  // Whether a string literal given for type parameter `param` gives its
  // own type, as for `K extends keyof T` (`get(user, "name")`: K = "name").
  bool wantsLiteral(const Type *param) const;
  // `o[k]` in generic code, of an object of a type parameter or with a key
  // of one (`k: K` with `K extends keyof T`): of type `T[K]`. Null, with
  // nothing checked, for any other.
  Expr *checkGenericIndex(IndexExpr *index);
  // `T[K]` of known types: the type of the member K of T (an element of a
  // tuple or an array, a field of an object, by a union of names the union
  // of theirs); null if there is none.
  const Type *memberType(const Type *object, const Type *key);
  // Conditional types, `infer`, template literal types and the intrinsic
  // string types (sema_conditional.cpp, docs/generics.md section 10).
  //
  // `T extends U ? A : B` of known types gives the branch at once, and
  // resolves only that one, so that a type alias can refer to itself in a
  // branch. Over type parameters it waits, as a computation (Type::typeOp)
  // whose operands are the types the names it uses stand for; a copy that
  // knows them resolves it again (evalConditional).
  const Type *resolveConditional(const TypeRef &ref);
  const Type *deferConditional(const TypeRef &ref);
  const Type *evalConditional(llvm::ArrayRef<const Type *> operands);
  // The branches of a conditional type that waits, with its `infer` types
  // unknown; generic code may give a value of either where the type is
  // expected (fitsConditional), and each copy checks the one it chooses.
  std::vector<const Type *> conditionalBranches(const Type *conditional);
  bool fitsConditional(const Type *from, const Type *to, unsigned depth = 0);
  // What a literal written where `expected` is expected looks at: of a
  // conditional type that waits, the union of its branches, so that `return
  // "text"` takes the literal type a branch has.
  const Type *literalExpectation(const Type *expected, unsigned depth = 0);
  // Binds `name` to `type` in the innermost scope of types, as a type
  // parameter is bound.
  void bindType(llvm::StringRef name, const Type *type, llvm::SMLoc loc);
  // `` `on${K}` ``, and `A & B` (only `K & string` and `T & {}`).
  const Type *resolveTemplate(const TypeRef &ref);
  const Type *resolveIntersection(const TypeRef &ref);
  const Type *evalTemplate(llvm::ArrayRef<const Type *> operands);
  const Type *evalStringCase(TypeOp op, const Type *type);
  // A type argument of a type alias or a utility type, where `undefined` and
  // `void` alone are types too (`Exclude<T, undefined>`); where the result is
  // used says whether it may be such a type.
  const Type *resolveTypeArgument(const TypeRef &ref);
  // The types `infer` gives (bound) for its type parameters (params) while a
  // type is matched against the pattern of a conditional type.
  struct InferMatch {
    llvm::ArrayRef<const Type *> params;
    std::vector<const Type *> bound;
  };
  // `type extends pattern`, as TypeScript decides it for a conditional
  // type: a member of a union or a literal extends what its whole does, an
  // object one with at least the fields of the pattern (as a constraint
  // says), an array and a tuple by their elements, a function by its
  // parameters and its result; `infer` takes what stands in its place.
  bool extendsType(const Type *type, const Type *pattern, InferMatch *inference = nullptr);
  bool extendsTuple(const Type *type, const Type *pattern, InferMatch *inference);
  bool extendsFunction(const Type *type, const Type *pattern, InferMatch *inference);
  bool extendsObject(const Type *type, const Type *pattern, InferMatch *inference);
  bool matchTemplate(const std::u16string &text, llvm::ArrayRef<const Type *> parts,
                     InferMatch *inference);
  // `typeof f` of a generic function or an overloaded method, for
  // ReturnType and Parameters: as TypeScript does, the last signature, with
  // each type parameter its constraint (or unknown); null for other types.
  const Type *signatureForUtility(const TypeRef &ref);
  // What a conditional type that waits needs to be resolved again: its
  // text, the names it uses that stand for types (whose types are the
  // operands after `tag`), and what the other names it uses mean where it
  // is written.
  struct ConditionalInfo {
    const TypeRef *ref = nullptr;
    const Type *tag = nullptr;
    std::vector<llvm::StringRef> bound;
    std::vector<std::pair<llvm::StringRef, Stmt *>> decls;
  };
  std::map<const TypeRef *, ConditionalInfo> conditionals_;
  llvm::DenseMap<const Type *, const ConditionalInfo *> conditionalTags_;
  // Inside the `extends` clause of a conditional type, where `infer` may be.
  unsigned extendsClause_ = 0;
  // The template literal type being resolved, for an error of what it makes.
  llvm::SMLoc templateLoc_;
  // A conditional type that is the body of a type alias may end in another
  // instance of an alias (`Len<R, [...Acc, H]>`): a tail call, which
  // instantiateAlias makes in a loop rather than nested, so that it may go
  // 1000 deep as in TypeScript. tailAllowed_ says the conditional type being
  // resolved is such a body; tailCall_ is the call it ends in.
  struct TailCall {
    TypeAliasDecl *alias = nullptr;
    std::vector<const Type *> args;
  };
  bool tailAllowed_ = false;
  std::optional<TailCall> tailCall_;
  // Writes through a wider type (sema_widen.cpp, docs/objects.md section 2):
  // whether a conversion lets code write what the value's own type does
  // not (`Dog[]` as `Animal[]`, a narrowed or read-only field); the
  // conversions that do, which the check of the whole program follows to
  // the writes; and whether some class or interface narrows a field of one
  // up its chain, which calls of the methods there convert `this` for.
  static bool sameReference(const Type *from, const Type *to);
  bool widensWrites(const Type *from, const Type *to);
  void checkWidenings(SourceFile &file);
  void reportWidening(const Type *from, const Type *to, llvm::SMLoc at, llvm::StringRef role,
                      llvm::StringRef name, llvm::SMLoc write, const Type *value,
                      const Type *through, bool element, llvm::StringRef field,
                      const Type *narrow, const Type *owner, int depth);
  std::map<std::pair<const Type *, const Type *>, bool> widens_;
  llvm::DenseSet<const CastExpr *> widenings_;
  bool narrowedFields_ = false;
  // The parameters each list variable stands for (see expandArgumentList).
  llvm::DenseMap<const VarSymbol *, std::vector<llvm::StringRef>> argumentLists_;
  // The type of a rest parameter is being resolved: a tuple type there may
  // have a rest element, `[int, ...string[]]`, a list of parameters.
  bool listContext_ = false;
  // Where the types being substituted come from, for the instances of
  // generic classes they make (see instantiateClass); none while unset.
  llvm::SMLoc instanceLoc_;
  bool methodsDeclared_ = false; // every class of the program has its methods
  bool classesLinked_ = false;   // linkClasses ran
  // Generic classes whose own bodies have errors: no instance of them is
  // checked, which would only repeat those.
  llvm::SmallPtrSet<const ClassDecl *, 2> brokenClasses_;
  // The checks of the instances of generic classes, by class.
  llvm::DenseMap<const ClassDecl *, Instance *> classChecks_;
  // The functions `new` of a generic class infers its type arguments with:
  // one for each constructor it takes (or one for none).
  std::map<std::pair<const ClassDecl *, const FunctionDecl *>, FuncSymbol *> constructorFunctions_;
  // Where an instance's body calls an instance whose body failed: the error
  // is that one's.
  llvm::DenseMap<const char *, const Instance *> failedCalls_;
  // The conversions generic code made of values of type parameters (see
  // noteForced), by the position and kind of the expression.
  llvm::DenseMap<std::pair<const char *, unsigned>, const Type *> forced_;
  // The instances of generic aliases, and the alias and arguments that made
  // an object type (to substitute in it).
  struct AliasInstance {
    const Type *type = nullptr;
    Type *self = nullptr; // while it is resolved, for the fields that refer to it
    bool resolving = false;
    bool recursive = false;
  };
  std::map<std::pair<const TypeAliasDecl *, std::vector<const Type *>>, AliasInstance> aliasInstances_;
  llvm::DenseMap<const Type *, std::pair<TypeAliasDecl *, std::vector<const Type *>>> aliasOrigins_;
  llvm::DenseMap<const TypeAliasDecl *, std::pair<std::vector<const Type *>, std::vector<const Type *>>>
      aliasParams_; // the type parameters and their defaults
  unsigned aliasDepth_ = 0;
  llvm::SMLoc aliasUse_; // see instantiateAlias
  // The object types substitute is rebuilding: a recursive one that no alias
  // made stays as it is.
  llvm::SmallPtrSet<const Type *, 4> substituting_;
  mutable llvm::DenseMap<const Type *, const Type *> numberParams_;
  std::vector<BlockNames> blockNames_;
  const BlockNames *declaredLater(llvm::StringRef name) const {
    for (const BlockNames &block : llvm::reverse(blockNames_))
      if (llvm::is_contained(block.names, name))
        return &block;
    return nullptr;
  }
  // See useModule. loading_ is the file whose top-level code is being
  // checked; null after all of it is.
  std::vector<ModuleState> moduleStates_;
  Module *module_ = nullptr;
  Module *loading_ = nullptr;
  // The file of each top-level function, type alias, enum, class and
  // method.
  llvm::DenseMap<const Stmt *, Module *> declModules_;
  // Classes (sema_class.cpp): the class whose code is being checked, which
  // may use its private members; the constructor (or initializers) being
  // checked and its `this`; whether the closures it creates may use `this`
  // (every field was set where they are created); checking a static
  // initializer, where `this` would be the class.
  ClassDecl *currentClass_ = nullptr;
  FunctionDecl *initFn_ = nullptr;
  const VarSymbol *initThis_ = nullptr;
  bool closureThisReady_ = true;
  bool staticInit_ = false;
  // `this.x` is being checked, a use of `this` the rules allow: x.
  llvm::StringRef thisMember_;
  // The callback being checked as an argument of a built-in such as map:
  // what it is called with, which chooses the variant of a method.
  const Signature *callback_ = nullptr;
  // The static fields of the class being declared that are set by now.
  llvm::SmallPtrSet<const VarSymbol *, 4> staticsSet_;
  unsigned classCount_ = 0;
  std::vector<ClassDecl *> classes_;
  llvm::SmallPtrSet<const ClassDecl *, 2> completing_;
  // The `super(...)` statements at the top level of the bodies of
  // constructors: the only place where they are allowed.
  llvm::SmallPtrSet<const CallExpr *, 4> rootSupers_;
  // The calls of constructors reported by checkConstructorCalls.
  llvm::SmallPtrSet<const CallExpr *, 4> reportedCalls_;
  // See signatureVariants.
  std::map<std::pair<const Type *, std::string>, std::vector<FunctionDecl *>> signatureVariants_;
  // Abstract methods without a return type: the implementation whose type
  // they take; and overrides without one, whose type is that of the method
  // they override, once known (see inheritedReturnType).
  llvm::DenseMap<const FunctionDecl *, FunctionDecl *> returnSources_;
  llvm::DenseMap<const FunctionDecl *, FunctionDecl *> inheritsReturn_;
  // The functions whose return types are being inferred, innermost last,
  // and those whose inference waits for a file checked out of turn: a call
  // to one of these is a cycle the types cannot settle.
  std::vector<const FuncSymbol *> inferring_;
  llvm::SmallPtrSet<const FuncSymbol *, 4> suspended_;
  // The callbacks of the methods of typed arrays (see ByteCallback).
  llvm::DenseMap<const FunctionDecl *, ByteCallback> byteCallbacks_;
  // The class Error (see declareErrorClass).
  ClassDecl *errorClass_ = nullptr;
  // Error and its built-in kinds, as kErrorKinds lists them.
  std::vector<ClassDecl *> errorKinds_;
  // SystemError, which the functions of node:fs throw (docs/io.md), and the
  // classes Stats and Dirent of node:fs.
  ClassDecl *systemErrorClass_ = nullptr;
  ClassDecl *statsClass_ = nullptr;
  ClassDecl *direntClass_ = nullptr;
  // ExecSyncError, which execSync and execFileSync throw when the program
  // they ran failed (docs/process.md).
  ClassDecl *execSyncErrorClass_ = nullptr;
  // The class of the built-in kind `name` (Error, TypeError...), or null.
  ClassDecl *errorKindClass(llvm::StringRef name) const;

  // The built-in modules written in tz (sema_library.cpp, src/lib), which
  // prepareLibraries and inLibrary (sema_events.cpp) find. An intrinsic, `function __tz_name(...): T` without a body: its types
  // must be ones the runtime takes and gives.
  void checkExternal(FunctionDecl *fn);
  // Accessors of their classes (FunctionDecl::accessor): the getter or the
  // setter `name` of `cls` or a class up its chain, or null.
  static FunctionDecl *accessorOf(ClassDecl *cls, llvm::StringRef name,
                                  FunctionDecl::Accessor kind);
  // `o.x` that reads through a getter: a call of it; null if x is none.
  Expr *checkAccessorRead(MemberExpr *member, ClassDecl *cls, const Type *expected);
  // `o.x = v` of an accessor x: the type of v, and the assignment becomes a
  // call of the setter (checkSetterCall), which pendingSetter_ waits for;
  // null if x is no accessor.
  const Type *checkAccessorTarget(MemberExpr *member, ClassDecl *cls, bool compound);
  Expr *checkSetterCall(AssignExpr *assign);
  MemberExpr *pendingSetter_ = nullptr;
  // A call the program writes of an accessor, `o.x()`: reported.
  bool rejectAccessorCall(CallExpr *call, MemberExpr *member, FuncSymbol *sym);
  // The calls of accessors that reading and assigning made.
  llvm::SmallPtrSet<const CallExpr *, 8> accessorCalls_;
};

// A tuple type of generic code with lists of types spread into it, `[int,
// ...A]`.
bool spreadsLists(const Type *type);

// Whether `type` mentions one of the type parameters `params`.
bool typeMentions(const Type *type, llvm::ArrayRef<const Type *> params);

// The instance of the generic class `generic` (or `generic` itself) that the
// class `type` is, or that a class up its chain is; null if none.
const ClassDecl *genericInstanceOf(const Type *type, const ClassDecl *generic);

// "2 arguments", "1-3 arguments", "1 or more arguments".
std::string argumentCount(unsigned min, unsigned max);

// A language tag as BCP 47 writes it: "ru", "en-US", "zh-Hant-TW",
// "de-DE-u-co-phonebk". What ICU would read wrongly ("en_US") is not one.
bool isLanguageTag(const std::string &tag);

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

// A variant of a method in messages, `add(v: Vec, k?: double)`, and a list
// of them, "f(), f(x: int) and f(s: string)".
std::string variantText(const FunctionDecl *fn);
std::string variantList(llvm::ArrayRef<FunctionDecl *> variants);

// The statements a literal declared by `stmts[i]` may go to: the ones after
// it, and the functions declared before it, which may run later.
std::vector<const Stmt *> followingStatements(llvm::ArrayRef<Stmt *> stmts, size_t i);

// The text `o.name` that reads a field of `object`, for hints;
// `o["content-type"]` for a name that is not an identifier.
std::string fieldText(const Expr *object, llvm::StringRef field);

// The names a statement or an expression mentions. With `inFunction` unset,
// only those inside the functions it contains.
void collectNames(const Stmt *stmt, llvm::StringSet<> &names, bool inFunction = true);
void collectNames(const Expr *expr, llvm::StringSet<> &names, bool inFunction = true);
// The names of the methods `body` calls, `x.name(...)`.
void collectMemberCalls(const Stmt *body, llvm::StringSet<> &names);

// Calls `visit` on every expression inside `stmt`, those in the functions it
// contains included, unless `intoFunctions` is unset.
void visitExprs(const Stmt *stmt, llvm::function_ref<void(const Expr *)> visit,
                bool intoFunctions = true);
void visitExprs(const Expr *expr, llvm::function_ref<void(const Expr *)> visit,
                bool intoFunctions = true);

} // namespace tz
