#pragma once

#include "ast/Ast.h"
#include "sema/Types.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <memory>

namespace tz {

// Lowers a checked AST to LLVM IR.
//
// Every local variable gets a stack slot (alloca) in the function's entry
// block and is accessed with loads and stores; LLVM's mem2reg pass turns them
// into registers. Top-level code becomes `main`.
class CodeGen {
public:
  CodeGen(llvm::LLVMContext &ctx, llvm::StringRef moduleName);

  std::unique_ptr<llvm::Module> generate(SourceFile &file);

private:
  llvm::Type *lower(const Type *type);

  // Module structure
  void declareRuntime();
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

  // Statements
  void emitStmt(Stmt *stmt);
  void emitVarDecl(VarDeclStmt *decl);
  void emitIf(IfStmt *stmt);
  void emitWhile(WhileStmt *stmt);
  void emitDoWhile(DoWhileStmt *stmt);
  void emitFor(ForStmt *stmt);
  void emitReturn(ReturnStmt *stmt);

  // Expressions
  llvm::Value *emitExpr(Expr *expr);
  llvm::Value *emitUnary(UnaryExpr *unary);
  llvm::Value *emitBinary(BinaryExpr *bin);
  llvm::Value *emitLogical(BinaryExpr *bin);
  llvm::Value *emitArithmetic(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs, const Type *type);
  llvm::Value *emitComparison(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs, const Type *type);
  llvm::Value *emitIntDivRem(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs);
  llvm::Value *emitAssign(AssignExpr *assign);
  llvm::Value *emitUpdate(UpdateExpr *update);
  llvm::Value *emitCall(CallExpr *call);
  void emitConsoleLog(CallExpr *call);
  llvm::Value *emitConditional(ConditionalExpr *cond);
  llvm::Value *emitConversion(llvm::Value *value, const Type *from, const Type *to);
  void emitPanic(llvm::StringRef message);

  llvm::Value *addressOf(const VarSymbol *var);
  llvm::Value *addressOfTarget(Expr *target);
  llvm::AllocaInst *createStackSlot(llvm::Type *type, const llvm::Twine &name);
  llvm::Constant *stringConstant(llvm::StringRef text);
  void setFunctionAttributes(llvm::Function *fn);

  llvm::LLVMContext &ctx_;
  std::unique_ptr<llvm::Module> module_;
  llvm::IRBuilder<> builder_;

  llvm::Function *function_ = nullptr;
  llvm::DenseMap<const VarSymbol *, llvm::Value *> addresses_;
  llvm::DenseMap<const FuncSymbol *, llvm::Function *> functions_;
  llvm::StringMap<llvm::Constant *> strings_;

  struct LoopTargets {
    llvm::BasicBlock *breakTarget;
    llvm::BasicBlock *continueTarget;
  };
  std::vector<LoopTargets> loops_;

  // Functions of the C runtime (runtime/tzrt.c).
  struct Runtime {
    llvm::FunctionCallee init;
    llvm::FunctionCallee printStr;
    llvm::FunctionCallee printI32;
    llvm::FunctionCallee printI64;
    llvm::FunctionCallee printF64;
    llvm::FunctionCallee printBool;
    llvm::FunctionCallee printChar;
    llvm::FunctionCallee panic;
  } rt_;
};

} // namespace tz
