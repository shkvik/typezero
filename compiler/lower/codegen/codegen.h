#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/check/parallel/parallel.h"
#include "compiler/program/types/types.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"

#include <deque>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <tuple>

namespace tz {

// Whether `expr` is a variable that a method can neither change nor release
// while it runs, so the object in it can be lent to the method.
bool staysDuringCall(const Expr *expr);
// What some code may run and throw (see codegen_throw.cpp).
struct ThrowSummary;
// Whether reading a key of the Json `object` may throw: unless `?.` checked
// it, it may be null or undefined.
bool jsonReadMayThrow(const Expr *object);
// Whether JSON.stringify of a value of `type` may throw: the value may hold
// itself, as a Json or the object of a class may.
bool stringifyMayThrow(const Type *type);
// Whether the locale and the options of toLocaleString or of
// Intl.DateTimeFormat (`args`, without the kind of toLocaleString) may be
// rejected when the program runs (see codegen_date.cpp).
bool dateOptionsMayThrow(llvm::ArrayRef<Expr *> args);
// Whether a built-in of binary data may throw (see codegen_bytes.cpp).
bool bytesMayThrow(const CallExpr *call);
// Whether a call of a built-in of Intl beyond dates may throw: what it is
// given may be rejected when the program runs (see codegen_intl.cpp).
bool intlCallMayThrow(const CallExpr *call);

// Lowers a checked AST to LLVM IR.
//
// Every local variable gets a stack slot (alloca) in the function's entry
// block and is accessed with loads and stores; LLVM's mem2reg pass turns them
// into registers. Top-level code becomes `main`.
//
// Representation of values:
//   int, long, double, boolean   i32, i64, double, i1
//   string                       { ptr obj, i64 meta } (runtime/core/string/string.c)
//   T[]                          ptr to an array header (runtime/core/array/array.c)
//   T | undefined                { T value, i1 has }; the value is zero when
//                                undefined, so it can always be released
//   function                     { ptr info, ptr env }: info is a constant
//                                { ptr code, string name }, env the closure's
//                                environment or null (see codegen_closure.cpp)
//   object                       ptr to { i64 rc, ptr layout, fields... }
//                                (see codegen_object.cpp); an object's
//                                `T | undefined` is that ptr, null when
//                                undefined. An instance of a class is one
//                                too (see codegen_class.cpp).
//   decimal                      { i64 coef, i64 meta } (runtime/library/decimal/decimal/decimal.c,
//                                codegen_decimal.cpp)
//   Result<T, E>                 { T value, E error, i1 ok } in some order
//                                (see codegen_result.cpp); the half not in
//                                use is zero, so both can always be released
//
// Strings, arrays, functions, objects and long decimals are reference
// counted ("managed", as are optionals of them). The ownership rules are
// simple for now and LLVM cleans up part of the overhead:
// - an expression of a managed type produces an owned (+1) value, which its
//   consumer either stores or releases;
// - a variable owns its value: assignment releases the old one, and leaving a
//   scope releases the variables declared in it;
// - an array owns its elements;
// - a call passes ownership of its arguments to the callee, which releases
//   its parameters when it returns;
// - an operand may be borrowed, without retain and release, when it is a
//   plain variable, or a field read through fields from one (`a.b.c`), and
//   nothing evaluated while it is in use can assign to anything.
class CodeGen {
public:
  CodeGen(llvm::LLVMContext &ctx, llvm::StringRef moduleName, const llvm::DataLayout &layout);

  std::unique_ptr<llvm::Module> generate(SourceFile &file);
  // Where the files of the program are, for the places in the stacks of
  // errors (file, line and column).
  void setSourceMgr(const llvm::SourceMgr *sourceMgr) { sourceMgr_ = sourceMgr; }
  // What of the program's async code may run in parallel (docs/async.md):
  // with a piece that may, the program runs on the threads of the parallel
  // runtime, tzrt_mt (see TZ_PARALLEL in runtime/core/tzrt/tzrt.h).
  void setParallelPlan(const ParallelPlan *plan);
  bool parallel() const { return parallel_; }
  // Before a place where async code touches what keeps node's order, the
  // piece it is in waits for its turn on the thread that runs it ahead.
  void orderAt(llvm::SMLoc loc);

private:
  struct JumpTarget;
  struct TailGroup;
  llvm::Type *lower(const Type *type);

  // Module structure
  void declareRuntime();
  void defineRefCountHelpers();
  void declareFunction(FunctionDecl *fn);
  void declareGlobals(SourceFile &file);
  void emitFunction(FunctionDecl *fn);
  // An intrinsic of a built-in module written in tz (FunctionDecl::external,
  // codegen_library.cpp): a call of the runtime's function of its name.
  void emitExternal(FunctionDecl *fn);
  // The keys of the getters of the classes of `chain`, the most derived
  // last, for the descriptor of a class of the library (TZ_FIELD_GETTER).
  void appendGetterKeys(llvm::ArrayRef<const ClassDecl *> chain,
                        std::vector<llvm::Constant *> &entries);
  void emitMain(SourceFile &file);
  void finishFunction();
  // Returns where control reaches the end of the body.
  void closeBody();

  // Blocks
  llvm::BasicBlock *newBlock(const llvm::Twine &name);
  // Continues emission in `block`, which must not be reachable by falling through.
  void switchTo(llvm::BasicBlock *block);
  // Falls through into `block` (if the current block is still open) and continues there.
  void emitBlock(llvm::BasicBlock *block);
  bool blockIsOpen();
  void branchTo(llvm::BasicBlock *block);
  // After a jump: code that follows lands in a block nothing branches to.
  void startDeadBlock();

  // Scopes: managed variables to release when control leaves them. A slot
  // without a type holds an object of a closure (a box or an environment).
  // `unwind` is where an error leaves the scope: it releases them too and
  // goes on outward (see codegen_throw.cpp); made when first needed.
  struct Scope {
    std::vector<std::pair<llvm::Value *, const Type *>> slots;
    // Slots of iterators that are closed (see iterCloseFunction) when the
    // scope ends, before the slots are released.
    std::vector<llvm::Value *> iterators;
    // Slots of dictionaries being iterated, whose count of iterations goes
    // down when the scope ends (see beginDictIteration).
    std::vector<llvm::Value *> dicts;
    // Slots of the async iterators of `for await`, closed when the scope
    // ends while they hold one, which the code awaits (codegen_async_iter.cpp).
    std::vector<llvm::Value *> asyncIterators;
    llvm::BasicBlock *unwind = nullptr;
  };
  void pushScope();
  void popScope();
  // Releases the variables of scopes_[depth..] without leaving them, before a
  // jump out of them (break, continue, return).
  void emitCleanups(size_t depth);
  void emitScopeCleanup(const Scope &scope);
  // A stack slot for a local variable or parameter; managed slots start empty
  // and are registered for release at the end of the current scope.
  llvm::Value *createLocal(const VarSymbol *var, llvm::StringRef name);
  llvm::Value *createManagedSlot(const Type *type, const llvm::Twine &name);
  // A slot for a pointer to an object of a closure, released with the scope.
  llvm::Value *createObjectSlot(const llvm::Twine &name);

  // Statements
  void emitStmt(Stmt *stmt);
  // The statements of a block, creating the functions it declares before the
  // first one that mentions them.
  void emitStatements(BlockStmt *block);
  void emitVarDecl(VarDeclStmt *decl);
  void emitIf(IfStmt *stmt);
  void emitWhile(WhileStmt *stmt);
  void emitDoWhile(DoWhileStmt *stmt);
  void emitFor(ForStmt *stmt);
  void emitForOf(ForOfStmt *stmt);
  void emitSwitch(SwitchStmt *stmt);
  void emitLabeled(LabeledStmt *stmt);
  void emitJump(Stmt *stmt);
  void emitReturn(ReturnStmt *stmt);
  // `return value`, or a plain `return` for null.
  void emitReturnValue(Expr *value);
  // Evaluates an expression for its side effects only.
  void emitDiscarded(Expr *expr);
  // Stores an owned value into a slot that may hold an older value.
  void storeReplacing(llvm::Value *address, llvm::Value *value, const Type *type);

  // Expressions
  llvm::Value *emitExpr(Expr *expr);
  llvm::Value *emitExprKind(Expr *expr);
  llvm::Value *emitUnary(UnaryExpr *unary);
  llvm::Value *emitNonNull(UnaryExpr *unary);
  llvm::Value *emitOptionalChain(OptionalChainExpr *chain);
  llvm::Value *emitChainLink(CastExpr *link);
  llvm::Value *emitBinary(BinaryExpr *bin);
  llvm::Value *emitLogical(BinaryExpr *bin);
  // What `a && b` or `a || b` gives when a decides: a's value `lhs` (owned),
  // as the type of the whole.
  llvm::Value *decidedValue(BinaryExpr *bin, llvm::Value *lhs);
  // Whether a borrowed value is truthy, as a condition tests it (see Sema's
  // checkTruthy): a string when it is not empty, `T | undefined` when it
  // holds a value that is.
  llvm::Value *emitTruth(llvm::Value *value, const Type *type);
  // The truth of an expression converted to boolean.
  llvm::Value *emitTruthOf(Expr *expr);
  llvm::Value *emitEquality(BinaryExpr *bin);
  llvm::Value *emitNullish(BinaryExpr *bin);
  llvm::Value *emitArithmetic(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs, const Type *type);
  llvm::Value *emitComparison(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs, const Type *type);
  // Whether two values of `type` are equal (==); both borrowed.
  llvm::Value *emitValuesEqual(llvm::Value *a, llvm::Value *b, const Type *type);
  llvm::Value *emitIntDivRem(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs);
  llvm::Value *emitAssign(AssignExpr *assign, bool needResult);
  llvm::Value *emitNullishAssign(AssignExpr *assign, bool needResult);
  llvm::Value *emitElementAssign(AssignExpr *assign, bool needResult);
  llvm::Value *emitLengthAssign(AssignExpr *assign, bool needResult);
  llvm::Value *emitUpdate(UpdateExpr *update);
  llvm::Value *emitTypeOf(Expr *operand);
  llvm::Value *emitEnumName(IndexExpr *index);
  llvm::Value *emitCall(CallExpr *call);
  void emitConsoleLog(CallExpr *call);
  // process, Date.now() and performance.now() (codegen_process.cpp).
  llvm::Value *emitProcessBuiltin(CallExpr *call);
  // The built-in modules: node:path, node:os, node:fs and tz:io
  // (codegen_node.cpp).
  llvm::Value *emitNodeBuiltin(CallExpr *call);
  // node:crypto (codegen_crypto.cpp): a call of runtime/library/crypto/, and
  // generateKeyPairSync.
  llvm::Value *emitCryptoCall(CallExpr *call);
  llvm::Value *emitCryptoKeyPair(CallExpr *call);
  // `ptr (ptr parts)`: makes an object of `type` from the struct of its
  // fields the runtime fills (TzMaker in runtime/core/tzrt/tzrt.h), taking them
  // over.
  llvm::Function *objectMaker(const Type *type);
  // The struct of the parts of `type` that its maker reads.
  llvm::StructType *makerParts(const Type *type);
  // The maker of SystemError, which the functions of node:fs throw.
  llvm::Function *systemErrorMaker();
  // node:fs/promises and node:readline; node:timers/promises
  // (codegen_async_io.cpp).
  llvm::Value *emitFsPromise(CallExpr *call);
  llvm::Value *emitReadlineBuiltin(CallExpr *call);
  llvm::Value *emitAsyncIoBuiltin(CallExpr *call);
  std::map<const Type *, llvm::Function *> objectMakers_;
  // spawnSync, execFileSync and execSync of node:child_process
  // (codegen_child.cpp).
  llvm::Value *emitChildBuiltin(CallExpr *call);
  // The maker of what they give, the result of spawnSync or an
  // ExecSyncError, from the TzSpawnParts of
  // runtime/library/child_process/child/child.c.
  llvm::Function *spawnMaker(const Type *type);
  llvm::StructType *spawnPartsType();
  // An owned value of `to` from what stream `stream` printed, in the parts
  // at `parts`: text, a Buffer, or undefined if it was no pipe. With
  // `retain`, the parts keep their reference.
  llvm::Value *spawnStream(llvm::Value *parts, unsigned stream, const Type *to, bool retain);
  std::map<const Type *, llvm::Function *> spawnMakers_;
  // A function of the runtime for those and console.error, declared when
  // first called.
  llvm::FunctionCallee processFunction(llvm::StringRef name, llvm::Type *ret,
                                       llvm::ArrayRef<llvm::Type *> params);
  llvm::Value *emitConditional(ConditionalExpr *cond);
  // Converts an owned value; the result is owned.
  llvm::Value *emitConversion(llvm::Value *value, const Type *from, const Type *to);

  // Reference counting of any managed value.
  void retainValue(llvm::Value *value, const Type *type);
  void releaseValue(llvm::Value *value, const Type *type);
  // `T | undefined`: whether it holds a value, and the value (zero when it
  // does not).
  llvm::Value *makeOptional(llvm::Value *value, llvm::Value *has, const Type *type);
  llvm::Value *undefinedValue(const Type *optionalType);
  llvm::Value *optionalHas(llvm::Value *value, const Type *optionalType);
  llvm::Value *optionalValue(llvm::Value *value, const Type *optionalType);

  // Operands that are used and then dropped.
  struct Operand {
    llvm::Value *value;
    bool owned; // must be released after use
    const Type *type;
    // While held (see holdOperand): the slot it waits in.
    llvm::Value *heldSlot = nullptr;
  };
  Operand emitOperand(Expr *expr, bool canBorrow);
  // A variable, or a field read through fields from one, without counting
  // its reference; null for anything else.
  llvm::Value *emitBorrowed(Expr *expr);
  void dropOperand(const Operand &operand);
  // Writes a value to a fresh stack slot, for runtime functions that take
  // values of any type by address.
  llvm::Value *spill(llvm::Value *value, const llvm::Twine &name);

  // Strings
  void retainString(llvm::Value *str);
  void releaseString(llvm::Value *str);
  llvm::Value *stringObj(llvm::Value *str);
  llvm::Value *stringMeta(llvm::Value *str);
  llvm::Value *stringLength(llvm::Value *str); // as i64
  // Calls a runtime function that returns a string through its first argument.
  llvm::Value *callReturningString(llvm::FunctionCallee fn, llvm::ArrayRef<llvm::Value *> args);
  llvm::Value *concatOperands(llvm::ArrayRef<Operand> operands);
  Operand emitConcatPart(Expr *expr, bool canBorrow);
  llvm::Value *emitConcat(BinaryExpr *bin);
  llvm::Value *emitTemplate(TemplateExpr *tmpl);
  llvm::Value *emitStringComparison(BinaryExpr *bin);
  llvm::Value *emitStringMethod(CallExpr *call);
  // Math, Number, parseInt, String.fromCharCode... (codegen_builtins.cpp)
  llvm::Value *emitMathCall(CallExpr *call);
  llvm::Value *emitNumberBuiltin(CallExpr *call);
  // Number(s) of a string; `str` is borrowed.
  llvm::Value *emitStringToNumber(llvm::Value *str);
  // A double that must be a whole number fitting in `to` (an int or a long),
  // or the program stops.
  llvm::Value *emitCheckedInteger(llvm::Value *value, const Type *to);
  // Passes `values` to the runtime as an array on the stack.
  llvm::Value *spillArray(llvm::ArrayRef<llvm::Value *> values, llvm::Type *element);
  llvm::Value *emitStringIndex(IndexExpr *index);
  llvm::Value *emitMember(MemberExpr *member);
  llvm::Constant *stringLiteral(const std::u16string &text);
  // Whether evaluating `expr` cannot assign to any variable.
  static bool cannotAssign(const Expr *expr);

  // Arrays (codegen_array.cpp)
  llvm::Constant *typeDescriptor(const Type *type);
  llvm::Value *arrayField(llvm::Value *array, unsigned field);
  llvm::Value *arrayLength(llvm::Value *array); // as i64
  llvm::Value *arrayData(llvm::Value *array);
  llvm::Value *elementAddress(llvm::Value *data, const Type *element, llvm::Value *index);
  // Appends owned values, growing the array if needed.
  void emitPush(llvm::Value *array, llvm::ArrayRef<llvm::Value *> values, const Type *element);
  // A value read from a range-checked position: `T | undefined` unless the
  // position was proven in range.
  llvm::Value *emitCheckedRead(llvm::Value *inRange, const Type *resultType,
                               llvm::function_ref<llvm::Value *()> read);
  llvm::Value *emitArrayLiteral(ArrayLitExpr *lit);
  // `step(acc, element)` over the elements of a borrowed array of numbers or
  // booleans, starting from `init`; the last result.
  llvm::Value *emitArrayFold(llvm::Value *array, const Type *element, llvm::Value *init,
                             llvm::function_ref<llvm::Value *(llvm::Value *, llvm::Value *)> step);
  llvm::Value *emitNewArray(NewExpr *expr);
  llvm::Value *emitArrayIndex(IndexExpr *index);
  llvm::Value *emitArrayMethod(CallExpr *call);
  void emitArrayForOf(ForOfStmt *stmt, llvm::Value *iterableSlot, llvm::BasicBlock *condBlock,
                      llvm::BasicBlock *bodyBlock, llvm::BasicBlock *endBlock);
  // A value converted to a string by the runtime, for arrays and optionals.
  llvm::Value *emitValueToString(llvm::Value *value, const Type *type);

  // Closures (codegen_closure.cpp)
  struct EnvLayout {
    llvm::StructType *type = nullptr; // { i64 rc, ptr drop, fields... }
    llvm::Function *drop = nullptr;
    std::vector<const VarSymbol *> fields;
    const void *key = nullptr; // in envLayouts_
    // An environment that may form cycles is a member of a group, with a
    // layout of its own (see memberDescriptor).
    bool member = false;
    llvm::Constant *descriptor = nullptr;
  };
  // The type of a variable, also of a function's while Sema left it unset.
  static const Type *typeOf(const VarSymbol *var);
  static const Type *functionTypeOf(const FuncSymbol *fn);
  // (ptr env, params...) -> ret: how a function value of `fnType` is called.
  llvm::FunctionType *closureFnType(const Type *fnType);
  llvm::Function *closureCode(FunctionDecl *fn);
  // The info of a function value: { ptr code, string name }.
  llvm::Constant *functionInfo(FunctionDecl *fn);
  const EnvLayout &envLayout(const FunctionDecl *fn);
  llvm::StructType *boxType(const Type *type);
  // A new box for a variable of `type`, holding its zero value; a member of
  // a group when `member` is, or the variable's box is cyclic.
  llvm::Value *newBox(const Type *type, bool member = false);
  llvm::Value *boxPointer(const VarSymbol *var);
  // A new environment with the current values of `layout`'s variables. On
  // the stack, it borrows them and is not counted.
  llvm::Value *createEnv(const EnvLayout &layout, bool onStack, bool member = true);
  llvm::Value *makeFunctionValue(llvm::Value *info, llvm::Value *env);
  // The value of a variable, owned or borrowed: a function's own value when
  // the variable names the function being emitted or one of its group.
  llvm::Value *emitVarValue(const VarSymbol *var, bool borrow);
  llvm::Value *emitFunctionExpr(FunctionExpr *fe);
  llvm::Value *emitFunctionAdapter(CastExpr *cast);
  // A function value of another result (CastExpr::adaptsResult,
  // codegen_library.cpp): a box with it is the environment of code that
  // calls it and converts what it gives.
  llvm::Value *emitResultAdapter(CastExpr *cast);
  std::map<std::pair<const Type *, const Type *>, llvm::Constant *> resultAdapters_;
  void emitGroupCreation(NestedGroup *group);
  // The environment for a direct call to a closure.
  llvm::Value *closureEnv(CallExpr *call);
  llvm::Value *emitValueCall(CallExpr *call);
  // Calls a borrowed function value with owned arguments; the parameters
  // after them get undefined.
  llvm::Value *callFunctionValue(llvm::Value *fn, const Type *fnType, llvm::ArrayRef<llvm::Value *> args);
  void emitClosure(FunctionDecl *fn);
  void emitPendingClosures();
  // Starts emitting the body of `fn` (null for main) into `function`.
  void beginFunction(llvm::Function *function, FunctionDecl *fn);
  void emitParamsAndBody(FunctionDecl *fn, llvm::ArrayRef<llvm::Value *> args);
  // The parameters alone, as locals of the current scope.
  void emitParams(FunctionDecl *fn, llvm::ArrayRef<llvm::Value *> args);
  // The addresses of the variables a closure captured, in its environment.
  void bindCaptures(const FunctionDecl *fn);

  // Objects (codegen_object.cpp). The fields of a layout lie in memory by
  // alignment, largest first, then by name: so no padding is needed between
  // them, and an object type and its read-only twin have the same layout.
  struct ObjectLayout {
    llvm::StructType *type = nullptr; // { i64 rc, ptr layout, fields... }
    // The struct element of each field, in the order of Type::fields().
    std::vector<unsigned> elements;
    // An object literal with methods holds the variables they capture after
    // its fields (see ClassDecl::literal), from this element on.
    unsigned captures = 0;
    // Releases what the fields hold and frees the object.
    llvm::Function *drop = nullptr;
    // Releases a reference, dropping the object with the last one.
    llvm::Function *release = nullptr;
  };
  const ObjectLayout &objectLayout(const Type *type);
  // The constant that describes objects of `type` created with the fields in
  // `keys` order (positions in Type::fields()) to the runtime: the drop
  // function, and the names, offsets and types of the fields in that order,
  // for printing.
  llvm::Constant *layoutDescriptor(const Type *type, llvm::ArrayRef<unsigned> keys);
  // The same with the TZ_LAYOUT_* flags of the descriptor given.
  llvm::Constant *layoutDescriptor(const Type *type, llvm::ArrayRef<unsigned> keys,
                                   uint32_t flags);
  void emitObjectDrop(const Type *type, ObjectLayout &layout);
  // The address of field `name` of `object`, which code knows as `type`;
  // with `exact`, the object is of that type and of no other.
  llvm::Value *fieldAddress(llvm::Value *object, const Type *type, llvm::StringRef name,
                            bool exact = false);
  // A new object of `type` with the descriptor for `keys`; its fields are
  // not set yet.
  llvm::Value *allocObject(const Type *type, llvm::ArrayRef<unsigned> keys);
  // The same as a member of a group, or not (see makesMember).
  llvm::Value *allocObject(const Type *type, llvm::ArrayRef<unsigned> keys, bool member);
  llvm::Value *emitObjectLiteral(ObjectLitExpr *lit);
  llvm::Value *emitSpreadLiteral(ObjectLitExpr *lit);
  // `["Ann", 30]` as a tuple.
  llvm::Value *emitTupleLiteral(ArrayLitExpr *lit);
  // Appends the elements of the borrowed tuple `tuple` to `array`.
  void emitTupleSpread(llvm::Value *array, llvm::Value *tuple, const Type *tupleType,
                       const Type *element);
  llvm::Value *emitObjectCall(CallExpr *call);
  llvm::Value *emitFieldRead(MemberExpr *member);
  llvm::Value *emitFieldAssign(AssignExpr *assign, bool needResult);
  // `o[k]` of a key of literals (IndexExpr::keyed): the address of the field
  // the atom `key` names.
  llvm::Value *keyedFieldAddress(llvm::Value *object, const Type *type, llvm::Value *key,
                                 const Type *keyType);
  // The position of field `name` in Type::fields().
  static unsigned fieldIndex(const Type *type, llvm::StringRef name);

  // Classes (codegen_class.cpp). A method is a function (ptr this,
  // params...) -> ret, the shape of the code of a closure: the object is its
  // environment, so a method bound to an object is the function value
  // { info, object }. A static method is a plain function, a static field a
  // global.
  void declareMethods(ClassDecl *cls);
  void emitMethods(ClassDecl *cls);
  llvm::Function *methodCode(const FunctionDecl *fn);
  // The descriptor of the instances: the layout's, with the class's name
  // and number, and the keys as node prints them.
  llvm::Constant *classDescriptor(const ClassDecl *cls);
  // A new instance whose fields are empty (zero).
  llvm::Value *allocInstance(const ClassDecl *cls);
  llvm::Value *allocInstance(const ClassDecl *cls, bool member);
  llvm::Value *emitNew(NewExpr *expr);
  llvm::Value *emitMethodCall(CallExpr *call);
  llvm::Value *emitBoundMethod(MemberExpr *member);
  llvm::Value *emitInstanceOf(BinaryExpr *bin);
  llvm::Value *emitMethodLiteral(ObjectLitExpr *lit);
  void emitStaticFields(ClassDecl *cls);
  // What an environment or a box holds in place of an object's layout: a
  // constant { ptr drop }, which starts as a layout does, so that a function
  // value's environment may be either (see tz.obj.release).
  llvm::Constant *dropDescriptor(llvm::Function *drop);
  // `new C(args)` and the part of `super(args)` that runs the class
  // extended: the initializers of the fields and the constructor `ctor` (of
  // `cls` or of a class up its chain), with `args` owned.
  void emitConstruct(llvm::Value *object, const ClassDecl *cls, const FuncSymbol *ctor,
                     llvm::ArrayRef<llvm::Value *> args);
  llvm::Value *emitSuperCall(CallExpr *call);
  // Whether constructing an object of `cls` with `ctor` may throw.
  bool constructionThrows(const ClassDecl *cls, const FuncSymbol *ctor) const;

  // Inheritance and interfaces (codegen_inherit.cpp). The descriptor of an
  // instance of a class is { TzLayout, ptr itables, [slots x { code, info }] }:
  // the method table follows the layout, at a fixed offset. Code that knows
  // an object by an interface reads a field at the offset it has in the
  // interface's own layout when every type that implements the interface
  // has it there; otherwise, and for a method, through the table the
  // object's descriptor gives for the interface (itables, by the
  // interface's number, which no two interfaces of one type share).
  struct InterfaceMember {
    int entry = -1;        // in the table; -1 for a field read directly
    bool callable = false; // a method: { code, info } rather than an offset
  };
  struct InterfaceInfo {
    // The object types whose objects code may see as the interface, itself
    // included.
    std::vector<const Type *> implementers;
    std::vector<InterfaceMember> members; // by the fields of the interface
    // The methods with several signatures: by method, by signature.
    std::vector<std::vector<InterfaceMember>> overloads;
    llvm::StructType *table = nullptr;
    unsigned color = 0;
    bool dispatch = false; // some member goes through the table
  };
  void prepareDispatch(SourceFile &file);
  // How code reaches member `name` of objects of `type`, if through a table;
  // `signature` picks one of a method with several signatures.
  const InterfaceMember *dispatchedMember(const Type *type, llvm::StringRef name,
                                          int signature = -1);
  // The address of entry `entry` of the table of `iface` in `object`'s
  // descriptor.
  llvm::Value *tableEntry(llvm::Value *object, const Type *iface, unsigned entry);
  // The itables of the objects of `type`: null if it implements no interface
  // that needs them.
  llvm::Constant *itablesOf(const Type *type);
  llvm::Constant *itable(const Type *type, const Type *iface);
  // The code a method of an interface runs for an object of a type that
  // holds a function in the field, which it calls; and its info.
  std::pair<llvm::Function *, llvm::Constant *> fieldStub(const Type *type, const Field &member);
  // The method of class `cls` (or up its chain) that implements a member of
  // type `fnType`.
  FunctionDecl *implementingMethod(const ClassDecl *cls, llvm::StringRef name, const Type *fnType);
  // `o.m(args)` through an interface.
  llvm::Value *emitInterfaceCall(CallExpr *call);
  // Where the method table of a class's descriptor starts, and the address of
  // the code (or with `info`, the info) at `slot` of the object's table.
  static constexpr uint64_t kVtableOffset = 48;
  llvm::Value *vtableEntry(llvm::Value *object, int slot, bool info);
  // Whether objects of other types may be seen as `type`: their drop
  // functions differ, so the descriptor's is called.
  bool hasSubtypes(const Type *type) const { return withSubtypes_.contains(type); }
  // The layout whose fields an object type puts first: of the class it
  // extends, or of an interface it implements; null for none.
  const Type *layoutPrefix(const Type *type) const;

  // Results (codegen_result.cpp). A Result is a struct of its two halves and
  // a flag; a half of type void (`ok()`) or never is an empty struct.
  struct ResultLayout {
    llvm::StructType *type = nullptr;
    unsigned value = 0, error = 0, ok = 0; // the elements
  };
  ResultLayout resultLayout(const Type *type);
  llvm::Type *lowerHalf(const Type *half);
  llvm::Constant *resultDescriptor(const Type *type);
  llvm::Value *resultIsOk(llvm::Value *result, const Type *type);
  // The value (or with `error`, the error) of a Result, as it lies there.
  llvm::Value *resultHalf(llvm::Value *result, const Type *type, bool error);
  // A Result of `type` from its halves; null for zero.
  llvm::Value *makeResult(const Type *type, llvm::Value *ok, llvm::Value *value,
                          llvm::Value *error);
  // An owned half of type `from` as a half of type `to`; null for zero when
  // it cannot hold anything.
  llvm::Value *convertHalf(llvm::Value *half, const Type *from, const Type *to);
  // Calls the callback `fn` with the owned half `half`, if it takes one.
  llvm::Value *callWithHalf(llvm::Value *fn, const Type *fnType, llvm::Value *half,
                            const Type *halfType);
  llvm::Value *emitResultCall(CallExpr *call);
  llvm::Value *emitResultMethod(CallExpr *call);
  llvm::Value *emitCombine(CallExpr *call);
  llvm::Value *emitCombineTuple(CallExpr *call);
  llvm::Value *emitResultMember(MemberExpr *member);
  // `yield* r`: an Err returns from the generator of safeTry.
  llvm::Value *emitSafeTryYield(YieldExpr *yield);

  // Tail calls (codegen_tail.cpp). A cycle of functions that call each
  // other (or one that calls itself) in tail position, emitted as one LLVM
  // function where those calls jump.
  struct TailGroup {
    std::vector<FunctionDecl *> members;
    bool emitted = false;
    // While emitting: the slots of each member's arguments, and where its
    // body starts.
    std::vector<std::vector<llvm::Value *>> slots;
    std::vector<llvm::BasicBlock *> starts;
  };
  void findTailGroups(llvm::ArrayRef<FunctionDecl *> functions);
  // The same for a closure: among its group, or itself.
  void findTailGroups(FunctionDecl *closure);
  void emitTailGroup(TailGroup &group);
  // A call `value` returns as it is to a function of the group being
  // emitted; null if none.
  CallExpr *tailCall(Expr *value) const;
  bool hasTailCall(Expr *value) const;
  void emitTailReturn(Expr *value);
  void emitTailJump(CallExpr *call);
  // Methods of arrays that take a callback (codegen_array.cpp).
  llvm::Value *emitCallbackMethod(CallExpr *call);
  llvm::Function *sortComparator(CallExpr *call);

  // Decimals (codegen_decimal.cpp). The runtime takes a decimal as its two
  // words and returns one through a pointer passed first, as for strings.
  llvm::FunctionCallee decimalFunction(llvm::StringRef name, llvm::Type *ret,
                                       llvm::ArrayRef<llvm::Type *> params);
  llvm::Value *decimalCoef(llvm::Value *value);
  llvm::Value *decimalMeta(llvm::Value *value);
  llvm::Value *callReturningDecimal(llvm::StringRef name, llvm::ArrayRef<llvm::Value *> args);
  // A literal (or its negation) of type decimal: a constant.
  llvm::Constant *emitDecimalLiteral(const Expr *lit);
  llvm::Value *decimalFromInteger(llvm::Value *value);
  // `a op b` of borrowed decimals (for `**`, b is a long); owned result.
  llvm::Value *emitDecimalArithmetic(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs);
  // Arithmetic and comparisons of decimals.
  llvm::Value *emitDecimalBinary(BinaryExpr *bin);
  // `x op= v` of a decimal whose old value `old` is borrowed from its slot.
  llvm::Value *emitDecimalCompound(BinaryOp op, llvm::Value *old, Expr *value);
  llvm::Value *emitDecimalUpdate(UpdateExpr *update, llvm::Value *address);
  llvm::Value *emitDecimalCall(CallExpr *call);
  llvm::Value *emitDecimalFold(CallExpr *call);
  // The sign of a borrowed decimal as an i32: -1, 0 or 1.
  llvm::Value *decimalSign(llvm::Value *value);
  // An i32 rounding mode from a string argument, or half-up without one.
  llvm::Value *emitRoundingMode(CallExpr *call, size_t index);

  // Unions and literal types (codegen_union.cpp): how their values lie is
  // described there. A string literal type or a union of them is an atom.
  static bool isAtomType(const Type *type);
  static bool isObjectUnion(const Type *type);
  // The type whose values a value of `type` is: the base of a number or
  // boolean literal type (or a union of them, `0 | 1`); `type` otherwise.
  static const Type *storedType(const Type *type);
  // Whether `T | undefined` is T itself, zero for undefined: a reference,
  // an atom, a tagged union.
  static bool zeroIsUndefined(const Type *inner);
  llvm::Type *lowerUnion(const Type *type);
  // The constant table of the strings of the atoms, and the text of one.
  llvm::GlobalVariable *atomTable();
  llvm::Value *atomString(llvm::Value *atom);
  llvm::Constant *atomDescriptor();
  llvm::Constant *unionDescriptor(const Type *type);
  // Tagged unions: the tag; a value from the part of a slot; the part.
  llvm::Value *unionTag(llvm::Value *value);
  llvm::Value *makeTagged(const Type *unionType, UnionSlot slot, llvm::Value *part);
  llvm::Value *taggedPart(llvm::Value *value, const Type *unionType, UnionSlot slot);
  // Branches on the tag of `value`: `each` emits the code of a slot, the
  // result for it; a tag of none of `slots` gives `otherwise`. Returns the
  // result of the one that ran (null without `resultType`).
  llvm::Value *forEachSlot(llvm::Value *value, llvm::ArrayRef<UnionSlot> slots,
                           llvm::function_ref<llvm::Value *(UnionSlot)> each,
                           llvm::Type *resultType, llvm::Value *otherwise);
  void countTagged(llvm::Value *value, const Type *type, bool retain);
  llvm::Value *emitUnionConversion(llvm::Value *value, const Type *from, const Type *to);
  // Whether `to` holds only members of the union `from` (or of its
  // optional): a conversion that takes a part of the value, or all of it.
  static bool narrowsUnion(const Type *from, const Type *to);
  llvm::Value *emitUnionEqual(llvm::Value *a, llvm::Value *b, const Type *type);
  llvm::Value *emitUnionTypeOf(llvm::Value *value, const Type *type);
  llvm::Value *emitUnionTruth(llvm::Value *value, const Type *type);
  // Objects of a union are told apart by the class number in their
  // descriptor; an object type that is no class has a number of its own,
  // from kPlainIdBase on, above those of every class.
  static constexpr uint32_t kPlainIdBase = 0x40000000;
  // Those of the objects of binary data, by their kind (TZ_BYTES_CLASS in
  // runtime/core/tzrt/tzrt.h).
  static constexpr uint32_t kBytesIdBase = 0x3FFFFF00;
  uint32_t plainTypeId(const Type *type);
  llvm::Value *objectClassId(llvm::Value *object);
  // Whether the class number `id` is that of an object of one of `types`.
  llvm::Value *emitIsOneOf(llvm::Value *id, llvm::ArrayRef<const Type *> types);
  llvm::Value *emitUnionFieldRead(MemberExpr *member);
  // The address of field `name` of an object of the union `type`, a field of
  // one type in every member.
  llvm::Value *unionFieldAddress(llvm::Value *object, const Type *type, llvm::StringRef name);
  // `"radius" in s` of a union.
  llvm::Value *emitObjectIn(BinaryExpr *bin);
  // `s.area()` with the method of each member (see CallExpr::unionMethods).
  llvm::Value *emitUnionMethodCall(CallExpr *call);
  llvm::GlobalVariable *atomTable_ = nullptr;
  llvm::Constant *atomDescriptor_ = nullptr;
  llvm::DenseMap<const Type *, uint32_t> plainIds_;
  llvm::DenseMap<const Type *, llvm::Function *> unionRetains_, unionReleases_;
  std::vector<const Type *> subtypes_; // see SourceFile::subtypes

  // Json (codegen_json.cpp): a pointer to a node of the runtime
  // (runtime/library/json/json/json.c), counted as objects are; `Json |
  // undefined` is that pointer, null for undefined.
  llvm::FunctionCallee jsonFunction(llvm::StringRef name, llvm::Type *ret,
                                    llvm::ArrayRef<llvm::Type *> params);
  // Whether a borrowed `Json | undefined` holds something other than null.
  llvm::Value *jsonPresent(llvm::Value *value);
  llvm::Value *emitJsonNull();
  llvm::Value *emitJsonMember(MemberExpr *member);
  llvm::Value *emitJsonIndex(IndexExpr *index);
  llvm::Value *emitJsonAssign(AssignExpr *assign, bool needResult);
  // An owned value converted to or from a Json (see emitConversion).
  llvm::Value *emitJsonConversion(llvm::Value *value, const Type *from, const Type *to);
  llvm::Value *emitJsonEquality(BinaryExpr *bin);
  llvm::Value *emitJsonNullish(BinaryExpr *bin);
  llvm::Value *emitJsonChainLink(CastExpr *link);
  llvm::Value *emitJsonTypeOf(Expr *operand);
  llvm::Value *emitJsonCall(CallExpr *call);
  llvm::Value *emitJsonObjectLiteral(ObjectLitExpr *lit);
  llvm::Value *emitJsonObjectCall(CallExpr *call);
  // JSON.parse<T> and fromJson<T>: a value of `type` built from the borrowed
  // Json `json` (null for undefined) that `path` says where it is, for the
  // error. Objects and arrays are built by a function of their own.
  llvm::Value *emitFromJson(const Type *type, llvm::Value *json, llvm::Value *path);
  llvm::Function *fromJsonFunction(const Type *type);
  llvm::StructType *jsonPathTy_ = nullptr; // { ptr parent, string key, i64 index }
  llvm::DenseMap<const Type *, llvm::Function *> fromJsonFunctions_;
  // The values of embedJson, by the path of the file.
  std::map<std::u16string, llvm::GlobalVariable *> embedCaches_;
  // Generators and iterators (codegen_iter.cpp). An iterator is an object of
  // the runtime, { i64 rc, ptr layout }, followed by the frame of an LLVM
  // coroutine that gives its values: a generator function, or one that
  // CodeGen makes for the iterator of an array or a helper such as map.
  //
  // Where values are consumed right where the iterator is made (`for...of`,
  // a spread, toArray...), nothing of that is made: the chain of helpers
  // becomes one loop, and a generator called there is emitted inline, with
  // the consumer at each `yield` (see prepare).
  //
  // What gets the values of an iteration: `each` gets each value, owned,
  // and returns what a `yield` that gave it gets back (owned, of type
  // `sent`; null if `sent` is null); `stop` ends the whole iteration, and
  // leaves the builder in a block that is closed.
  struct Consumer {
    std::function<llvm::Value *(llvm::Value *)> each;
    std::function<void()> stop;
    const Type *sent = nullptr;
  };
  // An iteration whose operands are evaluated already: `run` emits it.
  struct Producer {
    std::function<void(const Consumer &)> run;
  };
  // What the code of a function sees: a generator emitted inline runs its
  // body in its own, and the consumer at each `yield` in the code's.
  struct InlineGenerator;
  struct EmitContext {
    FunctionDecl *fn;
    llvm::Value *env;
    const Type *returnType;
    TailGroup *tailGroup;
    std::vector<JumpTarget> targets;
    InlineGenerator *inlined;
  };
  struct InlineGenerator {
    FunctionDecl *decl;
    Consumer consumer;
    EmitContext consumerContext;
    llvm::BasicBlock *exit; // where the generator finishes
    llvm::Value *returnSlot; // what it returns goes here, if anywhere
    size_t scopeDepth;       // the scopes its `return` leaves
  };
  // The coroutine being emitted: a generator, or the iterator of an array or
  // a helper.
  struct Coroutine {
    llvm::Value *promise; // { ptr out, ptr in, ptr ret, i32 running, i32 close }
    llvm::Value *id, *handle, *object;
    llvm::BasicBlock *finalBlock, *freeBlock, *returnBlock;
    const Type *sent; // what next(x) sends, or null
    // A generator with a `try`: closing it runs the `finally` blocks around
    // the `yield` it waits at (see emitCoroutineYield). What it yields.
    bool closes = false;
    const Type *yielded = nullptr;
    // The bytes of the object before the frame.
    uint64_t header = 16;
  };
  EmitContext saveContext();
  void restoreContext(const EmitContext &context);
  // Evaluates the operands of an iteration of `source`, in the order
  // JavaScript does, into slots of the current scope.
  Producer prepare(Expr *source, llvm::Value *returnSlot = nullptr);
  // The iteration of a value held in `slot`: an array, a string or an
  // iterator object, whose generator's return value goes to `returnSlot`.
  Producer valueProducer(llvm::Value *slot, const Type *type, llvm::Value *returnSlot = nullptr);
  Producer arrayProducer(llvm::Value *slot, const Type *arrayType, Builtin mode);
  // `s.matchAll(re)` (codegen_regex.cpp): the matches of the regex held in
  // `regexSlot` over the string in `textSlot`, as a copy of the regex whose
  // lastIndex is in `lastSlot` finds them; `shape` builds each.
  Producer matchAllProducer(llvm::Value *textSlot, llvm::Value *regexSlot, llvm::Value *lastSlot,
                            llvm::Value *shape);
  // The stage `stage` of a chain (map, filter...) before `down`: it gets
  // values of `element` and gives values of `result`. `operand` is the slot
  // of its callback, of `callbackType`, or of its count.
  Consumer stageConsumer(Builtin stage, const Type *callbackType, const Type *element,
                         const Type *result, llvm::Value *operand, const Consumer &down);
  bool isInlinable(const FunctionDecl *decl) const;
  // The body of `decl`, whose parameters are bound, with `consumer` at each
  // `yield`; what it returns goes to `returnSlot`, if anywhere.
  void emitInlineGenerator(FunctionDecl *decl, const Consumer &consumer, llvm::Value *returnSlot);
  void emitIteratorForOf(ForOfStmt *stmt);
  void emitIteratorSpread(llvm::Value *array, Expr *source, const Type *element);
  llvm::Value *emitIteratorBuiltin(CallExpr *call);
  llvm::Value *emitIteratorSink(CallExpr *call);
  llvm::Value *emitNext(CallExpr *call);
  // An iterator object for a helper or the iterator of an array.
  llvm::Value *emitIteratorObject(CallExpr *call);
  llvm::Value *emitYield(YieldExpr *yield);
  // Gives `value` to whoever iterates the generator being emitted; the
  // result is what comes back, as `resultType`.
  llvm::Value *emitYieldValue(llvm::Value *value, const Type *resultType);
  // Resumes the iterator `it`: true if it gave a value, which is in `out`.
  llvm::Value *emitResume(llvm::Value *it, llvm::Value *out, llvm::Value *in, llvm::Value *ret);
  // Calls a callback held in `slot` with owned arguments of `types`, as many
  // as it takes; the others are released.
  llvm::Value *callCallback(llvm::Value *slot, const Type *fnType, std::vector<llvm::Value *> args,
                            llvm::ArrayRef<const Type *> types);
  // Coroutines.
  llvm::Constant *iteratorLayout(unsigned flags);
  // `promise`, if given, is the type of a promise that holds more than
  // promiseTy_, which it starts with.
  void beginCoroutine(unsigned layoutFlags, const Type *sent, llvm::StructType *promise = nullptr);
  // A suspension that is not the last; the builder goes on where the
  // coroutine resumes.
  void emitSuspend();
  llvm::Value *emitCoroutineYield(llvm::Value *value, const Type *resultType);
  void emitCoroutineReturn(llvm::Value *value, const Type *type);
  void finishCoroutine();
  void emitGenerator(FunctionDecl *fn, llvm::Function *function);
  void emitPendingIterators();
  llvm::Function *iteratorFunction(Builtin builtin, const Type *source, const Type *operand,
                                   const Type *result);
  llvm::Function *coroIntrinsic(llvm::Intrinsic::ID id, llvm::ArrayRef<llvm::Type *> types = {});

  // Regular expressions (codegen_regex.cpp). A literal's program is a
  // constant; a literal without g and y is a constant object too (see
  // TzRegex in runtime/core/tzrt/tzrt.h).
  llvm::FunctionCallee regexFunction(llvm::StringRef name, llvm::Type *ret,
                                     llvm::ArrayRef<llvm::Type *> params);
  llvm::Constant *regexProgram(const RegexLitExpr *lit);
  // The machine code of a literal's program, a function of the type
  // TzReMatch (codegen_regex_native.cpp).
  llvm::Constant *regexMatcher(const RegexLitExpr *lit);
  llvm::Value *emitRegexLiteral(RegexLitExpr *lit);
  // How the runtime builds matches of `matchType` (TzMatchShape) for
  // `regex`, whose names tell which captures the named groups are; null for
  // a regex not known.
  llvm::Constant *matchShape(const Type *matchType, const RegexLitExpr *regex);
  llvm::Value *emitRegexCall(CallExpr *call);
  // replace() and replaceAll() with a function.
  llvm::Value *emitReplaceWith(CallExpr *call);
  std::map<std::vector<uint32_t>, llvm::Constant *> regexPrograms_;
  // Dictionaries (codegen_dict.cpp): a pointer to a TzMap of the runtime
  // (runtime/core/map/map.c), counted and released through its layout as an
  // object is. An entry is { i32 hash, i32 live, key, value }, laid out here.
  struct DictLayout {
    llvm::StructType *entry = nullptr;
    uint64_t keyOffset = 0, valueOffset = 0, size = 0;
    llvm::Constant *descriptor = nullptr; // TzMapLayout
    // Of a type that may form cycles: the same for maps that are no members
    // (see makesMember).
    llvm::Constant *plainDescriptor = nullptr;
  };
  const DictLayout &dictLayout(const Type *type);
  llvm::FunctionCallee dictFunction(llvm::StringRef name, llvm::Type *ret,
                                    llvm::ArrayRef<llvm::Type *> params);
  llvm::Value *dictField(llvm::Value *map, unsigned field);
  // The entry of a borrowed key in a borrowed map, or null; with `add`, the
  // entry made for it if there was none (the map keeps a copy of the key),
  // and `added` is set then, if given.
  llvm::Value *emitDictFind(llvm::Value *map, llvm::Value *key, const Type *keyType);
  llvm::Value *emitDictAdd(llvm::Value *map, llvm::Value *key, const Type *keyType,
                           llvm::Value **added = nullptr);
  llvm::Value *entryKey(llvm::Value *entry, const Type *type);
  llvm::Value *entryValue(llvm::Value *entry, const Type *type);
  // After the value of a Record's entry changed: process.env tells the
  // environment.
  void emitEntryStored(llvm::Value *map, llvm::Value *entry);
  // While a dictionary held in `slot` is iterated its entries stay where
  // they are; the scope ends the iteration.
  void beginDictIteration(llvm::Value *slot);
  // The iteration of the dictionary in `slot`: keys, values or entries as
  // the iterator builtin `mode` gives them; `position` is the slot of the
  // position, if it must be one that the runtime sees (see
  // emitPendingIterators).
  Producer dictProducer(llvm::Value *slot, const Type *type, Builtin mode,
                        llvm::Value *position = nullptr);
  // Calls `each` with each live entry of the dictionary in `slot`.
  void emitDictLoop(llvm::Value *slot, const Type *type, llvm::Value *position,
                    llvm::function_ref<void(llvm::Value *)> each);
  llvm::Value *emitDictCall(CallExpr *call);
  llvm::Value *emitDictNew(CallExpr *call);
  llvm::Value *emitGroupBy(CallExpr *call);
  llvm::Value *emitDictForEach(CallExpr *call);
  llvm::Value *emitRecordLiteral(ObjectLitExpr *lit);
  llvm::Value *emitDictObjectCall(CallExpr *call);
  std::map<const Type *, DictLayout> dictLayouts_; // stable while more are added
  llvm::StructType *mapTy_ = nullptr;     // TzMap
  llvm::StructType *mapDescTy_ = nullptr; // TzMapLayout
  std::map<std::vector<uint32_t>, llvm::Constant *> regexMatchers_;
  std::map<std::pair<const Type *, std::vector<uint32_t>>, llvm::Constant *> matchShapes_;
  // Exceptions (codegen_throw.cpp, docs/errors.md). The error in flight is
  // the runtime's tz_thrown: a function that throws stores its Error there
  // and returns, releasing its variables on the way out, and the caller of a
  // function that may throw checks tz_thrown after the call. Which functions
  // may throw is found over the whole program (analyzeThrows): the others,
  // and their callers, pay nothing.
  void analyzeThrows(SourceFile &file);
  bool mayThrow(const FunctionDecl *fn) const { return throwing_.contains(fn); }
  // A call of the method `fn`; through the method table, any method that
  // overrides it down the chain may run.
  bool callMayThrow(const FunctionDecl *fn, bool virtualCall) const;
  // A call through a value of the function type `fnType` may throw.
  bool typeThrows(const Type *fnType) const { return throwingTypes_.contains(fnType); }
  // Evaluating `expr` may throw.
  bool exprThrows(const Expr *expr);
  bool summaryThrows(const ThrowSummary &summary) const;
  // A stack slot for the error in flight and the stack limit, the runtime's.
  llvm::GlobalVariable *thrownGlobal();
  // The address of this thread's tz_thrown, made by builder_ or by `b`.
  llvm::Value *thrownSlot();
  llvm::Value *thrownSlot(llvm::IRBuilderBase &b);
  // A constant TzFrame for `loc` in the function being emitted, or named
  // `function`.
  llvm::Constant *frameDescriptor(llvm::SMLoc loc, const std::string *function = nullptr);
  std::string frameFunctionName() const;
  // Where an error at the current point goes: the release of the innermost
  // scope, then outward.
  llvm::BasicBlock *unwindTarget();
  llvm::BasicBlock *unwindBlock(size_t scope);
  llvm::BasicBlock *outerUnwind(size_t scope);
  // Fills the unwind block of scope `i`, which is ending.
  void finishScopeUnwind(size_t i);
  // Fills the block where an error leaves the function being emitted.
  void finishFunctionUnwind();
  // After a call that may throw (at loc_): an error in flight goes up.
  void emitThrowCheck();
  // After a call that may throw, if `throws`.
  void emitThrowCheckIf(bool throws) {
    if (throws)
      emitThrowCheck();
  }
  // emitThrowCheck, with `release` on the way up for what only the code at
  // hand knows of.
  void emitThrowCheckWith(llvm::function_ref<void()> release);
  // Throws `error`, an owned Error, from `loc`; the block ends.
  void emitThrow(llvm::Value *error, llvm::SMLoc loc);
  // A built-in fails at loc_: throws a new Error of `kind` (an index of
  // kErrorKinds) with `message`. The block ends.
  void emitThrowNew(int kind, llvm::StringRef message);
  // An error the runtime made (a function of it that may throw was called):
  // goes up from loc_. The block ends.
  void emitRuntimeThrow();
  void emitThrowStmt(ThrowStmt *stmt);
  void emitTry(TryStmt *stmt);
  // `return` and the jumps that may leave a `try` with a `finally`: they run
  // it first. emitReturnOf returns `value` (owned, of the function's return
  // type; null for none).
  void emitReturnOf(llvm::Value *value);
  // The call whose result `value`, a number, is, with only arithmetic of
  // numbers after it: nothing that may throw or release; null if none.
  static const CallExpr *returnedCall(const Expr *value);
  void emitJumpTo(size_t target, bool isBreak);
  // The innermost `try` whose `finally` a jump to targets_[target] (or a
  // `return`, for ~0) passes, or null.
  struct TryContext;
  TryContext *finallyBetween(size_t target);
  // At the start of a function that may recurse: RangeError if the stack is
  // too low.
  void emitStackCheck(llvm::SMLoc loc);
  // The class Error (ClassDecl::builtin): its constructor, its descriptor
  // for each kind, the function the runtime makes errors with, and what main
  // tells the runtime about it.
  void emitErrorConstructor(FunctionDecl *fn);
  const ClassDecl *errorKindClass(int kind) const;
  llvm::Constant *errorDescriptor(int kind);
  llvm::Function *errorMaker();
  void emitErrorSupport();
  llvm::Value *emitNewError(NewExpr *expr);
  llvm::Value *emitErrorBuiltin(CallExpr *call);
  // Result.fromThrowable(f, toError?), _unsafeUnwrap() and _unsafeUnwrapErr().
  llvm::Value *emitFromThrowable(CallExpr *call);
  llvm::Value *emitUnsafeUnwrap(CallExpr *call);
  // Owned values held while code that may throw runs: a value that is
  // managed waits in a managed slot of the current scope, which the error
  // path releases with the variables (`held` false when not needed).
  struct Held {
    llvm::Value *value;
    llvm::Value *slot;
    const Type *type;
  };
  Held hold(llvm::Value *value, const Type *type, bool needed);
  llvm::Value *take(Held &held);
  void holdOperand(Operand &operand, bool needed);
  void takeOperand(Operand &operand);
  // Evaluates `exprs` in order, owned; each managed one is held while one
  // after it that may throw runs.
  std::vector<llvm::Value *> emitHeldValues(llvm::ArrayRef<Expr *> exprs);

  // Async (codegen_async.cpp, docs/async.md). A Promise is an object of the
  // runtime (TzPromise in runtime/core/tzrt/tzrt.h) with its value after the
  // header; an async function is a coroutine whose frame follows its promise
  // and the reactor it awaits with.
  struct AsyncState {
    llvm::Value *object = nullptr;  // the promise
    llvm::Value *reactor = nullptr; // what it awaits with
    const Type *value = nullptr;    // what the promise holds; void for nothing
    // A callback whose promise nobody takes: the ramp drops it.
    bool dropsPromise = false;
    // An async generator (codegen_async_iter.cpp): `object` is the generator.
    bool generator = false;
  };
  llvm::FunctionCallee asyncFunction(llvm::StringRef name, llvm::Type *ret,
                                     llvm::ArrayRef<llvm::Type *> params);
  // The bytes the value of a Promise<value> takes, a multiple of 8.
  uint64_t promiseValueSize(const Type *value);
  llvm::Value *promiseValueAddress(llvm::Value *promise);
  llvm::Value *promiseField(llvm::Value *promise, unsigned field);
  // The layout of the promises of a value of type `value`: the drop function
  // of the runtime, and the value as its one field.
  llvm::Constant *promiseLayout(const Type *value);
  llvm::Value *newPromise(const Type *value);
  // Settles `promise`: with the owned `value` (null if it holds nothing),
  // with an owned error, or as the owned promise `other` does.
  void fulfillPromise(llvm::Value *promise, llvm::Value *value, const Type *type);
  void rejectPromise(llvm::Value *promise, llvm::Value *error);
  void followPromise(llvm::Value *promise, llvm::Value *other);
  void emitAsyncFunction(FunctionDecl *fn, llvm::Function *function);
  llvm::Function *emitAsyncMain(SourceFile &file);
  void beginAsync();
  void finishAsync();
  llvm::Function *asyncResumeFunction();
  // `return` in an async function, and where it ends: its promise is
  // fulfilled with `value` (owned, null for none).
  void emitAsyncReturnStmt(ReturnStmt *stmt);
  void emitAsyncReturn(llvm::Value *value);
  // An error leaves the body: it rejects the promise.
  void emitAsyncUnwind();
  llvm::Constant *asyncFrameDescriptor(llvm::SMLoc loc);
  void suspendAsync(llvm::Value *promise, llvm::SMLoc loc);
  // Waits for the owned `promise` of `type`: its value, owned, or its error
  // thrown from `loc`.
  llvm::Value *awaitPromise(llvm::Value *promise, const Type *type, llvm::SMLoc loc);
  // Whether a value of `type` may be a Promise: a union with one.
  static bool holdsPromise(const Type *type);
  llvm::Value *awaitUnion(llvm::Value *value, const Type *type, const Type *result,
                          llvm::SMLoc loc);
  llvm::Value *emitAwait(AwaitExpr *await);
  llvm::Value *emitAsyncBuiltin(CallExpr *call);
  // Async generators and `for await` (codegen_async_iter.cpp).
  llvm::Constant *asyncGeneratorLayout();
  void emitAsyncGenerator(FunctionDecl *fn, llvm::Function *function);
  // The body is done, with an owned error or null.
  void finishAsyncGenerator(llvm::Value *error);
  // `await` of an owned value of `type`: what it gives, owned.
  llvm::Value *awaitValueOf(llvm::Value *value, const Type *type, llvm::SMLoc loc);
  void agenYield(llvm::Value *value, const Type *type, llvm::SMLoc loc);
  llvm::Value *emitAsyncYield(YieldExpr *yield);
  void emitAsyncYieldAll(YieldExpr *yield, const Type *yielded);
  llvm::Value *emitAsyncStep(llvm::Value *slot, const Type *element, llvm::SMLoc loc,
                             llvm::BasicBlock *endBlock);
  // return() of the async iterator in `slot`, if any, awaited.
  void closeAsyncIterator(llvm::Value *slot);
  void emitForAwait(ForOfStmt *stmt);
  // `return x` of an async generator awaits x, which `for await` drops.
  void emitAsyncGeneratorReturn(ReturnStmt *stmt);
  llvm::Constant *agenLayout_ = nullptr;
  // While the code that destroys a coroutine where it waits is emitted:
  // nothing is awaited there.
  bool cleanupNoAwait_ = false;
  llvm::Value *emitPromiseNew(CallExpr *call);
  llvm::Value *emitPromiseStatic(CallExpr *call);
  llvm::Value *emitPromiseCombine(CallExpr *call);
  // void (ptr dst, ptr src): the value of type `from` at src, made a `to`
  // (a union of it), into dst; race and any of promises of different values.
  llvm::Function *combineConverter(const Type *from, const Type *to);
  llvm::Value *emitPromiseReaction(CallExpr *call);
  llvm::Constant *resolverInfo(const Type *value);
  llvm::Constant *rejecterInfo();
  llvm::StructType *reactionType();
  llvm::Function *reactionDrop();
  llvm::Function *reactionHandler(Builtin kind, const Type *value, const Type *result,
                                  const Type *onFulfilled, const Type *onRejected);
  // A new reaction to `source` with `handler`, which settles `derived`
  // (borrowed) and holds the owned callbacks (null for none). With `later`
  // it waits for `source` from the next turn of the queue on.
  void attachReaction(llvm::Value *source, llvm::Value *derived, llvm::Function *handler,
                      llvm::Value *onFulfilled, llvm::Value *onRejected, bool later = false);
  // ResultAsync: a Promise of a Result, and the reactions its functions and
  // methods make.
  llvm::Function *resultAsyncHandler(Builtin kind, const Type *value, const Type *result,
                                     const Type *first, const Type *second);
  llvm::Value *emitResultAsync(CallExpr *call);
  llvm::Value *emitResultAsyncOf(CallExpr *call, llvm::ArrayRef<llvm::Value *> values);
  llvm::Value *emitResultAsyncThrowable(CallExpr *call);
  AsyncState *async_ = nullptr;
  bool usesAsync_ = false;
  // The program is an ES module for Node: its top-level code runs inside a
  // microtask.
  bool esModule_ = false;
  llvm::StructType *promiseObjTy_ = nullptr;
  llvm::StructType *reactionTy_ = nullptr;
  llvm::Function *asyncResumeFn_ = nullptr;
  llvm::Function *reactionDropFn_ = nullptr;
  llvm::Constant *rejecterInfo_ = nullptr;
  llvm::DenseMap<const Type *, llvm::Constant *> promiseLayouts_, resolverInfos_;
  std::map<std::tuple<Builtin, const Type *, const Type *, const Type *, const Type *>,
           llvm::Function *>
      reactionHandlers_;
  std::map<std::pair<const Type *, const Type *>, llvm::Function *> combineConverters_;

  // unknown, object and classes as values (codegen_unknown.cpp). An unknown
  // is a pointer, null for undefined: an object of the language as it is,
  // any other value in a box of the runtime (TzBox,
  // runtime/core/unknown/unknown.c). A class as a value is a constant object
  // (TzClass) made when first used.
  llvm::FunctionCallee unknownFunction(llvm::StringRef name, llvm::Type *ret,
                                       llvm::ArrayRef<llvm::Type *> params);
  // An owned value of `from` as one of `to`, one of them an unknown.
  llvm::Value *emitUnknownConversion(llvm::Value *value, const Type *from, const Type *to);
  // An owned value of `type` (not an object of the language) in a new box.
  llvm::Value *boxValue(llvm::Value *value, const Type *type);
  llvm::Value *emitUnknownTypeOf(Expr *operand);
  llvm::Value *emitUnknownIsArray(Expr *arg);
  llvm::Constant *classObject(const ClassDecl *cls);
  // The address of a static field in the object of its class (when the
  // class is used as a value: ClassDecl::usedAsValue).
  llvm::Constant *staticFieldAddress(const ClassDecl *cls, const VarSymbol *var);
  llvm::DenseMap<const ClassDecl *, llvm::StructType *> classObjectTypes_;
  // `t.join(sep)` of a tuple.
  llvm::Value *emitTupleJoin(CallExpr *call);

  // Decorators (codegen_decorate.cpp). A class declaration sets the static
  // fields, gives the methods that decorators may replace (FunctionDecl::impl)
  // their adapters, and runs the decorators.
  void emitClassDeclaration(ClassDecl *cls);
  // Such a method: its body as `.original`, and the stub that calls what its
  // global holds.
  void emitDecoratedMethod(FunctionDecl *fn);
  llvm::Function *originalCode(FunctionDecl *fn);
  // The info of the function value that runs the body: the first `value` of
  // the descriptor.
  llvm::Constant *implAdapterInfo(FunctionDecl *fn);
  llvm::DenseMap<const FunctionDecl *, llvm::Function *> originals_;
  llvm::DenseMap<const FunctionDecl *, llvm::Constant *> adapterInfos_;
  // Metadata: Reflect.defineMetadata and the others; the constant whose
  // address is an object key; and at the end, if the program uses
  // metadata, the objects of its classes for the runtime, given to it right
  // after `init` in main.
  llvm::Value *emitMetaCall(CallExpr *call);
  // The intrinsics of the built-in modules written in tz, `__tz_*`
  // (codegen_events.cpp).
  llvm::Value *emitLibBuiltin(CallExpr *call);
  // The runtime's side of the AbortSignal `signal` of `type` (an AbortSignal
  // of library/events/web.tz, or undefined): its field #native, borrowed or
  // retained, or null.
  llvm::Value *nativeSignal(llvm::Value *signal, const Type *type, bool retain = false);
  static bool isAbortSignal(const Type *type);
  // The intrinsics of node:stream (codegen_stream.cpp): LibChunk,
  // LibIsChunk and LibOverrides. emitChunkAs: the value of `to` the unknown
  // `u` (borrowed) holds, owned, going to `fail` if it holds none.
  llvm::Value *emitStreamBuiltin(CallExpr *call);
  llvm::Value *emitChunkAs(llvm::Value *u, const Type *to, llvm::BasicBlock *fail);
  llvm::Constant *metaKeyConstant(const void *key);
  void finishMetadata(SourceFile &file, llvm::Instruction *init);
  llvm::DenseMap<const void *, llvm::Constant *> metaKeys_;
  bool usesMetadata_ = false;
  llvm::Instruction *mainInit_ = nullptr;
  llvm::DenseMap<const ClassDecl *, llvm::Constant *> classObjects_;

  // Classes as values and reflection (codegen_reflect.cpp, docs/decorators.md
  // sections 5 and 6). A class expression is its class's object, after its
  // static fields get their values. The classes reflection reaches get a
  // description (TzClassInfo in runtime/core/tzrt/tzrt.h) that the object of
  // the class points to: the function that makes an object from arguments as
  // unknowns, the one that makes it from a Json, and its methods as functions
  // that take their `this`. A function value in an unknown is called through
  // the invoker of its type.
  llvm::Value *emitClassExpr(ClassExpr *expr);
  llvm::Value *emitReflectNew(NewExpr *expr);
  llvm::Value *emitReflectCall(CallExpr *call);
  // `u`, an unknown (borrowed), as an owned value of `to`; control goes to
  // `fail`, with nothing owned, if it holds no such value.
  llvm::Value *emitUnknownAs(llvm::Value *u, const Type *to, llvm::BasicBlock *fail);
  // The classes whose objects are of the object type `type`: a class and
  // those down its chain, or those that implement an interface; and whether
  // the class number `id` is that of one of `classes` or of a class down the
  // chain of one.
  std::vector<const ClassDecl *> classesOfType(const Type *type);
  llvm::Value *classIdIn(llvm::Value *id, llvm::ArrayRef<const ClassDecl *> classes);
  // How well the unknown `u` (borrowed) fits a parameter of type `to`, an
  // i32, to choose among the variants of a method or a constructor when the
  // program runs (docs/overloads.md, section 6): 0 not at all, 1 only through
  // the check (an integral double for an int, anything for an unknown), 2 by
  // widening (an int for a double, an object for a class up its chain), 3
  // exactly.
  llvm::Value *emitUnknownFit(llvm::Value *u, const Type *to);
  // The position among `variants` of the one the arguments `args[0..count)`
  // (unknowns, borrowed) fit best, as a call that compiles chooses: each
  // argument fits it no worse than any other variant that takes them, and one
  // better; -1 if none takes them, -2 if several fit equally well. A method
  // takes only a `this` of its class, whose number `selfId` gives (null for
  // constructors).
  llvm::Value *emitChooseVariant(llvm::ArrayRef<const FunctionDecl *> variants,
                                 llvm::Value *selfId, llvm::Value *args, llvm::Value *count);
  // The variants of the method `name` an object of `cls` has, as reflection
  // gives them: its own and those up its chain none of them overrides.
  std::vector<const FunctionDecl *> reflectVariants(const ClassDecl *cls, llvm::StringRef name);
  // The function value Reflect.get gives of a method of several variants:
  // of `type`, `(this: unknown, ...args: unknown[]) => unknown`, which chooses
  // the variant by its arguments when it is called. The info of the value.
  llvm::Constant *reflectDispatchInfo(const ClassDecl *cls,
                                      llvm::ArrayRef<const FunctionDecl *> variants,
                                      const Type *&type);
  std::map<std::pair<const ClassDecl *, std::string>, llvm::Constant *> reflectDispatchers_;
  // The arguments `args[0..count)` (unknowns, borrowed) for the parameters
  // `params` and the rest parameter `rest` (an array type, or null), appended
  // to `values`; `min` of them are surely there. On a mismatch, what was
  // converted is released and `onFail` gets the parameter, its type and the
  // argument, and must leave the block.
  void emitReflectArgs(llvm::Value *args, llvm::Value *count, size_t min,
                       llvm::ArrayRef<const Type *> params, const Type *rest,
                       std::vector<llvm::Value *> &values,
                       llvm::function_ref<void(llvm::Value *, const Type *, llvm::Value *)> onFail);
  // The TypeError of an argument `got` (borrowed) that is no `expected`.
  void emitReflectMismatch(llvm::StringRef what, llvm::Value *index, const Type *expected,
                           llvm::Value *got);
  llvm::StructType *classInfoType();
  llvm::StructType *reflectMethodTy();
  llvm::Constant *classInfoGlobal(const ClassDecl *cls);
  // `ptr (ptr args, i32 count)`: an object of the class from arguments as
  // unknowns, checked against its constructors; its body is emitted at the
  // end. The description has it, and so does Reflect.construct of the class
  // known when compiling.
  llvm::Function *constructFunction(const ClassDecl *cls);
  void emitConstructThunk(const ClassDecl *cls, llvm::Function *fn);
  // A method as a function value of `type`, `(this: unknown, ...params) =>
  // R`, which checks its `this`: the info of the value.
  llvm::Constant *reflectAdapterInfo(const FunctionDecl *fn, const Type *&type);
  llvm::Function *invokerFor(const Type *fnType);
  // `ptr (ptr json, ptr path)`: an object of the class from a Json, through
  // its constructor; its body is emitted at the end.
  llvm::Function *classFromJsonFunction(const ClassDecl *cls);
  void emitClassFromJson(const ClassDecl *cls, llvm::Function *fn);
  // At the end: the descriptions, and what main gives the runtime first.
  void finishReflection(SourceFile &file, llvm::Instruction *init);
  std::vector<const ClassDecl *> emittedClasses_;
  llvm::DenseMap<const ClassDecl *, llvm::GlobalVariable *> classInfos_;
  llvm::DenseMap<const ClassDecl *, llvm::Function *> classConverters_;
  std::vector<const ClassDecl *> pendingConverters_;
  llvm::DenseMap<const FunctionDecl *, llvm::Constant *> reflectAdapters_;
  // Of Builtin::ReflectMethod: the box of each method's function, by its
  // info.
  llvm::DenseMap<const llvm::Constant *, llvm::GlobalVariable *> reflectBoxes_;
  llvm::DenseMap<const Type *, llvm::Function *> invokers_;
  llvm::SetVector<const Type *> unknownFunctionTypes_; // that reach an unknown
  llvm::MapVector<const ClassDecl *, llvm::Function *> constructFunctions_;
  bool usesApply_ = false;
  // What the program looks up through the descriptions: `new` of a class
  // known only when it runs (and Reflect.construct), Reflect.get, and
  // fromJson(cls, j). A description that nothing looks up is not emitted.
  bool reflectsNew_ = false, reflectsGet_ = false, reflectsJson_ = false;
  llvm::StructType *classInfoTy_ = nullptr;
  llvm::StructType *reflectMethodTy_ = nullptr;

  // Dates (codegen_date.cpp). A Date is an object { i64 rc, ptr layout,
  // double time } that the code makes; the runtime
  // (runtime/library/date/date/date.c) computes with times. An
  // Intl.DateTimeFormat is an object of the runtime.
  llvm::Value *emitDateCall(CallExpr *call);
  // A new date that holds `time`.
  llvm::Value *newDate(llvm::Value *time);
  llvm::Value *dateTimeAddress(llvm::Value *date);
  // The TzDateOptions of a locale and options (both may be left out) on the
  // stack; `held` gets the operands to drop after the call.
  llvm::Value *emitDateOptions(llvm::ArrayRef<Expr *> args, std::vector<Operand> &held);

  // Binary data (codegen_bytes.cpp). A typed array, a Buffer or a DataView
  // is a pointer to a TzBytes, an ArrayBuffer to a TzArrayBuffer
  // (runtime/core/tzrt/tzrt.h); both have their data pointer and their length
  // at the same offsets. The code reads and writes elements itself.
  llvm::Value *emitBytesCall(CallExpr *call);
  llvm::Value *emitBytesIndex(IndexExpr *index);
  llvm::Value *emitBytesElementAssign(AssignExpr *assign, bool needResult);
  llvm::Value *emitBytesUpdate(UpdateExpr *update);
  // `.length` of a typed array, `.byteLength` of a DataView or an ArrayBuffer.
  llvm::Value *emitBytesMember(MemberExpr *member);
  // forEach, map, filter, reduce... of a typed array (see emitCallbackMethod).
  llvm::Value *emitBytesCallbackMethod(CallExpr *call);
  Producer bytesProducer(llvm::Value *slot, const Type *type);
  // The data pointer and the length (as i64) of a borrowed object of binary
  // data; their loads are marked apart from those of the elements (see
  // bytesTbaa), so that a loop that writes elements keeps them in registers.
  llvm::Value *bytesData(llvm::Value *bytes);
  llvm::Value *bytesLength(llvm::Value *bytes);
  // Element `index` of a typed array of `type` at `data`, as a value of its
  // element type; and storing such a value, converted as JavaScript does.
  llvm::Value *loadElement(llvm::Value *data, const Type *type, llvm::Value *index);
  void storeElement(llvm::Value *data, const Type *type, llvm::Value *index, llvm::Value *value);
  llvm::MDNode *bytesTbaa(bool header);
  // The constant TzEncodingTable of a single-byte encoding, by name.
  llvm::Constant *encodingTable(llvm::StringRef name);
  llvm::MDNode *tbaaHeader_ = nullptr, *tbaaData_ = nullptr;
  llvm::StructType *bytesTy_ = nullptr; // TzBytes
  std::map<std::u16string, llvm::Constant *> embeddedBytes_;
  llvm::StringMap<llvm::Constant *> encodingTables_;

  // The rest of Intl (codegen_intl.cpp): objects of the runtime
  // (runtime/library/intl/intl/intl.h).
  llvm::Value *emitIntlCall(CallExpr *call);
  // The TzIntlOptions of a locale and options (either may be null) on the
  // stack; `held` gets the operands to drop after the call.
  llvm::Value *emitIntlOptions(Expr *locale, Expr *options, std::vector<Operand> &held);
  // A number for the runtime (TzIntlNumber): its kind and words, and the
  // operand to drop after the call.
  struct IntlNumber {
    Operand operand;
    int kind;
    llvm::Value *d, *integer, *meta;
  };
  IntlNumber emitIntlNumber(Expr *arg);
  std::vector<llvm::Value *> intlNumberArgs(const IntlNumber &n);
  // The TzIntlShape of objects of `type` that the runtime makes.
  llvm::Constant *intlShape(const Type *type);
  // The info of `nf.format` and the like as a value of `fnType`.
  llvm::Constant *intlBoundInfo(IntlKind kind, const Type *fnType);
  // The locales of getCanonicalLocales and supportedLocalesOf, as an array.
  llvm::Value *emitLocaleArray(Expr *arg);
  llvm::DenseMap<const Type *, llvm::Constant *> intlShapes_;
  std::map<std::pair<IntlKind, const Type *>, llvm::Constant *> intlBound_;
  // Cycles (codegen_cycles.cpp, docs/objects.md section 11). Over the whole
  // program, the graph of "a slot of this type may hold an object of that
  // one" (fields, elements, entries, captures, boxes, subtypes) tells which
  // types may form a cycle; their objects are made as members of groups
  // (runtime/core/group/group.c). References that may point at a member are
  // counted through the group, and stores into a member join groups; everything
  // else is counted as before.
  void analyzeCycles(SourceFile &file);
  // Layer 1: the allocations whose objects are never stored (StoreScanner).
  void findUnstored(SourceFile &file);
  // Objects of this object, array or dictionary type are made as members.
  bool isCyclic(const Type *type) const { return cyclicTypes_.contains(type); }
  // The same for the environments of closures (by the key of envLayouts_)
  // and the boxes of variables of a type.
  bool isCyclicEnv(const void *key) const { return cyclicEnvs_.contains(key); }
  bool isCyclicBox(const Type *type) const { return cyclicBoxes_.contains(type); }
  // A reference of this type may point at a member; a value of this type
  // may hold such a reference.
  bool refMayBeMember(const Type *type);
  bool mayHoldMember(const Type *type);
  // Objects of this type may have weak references (WeakRef<T> of it or of a
  // type up its chain), which their drop forgets.
  bool isWeakTarget(const Type *type) const { return weakTargets_.contains(type); }
  llvm::FunctionCallee groupFunction(llvm::StringRef name, llvm::Type *ret,
                                     llvm::ArrayRef<llvm::Type *> params);
  // A new member of `size` bytes, of a group of its own, counted once.
  llvm::Value *allocMember(uint64_t size, const llvm::Twine &name);
  // A fresh object of `type` whose fields the code filled with plain stores:
  // if it is a member, what they hold joins its group.
  void adoptFields(llvm::Value *object, const Type *type);
  // Whether the allocation `site` of `type` makes a member: the type may
  // form cycles, and what it makes may be stored somewhere (layer 1, see
  // StoreScanner).
  bool makesMember(const Expr *site, const Type *type) const {
    return isCyclic(type) && !unstoredSites_.contains(site);
  }
  llvm::DenseSet<const Expr *> unstoredSites_;
  // A layout for the environments and boxes that are members, which the
  // runtime reads their drop function and name from.
  llvm::Constant *memberDescriptor(llvm::Function *drop, llvm::StringRef name);
  // A new array of `arrayType` (`tz_arr_new`, or a member).
  llvm::Value *newArray(const Type *arrayType, llvm::Value *length, llvm::Value *capacity,
                        const llvm::Twine &name = "array");
  llvm::Value *newArray(const Type *arrayType, llvm::Value *length, llvm::Value *capacity,
                        bool member, const llvm::Twine &name);
  // After `owner` took over the owned `value` of `type` into a slot of its:
  // joins the groups of what it refers to (tz_group_store).
  void groupStore(llvm::Value *owner, llvm::Value *value, const Type *type);
  void countShared(llvm::IRBuilder<> &b, llvm::Value *obj, llvm::Value *count, bool retain,
                   llvm::BasicBlock *freeBlock, llvm::BasicBlock *doneBlock);
  // The parallel runtime (runtime/core/publish/publish.c): before a value goes
  // where other threads may reach it, it becomes shared. storeBarrier: into
  // `owner`, an object of the heap, which is shared or not; publishValue: into
  // a global that functions see.
  void storeBarrier(llvm::Value *owner, llvm::Value *value, const Type *type);
  void publishValue(llvm::Value *value, const Type *type);
  // Releases `value`, which a slot of `owner` held.
  void releaseIn(llvm::Value *owner, llvm::Value *value, const Type *type);
  // `value` moved out of a slot of `owner` and is owned by the code now.
  void takeFrom(llvm::Value *owner, llvm::Value *value, const Type *type);
  // Stores an owned value into a slot of `owner` (of static type
  // `ownerType`) that may hold an older one: storeReplacing for an owner that
  // may be a member.
  void storeIn(llvm::Value *owner, const Type *ownerType, llvm::Value *address, llvm::Value *value,
               const Type *type);
  // Whether objects of the static type `type` (an object, array or
  // dictionary type, or a box or environment) may be members whose slots
  // need storeIn: refMayBeMember of it.
  bool ownerMayBeMember(const Type *type) { return refMayBeMember(type); }
  // The value of the variable `var` at `address` (its slot or the field of
  // its box) becomes `value`, owned; `replacing` releases what it held.
  void storeVar(const VarSymbol *var, llvm::Value *address, llvm::Value *value, bool replacing);
  // Counting of an object whose static type is not known here: through its
  // group if it is a member (when the program has any).
  void retainObject(llvm::Value *object);
  // A global that is never assigned again holds its value for good: the
  // groups in it are eternal (tz_group_pin_value), and at the end of a
  // program checked for leaks it lets them go (tz_group_end_value).
  bool isEternal(const VarSymbol *var) const { return eternals_.contains(var); }
  void pinValue(llvm::Value *value, const Type *type);
  void endValue(llvm::Value *value, const Type *type);
  // The group-aware form of a release function of objects: members go
  // through their group.
  llvm::Function *groupAwareRelease(llvm::Function *plain);
  // The drop function of the objects of a cyclic type: what the fields hold
  // goes as from a member (releaseIn), and the memory unless it is one.
  void emitMemberDrop(const Type *type, ObjectLayout &layout);
  // At the start of the drop of an object that may have weak references.
  void forgetWeakRefs(llvm::Value *object);
  // `new WeakRef(x)` and `w.deref()`.
  llvm::Value *emitWeakRefCall(CallExpr *call);
  // releaseIn of a box or an environment that `owner` held.
  void releaseObjectIn(llvm::Value *owner, llvm::Value *object);
  llvm::DenseSet<const Type *> cyclicTypes_, cyclicBoxes_, weakTargets_;
  llvm::DenseSet<const void *> cyclicEnvs_;
  // Representations of cyclic array types, which generic code may see.
  llvm::DenseSet<const Type *> cyclicArrayReps_;
  llvm::DenseMap<const Type *, bool> refMember_;
  llvm::DenseSet<const VarSymbol *> eternals_;
  // Layer 2: the captures of closures (by the key of their environment)
  // that are not counted, as the closure lives in an object its owner made
  // and holds alone; and those fields of the owners, by type and name.
  std::set<std::pair<const void *, const VarSymbol *>> uncounted_;
  std::set<std::pair<const Type *, std::string>> ownedFields_, backRefFields_;
  // Debug mode (a program compiled with TZ_CHECK_LEAKS set, as the tests
  // are): the proofs of layers 1 and 2 are checked when the program runs,
  // and large groups are walked (runtime/core/group/group.c).
  bool debugMemory_ = false;
  // The field a store into a member being emitted writes, for the place
  // debug mode notes (tz_group_site).
  std::string storeField_;
  llvm::SMLoc storeLoc_;
  llvm::Constant *groupSite();
  std::map<std::pair<llvm::Constant *, std::string>, llvm::Constant *> groupSites_;
  // An object made at `site` that the compiler found is never stored.
  void noteUnstored(llvm::Value *object, const Expr *site);
  // In the drop of an object of `type`, the value of its field `name`, which
  // goes now: an owned object (layer 2) must have no other reference.
  void checkOwned(const Type *type, llvm::StringRef name, llvm::Value *value,
                  const Type *fieldType);
  // The functions that give the walk the references in the members, by
  // layout, handed to the runtime after main's init.
  void finishGroupDebug(llvm::Instruction *init);
  // The parallel runtime (runtime/core/publish/publish.c): for each descriptor
  // of an object the program makes, the function that publishes what the object
  // holds, handed over at the start of main.
  void finishPublishTable(llvm::Instruction *init);
  llvm::Function *publishFunction(const Type *type, llvm::StructType *layout,
                                  llvm::ArrayRef<std::pair<const VarSymbol *, unsigned>> captures,
                                  llvm::ArrayRef<std::pair<unsigned, const Type *>> values,
                                  llvm::StringRef name);
  // Other objects of the program's own: their descriptor, struct and the
  // values in it, by element.
  struct PublishedObject {
    llvm::Constant *descriptor;
    llvm::StructType *type;
    std::vector<std::pair<unsigned, const Type *>> values;
  };
  std::vector<PublishedObject> publishedObjects_;
  // The async functions whose pieces may run ahead of their turn: what they
  // get is published as they start.
  llvm::DenseSet<const FunctionDecl *> aheadFunctions_;
  llvm::Function *traceFunction(const Type *type, llvm::StructType *layout,
                                llvm::ArrayRef<std::pair<const VarSymbol *, unsigned>> captures,
                                llvm::StringRef name);
  llvm::DenseMap<llvm::Function *, llvm::Constant *> memberDescriptors_;
  std::vector<std::pair<llvm::Constant *, const Type *>> boxDescriptors_;
  // The slots in main of those that no function uses.
  llvm::DenseSet<llvm::Value *> eternalSlots_;
  bool hasCycles_ = false;       // some type, environment or box is cyclic
  const ParallelPlan *plan_ = nullptr;
  bool parallel_ = false;        // runs on the threads of tzrt_mt
  // The awaits after which a piece may run ahead of its turn, and the
  // places where pieces touch what keeps node's order (see ParallelPlan).
  // Each by its number, from 1: the site of runtime/core/parallel/parallel.c.
  llvm::DenseMap<const Node *, uint32_t> aheadStarts_;
  llvm::DenseSet<const char *> orderedLocs_;
  const AwaitExpr *awaiting_ = nullptr; // the await being emitted
  llvm::DenseSet<const VarSymbol *> libraryState_; // see addressOf
  bool anyCyclicObject_ = false; // some object or dictionary type is
  bool fnMayBeMember_ = false;   // a function value's environment may be a member
  llvm::Function *refRetainFn_ = nullptr; // tz.ref.retain
  llvm::DenseMap<llvm::Function *, llvm::Function *> groupReleases_;
  llvm::Function *plainInstanceReleaseFn_ = nullptr;
  // A promise of `value` is a member of a group: what it settles with may be
  // one (its value, or its error when an Error may be), see
  // runtime/core/async/async.c.
  bool promiseIsMember(const Type *value) {
    return hasCycles_ && (anyCyclicObject_ || (value && mayHoldMember(value)));
  }
  llvm::Function *plainObjReleaseFn_ = nullptr;
  llvm::Function *plainObjRetainFn_ = nullptr;

  llvm::Value *addressOf(const VarSymbol *var);
  llvm::AllocaInst *createStackSlot(llvm::Type *type, const llvm::Twine &name);
  llvm::Constant *utf8Constant(llvm::StringRef text);
  void setFunctionAttributes(llvm::Function *fn);

  llvm::LLVMContext &ctx_;
  std::unique_ptr<llvm::Module> module_;
  llvm::IRBuilder<> builder_;
  llvm::StructType *strTy_;   // { ptr obj, i64 meta }
  llvm::StructType *arrayTy_; // the array header, see runtime/core/array/array.c
  llvm::StructType *typeDescTy_;
  llvm::StructType *fnTy_;     // { ptr info, ptr env }
  llvm::StructType *fnInfoTy_; // { ptr code, string name }
  llvm::StructType *decTy_;    // { i64 coef, i64 meta }

  llvm::Function *function_ = nullptr;
  const Type *returnType_ = nullptr;
  FunctionDecl *currentFn_ = nullptr; // null in main
  llvm::Value *envParam_ = nullptr;   // the environment of the closure being emitted
  // The variables of the function being emitted, and the globals.
  llvm::DenseMap<const VarSymbol *, llvm::Value *> addresses_, globals_;
  // Boxed variables: the slot that holds the box of one the function
  // declares, or the box of one it captured.
  llvm::DenseMap<const VarSymbol *, llvm::Value *> boxSlots_, envBoxes_;
  // The slot that holds the environment of a group the function declares.
  llvm::DenseMap<const NestedGroup *, llvm::Value *> groupEnvs_;
  llvm::DenseMap<const FunctionDecl *, llvm::Function *> closureCode_;
  llvm::DenseMap<const FunctionDecl *, llvm::Constant *> infos_;
  llvm::DenseMap<const void *, EnvLayout> envLayouts_;
  llvm::DenseMap<const Type *, llvm::StructType *> boxTypes_;
  llvm::DenseMap<const Type *, llvm::Function *> boxDrops_;
  std::map<std::pair<const FunctionDecl *, const Type *>, llvm::Constant *> adapters_;
  std::vector<FunctionDecl *> pendingClosures_;
  bool comparesFunctions_ = false;
  // The program has TLS (SourceFile::usesTls): the intrinsics `__tz_tls_*` call it.
  bool usesTls_ = false;
  std::deque<TailGroup> tailGroups_;
  llvm::DenseMap<const FunctionDecl *, TailGroup *> tailGroupOf_;
  llvm::DenseSet<const void *> tailSearched_;
  TailGroup *tailGroup_ = nullptr; // being emitted
  // Statements that are calls in tail position of a function returning void.
  llvm::DenseSet<const Stmt *> tailStatements_;
  llvm::DenseMap<const FuncSymbol *, llvm::Function *> functions_;
  llvm::DenseMap<const Type *, llvm::Constant *> typeDescriptors_;
  std::map<const Type *, ObjectLayout> objectLayouts_; // stable while more are added
  llvm::DenseMap<const Type *, ResultLayout> resultLayouts_;
  std::map<std::pair<const Type *, std::vector<unsigned>>, llvm::Constant *> layoutDescriptors_;
  llvm::StructType *fieldDescTy_;  // { ptr name, i32 length, i32 offset, ptr type }
  // { ptr drop, ptr fields, i32 count, i32 flags, ptr name, i32 name length,
  //   i32 class number }
  llvm::StructType *layoutDescTy_;
  llvm::DenseMap<const ClassDecl *, llvm::Constant *> classDescriptors_;
  llvm::DenseMap<llvm::Function *, llvm::Constant *> dropDescriptors_;
  // Interfaces (codegen_inherit.cpp): those with implementers, and how many
  // numbers they take; the interfaces with tables that each type has; the
  // types with subtypes; and what was made for them.
  std::map<const Type *, InterfaceInfo> interfaces_;
  std::map<const Type *, std::vector<const Type *>> tablesOf_;
  unsigned colors_ = 0;
  llvm::DenseSet<const Type *> withSubtypes_;
  std::map<const Type *, llvm::Constant *> itables_;
  std::map<std::pair<const Type *, const Type *>, llvm::Constant *> itableConstants_;
  std::map<std::pair<const Type *, std::string>, std::pair<llvm::Function *, llvm::Constant *>>
      fieldStubs_;
  std::vector<const VarSymbol *> managedGlobals_;
  // What the names of the top-level declarations of imported files start
  // with in the IR: their file.
  llvm::DenseMap<const Stmt *, std::string> symbolPrefixes_;
  llvm::StringMap<llvm::Constant *> utf8Constants_;
  std::map<std::u16string, llvm::Constant *> literals_;
  std::vector<Scope> scopes_;

  // A loop, switch or labeled statement that `break` and `continue` jump out
  // of.
  struct JumpTarget {
    llvm::SmallVector<llvm::StringRef, 1> labels;
    bool isLoop;
    bool isSwitch;
    llvm::BasicBlock *breakTarget;
    llvm::BasicBlock *continueTarget; // loops only
    size_t scopeDepth; // scopes to keep when jumping to either target
    // `continue` in a loop over a generator emitted inline goes on in the
    // generator, after the `yield`, inside its scopes; ~0 for scopeDepth.
    size_t continueDepth = ~size_t(0);
  };
  std::vector<JumpTarget> targets_;
  // Generators and iterators: the generator emitted inline whose code is
  // being emitted, the coroutine being emitted, and the functions CodeGen
  // makes for the iterators of arrays and helpers, emitted after the others.
  InlineGenerator *inline_ = nullptr;
  // How many times the code being emitted is copied by the generators
  // emitted inline around it, one copy for each `yield`.
  unsigned inlineCopies_ = 1;
  Coroutine *coro_ = nullptr;
  struct IteratorJob {
    llvm::Function *function;
    Builtin builtin;
    const Type *source, *operand, *result;
  };
  std::vector<IteratorJob> pendingIterators_;
  std::map<std::tuple<Builtin, const Type *, const Type *, const Type *>, llvm::Function *>
      iteratorFunctions_;
  llvm::Function *iterDropFn_ = nullptr;
  // `void tz.iter.close(ptr iterator)`: return() of JavaScript.
  llvm::Function *iterCloseFunction();
  llvm::Function *iterCloseFn_ = nullptr;
  std::map<unsigned, llvm::Constant *> iteratorLayouts_;
  llvm::StructType *promiseTy_ = nullptr;
  // The labels of the loop or switch being emitted next.
  llvm::SmallVector<llvm::StringRef, 1> pendingLabels_;
  // Where the links of the optional chains being emitted go when their object
  // is undefined: the innermost chain's last.
  std::vector<llvm::BasicBlock *> chainEnds_;
  void pushLoop(llvm::BasicBlock *breakTarget, llvm::BasicBlock *continueTarget, size_t scopeDepth);

  // Functions of the C runtime (runtime/).
  struct Runtime {
    llvm::FunctionCallee init;
    llvm::FunctionCallee shutdown;
    llvm::FunctionCallee mainDone;
    llvm::FunctionCallee printI32;
    llvm::FunctionCallee printI64;
    llvm::FunctionCallee printF64;
    llvm::FunctionCallee printBool;
    llvm::FunctionCallee printChar;
    llvm::FunctionCallee printString;
    llvm::FunctionCallee printValue;
    llvm::FunctionCallee debugger;
    llvm::FunctionCallee strFree;
    llvm::FunctionCallee strConcatN;
    llvm::FunctionCallee strEqual;
    llvm::FunctionCallee strCompare;
    llvm::FunctionCallee strCharAt;
    llvm::FunctionCallee strCharCodeAt;
    llvm::FunctionCallee strCodePointAt;
    llvm::FunctionCallee strSlice;
    llvm::FunctionCallee strSubstring;
    llvm::FunctionCallee strIndexOf;
    llvm::FunctionCallee strIncludes;
    llvm::FunctionCallee strStartsWith;
    llvm::FunctionCallee strEndsWith;
    llvm::FunctionCallee strRepeat;
    llvm::FunctionCallee strFromI64;
    llvm::FunctionCallee strFromF64;
    llvm::FunctionCallee strCharAtOrEmpty;
    llvm::FunctionCallee strCodePoint;
    llvm::FunctionCallee strLastIndexOf;
    llvm::FunctionCallee strSplit;
    llvm::FunctionCallee strTrim;
    llvm::FunctionCallee strPad;
    llvm::FunctionCallee strToUpper;
    llvm::FunctionCallee strToLower;
    llvm::FunctionCallee strReplace;
    llvm::FunctionCallee strFromCharCodes;
    llvm::FunctionCallee strFromCodePoints;
    llvm::FunctionCallee strToNumber;
    llvm::FunctionCallee parseInt;
    llvm::FunctionCallee parseFloat;
    llvm::FunctionCallee numToFixed;
    llvm::FunctionCallee numToRadix;
    llvm::FunctionCallee toInt32;
    llvm::FunctionCallee mathRound;
    llvm::FunctionCallee mathPow;
    llvm::FunctionCallee mathPowLong;
    llvm::FunctionCallee mathHypot;
    llvm::FunctionCallee mathRandom;
    llvm::FunctionCallee checkedInt32OfLong;
    llvm::FunctionCallee valueToString;
    llvm::FunctionCallee arrNew;
    llvm::FunctionCallee arrNewLength;
    llvm::FunctionCallee arrFree;
    llvm::FunctionCallee arrGrow;
    llvm::FunctionCallee arrShift;
    llvm::FunctionCallee arrUnshiftSlots;
    llvm::FunctionCallee arrSetLength;
    llvm::FunctionCallee arrSlice;
    llvm::FunctionCallee arrCopy;
    llvm::FunctionCallee arrAppend;
    llvm::FunctionCallee arrAppendCodePoints;
    llvm::FunctionCallee arrIndexOf;
    llvm::FunctionCallee arrJoin;
    llvm::FunctionCallee arrReverse;
    llvm::FunctionCallee arrFill;
    llvm::FunctionCallee arrSort;
    llvm::FunctionCallee arrSortWith;
    llvm::FunctionCallee arrSplice;
    llvm::FunctionCallee objAlloc;
    llvm::FunctionCallee objFree;
    llvm::FunctionCallee objKeys;
    llvm::FunctionCallee objValues;
    llvm::FunctionCallee objEntries;
  } rt_;
  // A runtime function of doubles: tz_math_sin and the like.
  llvm::FunctionCallee mathFunction(llvm::StringRef name, unsigned params);
  llvm::Function *retainFn_ = nullptr;
  llvm::Function *releaseFn_ = nullptr;
  llvm::Function *arrRetainFn_ = nullptr;
  llvm::Function *arrReleaseFn_ = nullptr;
  llvm::Function *objRetainFn_ = nullptr;
  llvm::Function *objReleaseFn_ = nullptr;
  llvm::Function *instanceReleaseFn_ = nullptr;
  // tz.dec.retain and tz.dec.release, made when first needed.
  llvm::Function *decimalRefCount(bool retain);
  llvm::Function *decRetainFn_ = nullptr;
  llvm::Function *decReleaseFn_ = nullptr;

  // Exceptions (codegen_throw.cpp): what analyzeThrows found, the functions
  // that check the stack at their start (those that may recurse), and the
  // tail calls that are jumps (findTailGroups), which do not recurse.
  llvm::DenseSet<const FunctionDecl *> throwing_, recursive_;
  llvm::DenseSet<const Type *> throwingTypes_;
  llvm::DenseMap<const Expr *, bool> exprThrows_;
  llvm::DenseSet<const CallExpr *> tailJumps_;
  // A `try` being emitted. Errors from its block go to its `catch`, or with
  // only a `finally`, to that; errors from the `catch` go to the `finally`.
  // A `finally` runs on every way out of the block and the `catch`: each
  // sets `completion` to say how to go on after it (0 on to the end, 1 throw
  // the pending error, 2 return the pending value, 3 + k take exits[k]).
  struct TryContext {
    enum class Phase { Block, Handler, Finally };
    TryStmt *stmt;
    size_t scopeDepth;   // scopes_.size() at its start
    size_t targetsDepth; // targets_.size() at its start
    Phase phase = Phase::Block;
    llvm::BasicBlock *handler = nullptr;    // the catch
    llvm::BasicBlock *finally = nullptr;    // the finally, with completion set
    llvm::BasicBlock *throwEntry = nullptr; // the finally for an error
    llvm::Value *completion = nullptr;
    llvm::Value *pendingError = nullptr, *pendingValue = nullptr;
    bool returns = false; // some `return` goes through the `finally`
    struct Exit {
      size_t target;
      bool isBreak;
    };
    std::vector<Exit> exits;
  };
  std::vector<TryContext> tries_;
  // Where an error leaves the function being emitted.
  llvm::BasicBlock *unwindExit_ = nullptr;
  // The call a `return` being emitted gives, left unchecked (see emitReturn).
  const CallExpr *returnCall_ = nullptr;
  // The expression being emitted: where a built-in that fails throws from.
  llvm::SMLoc loc_;
  const llvm::SourceMgr *sourceMgr_ = nullptr;
  llvm::GlobalVariable *thrown_ = nullptr; // tz_thread
  std::map<std::pair<std::string, const char *>, llvm::Constant *> frames_;
  llvm::StringMap<std::string> filePaths_;
  ClassDecl *errorClass_ = nullptr;
  llvm::Function *errorMaker_ = nullptr;
};

} // namespace tz
