#pragma once

#include "ast/ast.h"
#include "sema/types.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

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
//
// Strings and arrays are reference counted ("managed", as are optionals of
// them). The ownership rules are simple for now and LLVM cleans up part of
// the overhead:
// - an expression of a managed type produces an owned (+1) value, which its
//   consumer either stores or releases;
// - a variable owns its value: assignment releases the old one, and leaving a
//   scope releases the variables declared in it;
// - an array owns its elements;
// - a call passes ownership of its arguments to the callee, which releases
//   its parameters when it returns;
// - an operand may be borrowed, without retain and release, when it is a
//   plain variable and nothing evaluated while it is in use can reassign that
//   variable.
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

  // Scopes: managed variables to release when control leaves them.
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

  // Statements
  void emitStmt(Stmt *stmt);
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
  // Evaluates an expression for its side effects only.
  void emitDiscarded(Expr *expr);
  // Stores an owned value into a slot that may hold an older value.
  void storeReplacing(llvm::Value *address, llvm::Value *value, const Type *type);

  // Expressions
  llvm::Value *emitExpr(Expr *expr);
  llvm::Value *emitUnary(UnaryExpr *unary);
  llvm::Value *emitBinary(BinaryExpr *bin);
  llvm::Value *emitLogical(BinaryExpr *bin);
  llvm::Value *emitEquality(BinaryExpr *bin);
  llvm::Value *emitNullish(BinaryExpr *bin);
  llvm::Value *emitArithmetic(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs, const Type *type);
  llvm::Value *emitComparison(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs, const Type *type);
  // Whether two values of `type` are equal (===); both borrowed.
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
  // `T | undefined`
  llvm::Value *makeOptional(llvm::Value *value, llvm::Value *has, const Type *type);
  llvm::Value *undefinedValue(const Type *optionalType);

  // Operands that are used and then dropped.
  struct Operand {
    llvm::Value *value;
    bool owned; // must be released after use
    const Type *type;
  };
  Operand emitOperand(Expr *expr, bool canBorrow);
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
  llvm::Value *emitNewArray(NewExpr *expr);
  llvm::Value *emitArrayIndex(IndexExpr *index);
  llvm::Value *emitArrayMethod(CallExpr *call);
  void emitArrayForOf(ForOfStmt *stmt, llvm::Value *iterableSlot, llvm::BasicBlock *condBlock,
                      llvm::BasicBlock *bodyBlock, llvm::BasicBlock *endBlock);
  // A value converted to a string by the runtime, for arrays and optionals.
  llvm::Value *emitValueToString(llvm::Value *value, const Type *type);

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

  llvm::Function *function_ = nullptr;
  const Type *returnType_ = nullptr;
  llvm::DenseMap<const VarSymbol *, llvm::Value *> addresses_;
  llvm::DenseMap<const FuncSymbol *, llvm::Function *> functions_;
  llvm::DenseMap<const Type *, llvm::Constant *> typeDescriptors_;
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
    llvm::FunctionCallee arrIndexOf;
    llvm::FunctionCallee arrJoin;
    llvm::FunctionCallee arrReverse;
    llvm::FunctionCallee arrFill;
    llvm::FunctionCallee arrSort;
    llvm::FunctionCallee arrSplice;
  } rt_;
  // A runtime function of doubles: tz_math_sin and the like.
  llvm::FunctionCallee mathFunction(llvm::StringRef name, unsigned params);
  llvm::Function *retainFn_ = nullptr;
  llvm::Function *releaseFn_ = nullptr;
  llvm::Function *arrRetainFn_ = nullptr;
  llvm::Function *arrReleaseFn_ = nullptr;
};

} // namespace tz
