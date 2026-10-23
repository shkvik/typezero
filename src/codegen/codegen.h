#pragma once

#include "ast/ast.h"
#include "sema/types.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <deque>
#include <map>
#include <memory>

namespace tz {

// Lowers a checked AST to LLVM IR.
//
// Every local variable gets a stack slot (alloca) in the function's entry
// block and is accessed with loads and stores; LLVM's mem2reg pass turns them
// into registers. Top-level code becomes `main`.
//
// Representation of values:
//   int, long, double, boolean   i32, i64, double, i1
//   string                       { ptr obj, i64 meta } (runtime/string.c)
//   T[]                          ptr to an array header (runtime/array.c)
//   T | undefined                { T value, i1 has }; the value is zero when
//                                undefined, so it can always be released
//   function                     { ptr info, ptr env }: info is a constant
//                                { ptr code, string name }, env the closure's
//                                environment or null (see codegen_closure.cpp)
//   object                       ptr to { i64 rc, ptr layout, fields... }
//                                (see codegen_object.cpp); an object's
//                                `T | undefined` is that ptr, null when
//                                undefined
//
// Strings, arrays, functions and objects are reference counted ("managed",
// as are optionals of them). The ownership rules are simple for now and LLVM
// cleans up part of the overhead:
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

private:
  llvm::Type *lower(const Type *type);

  // Module structure
  void declareRuntime();
  void defineRefCountHelpers();
  void declareFunction(FunctionDecl *fn);
  void declareGlobals(SourceFile &file);
  void emitFunction(FunctionDecl *fn);
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
  struct Scope {
    std::vector<std::pair<llvm::Value *, const Type *>> slots;
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
  llvm::Value *emitConditional(ConditionalExpr *cond);
  // Converts an owned value; the result is owned.
  llvm::Value *emitConversion(llvm::Value *value, const Type *from, const Type *to);
  void emitPanic(llvm::StringRef message);

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
  // A new box for a variable of `type`, holding its zero value.
  llvm::Value *newBox(const Type *type);
  llvm::Value *boxPointer(const VarSymbol *var);
  // A new environment with the current values of `layout`'s variables. On
  // the stack, it borrows them and is not counted.
  llvm::Value *createEnv(const EnvLayout &layout, bool onStack);
  llvm::Value *makeFunctionValue(llvm::Value *info, llvm::Value *env);
  // The value of a variable, owned or borrowed: a function's own value when
  // the variable names the function being emitted or one of its group.
  llvm::Value *emitVarValue(const VarSymbol *var, bool borrow);
  llvm::Value *emitFunctionExpr(FunctionExpr *fe);
  llvm::Value *emitFunctionAdapter(CastExpr *cast);
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
  // The addresses of the variables a closure captured, in its environment.
  void bindCaptures(const FunctionDecl *fn);

  // Objects (codegen_object.cpp). The fields of a layout lie in memory by
  // alignment, largest first, then by name: so no padding is needed between
  // them, and an object type and its read-only twin have the same layout.
  struct ObjectLayout {
    llvm::StructType *type = nullptr; // { i64 rc, ptr layout, fields... }
    // The struct element of each field, in the order of Type::fields().
    std::vector<unsigned> elements;
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
  void emitObjectDrop(const Type *type, ObjectLayout &layout);
  llvm::Value *fieldAddress(llvm::Value *object, const Type *type, llvm::StringRef name);
  llvm::Value *emitObjectLiteral(ObjectLitExpr *lit);
  llvm::Value *emitFieldRead(MemberExpr *member);
  llvm::Value *emitFieldAssign(AssignExpr *assign, bool needResult);

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

  llvm::Value *addressOf(const VarSymbol *var);
  llvm::AllocaInst *createStackSlot(llvm::Type *type, const llvm::Twine &name);
  llvm::Constant *utf8Constant(llvm::StringRef text);
  void setFunctionAttributes(llvm::Function *fn);

  llvm::LLVMContext &ctx_;
  std::unique_ptr<llvm::Module> module_;
  llvm::IRBuilder<> builder_;
  llvm::StructType *strTy_;   // { ptr obj, i64 meta }
  llvm::StructType *arrayTy_; // the array header, see runtime/array.c
  llvm::StructType *typeDescTy_;
  llvm::StructType *fnTy_;     // { ptr info, ptr env }
  llvm::StructType *fnInfoTy_; // { ptr code, string name }

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
  std::deque<TailGroup> tailGroups_;
  llvm::DenseMap<const FunctionDecl *, TailGroup *> tailGroupOf_;
  llvm::DenseSet<const void *> tailSearched_;
  TailGroup *tailGroup_ = nullptr; // being emitted
  // Statements that are calls in tail position of a function returning void.
  llvm::DenseSet<const Stmt *> tailStatements_;
  llvm::DenseMap<const FuncSymbol *, llvm::Function *> functions_;
  llvm::DenseMap<const Type *, llvm::Constant *> typeDescriptors_;
  std::map<const Type *, ObjectLayout> objectLayouts_; // stable while more are added
  std::map<std::pair<const Type *, std::vector<unsigned>>, llvm::Constant *> layoutDescriptors_;
  llvm::StructType *fieldDescTy_;  // { ptr name, i32 length, i32 offset, ptr type }
  llvm::StructType *layoutDescTy_; // { ptr drop, ptr fields, i32 count }
  std::vector<const VarSymbol *> managedGlobals_;
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
  };
  std::vector<JumpTarget> targets_;
  // The labels of the loop or switch being emitted next.
  llvm::SmallVector<llvm::StringRef, 1> pendingLabels_;
  // Where the links of the optional chains being emitted go when their object
  // is undefined: the innermost chain's last.
  std::vector<llvm::BasicBlock *> chainEnds_;
  void pushLoop(llvm::BasicBlock *breakTarget, llvm::BasicBlock *continueTarget, size_t scopeDepth);

  // Functions of the C runtime (runtime/*.c).
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
    llvm::FunctionCallee panic;
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
    llvm::FunctionCallee toInt32;
    llvm::FunctionCallee mathRound;
    llvm::FunctionCallee mathPow;
    llvm::FunctionCallee mathPowLong;
    llvm::FunctionCallee mathHypot;
    llvm::FunctionCallee mathRandom;
    llvm::FunctionCallee checkedInt32;
    llvm::FunctionCallee checkedInt64;
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
  } rt_;
  // A runtime function of doubles: tz_math_sin and the like.
  llvm::FunctionCallee mathFunction(llvm::StringRef name, unsigned params);
  llvm::Function *retainFn_ = nullptr;
  llvm::Function *releaseFn_ = nullptr;
  llvm::Function *arrRetainFn_ = nullptr;
  llvm::Function *arrReleaseFn_ = nullptr;
  llvm::Function *objRetainFn_ = nullptr;
  llvm::Function *objReleaseFn_ = nullptr;
};

} // namespace tz
