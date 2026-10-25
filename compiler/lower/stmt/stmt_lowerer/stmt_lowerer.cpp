#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/classes/decorator_lowerer/decorator_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/errors/try_lowerer/try_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/functions/tail_call_lowerer/tail_call_lowerer.h"
#include "compiler/lower/iterators/async_generator_lowerer/async_generator_lowerer.h"
#include "compiler/lower/memory/publish_lowerer/publish_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/stmt/loop_lowerer/loop_lowerer.h"
#include "compiler/lower/stmt/switch_lowerer/switch_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <array>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

StmtLowerer::StmtLowerer(Lowering &lowering) : lowering_(lowering) {}

void StmtLowerer::emitStmt(Stmt *stmt) {
  lowering_.orderAt(stmt->loc());
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    lowering_.functionFrame().pushScope();
    emitStatements(cast<BlockStmt>(stmt));
    lowering_.functionFrame().popScope();
    break;
  case Stmt::Kind::VarDecl:
    emitVarDecl(cast<VarDeclStmt>(stmt));
    break;
  case Stmt::Kind::Function:
    break; // emitted on its own
  case Stmt::Kind::Expr:
    if (lowering_.tailStatements_.contains(stmt)) {
      lowering_.tailCallLowerer().emitTailJump(cast<CallExpr>(cast<ExprStmt>(stmt)->expr));
    } else {
      emitDiscarded(cast<ExprStmt>(stmt)->expr);
    }
    break;
  case Stmt::Kind::If:
    emitIf(cast<IfStmt>(stmt));
    break;
  case Stmt::Kind::While:
    lowering_.loopLowerer().emitWhile(cast<WhileStmt>(stmt));
    break;
  case Stmt::Kind::DoWhile:
    lowering_.loopLowerer().emitDoWhile(cast<DoWhileStmt>(stmt));
    break;
  case Stmt::Kind::For:
    lowering_.loopLowerer().emitFor(cast<ForStmt>(stmt));
    break;
  case Stmt::Kind::ForOf:
    lowering_.loopLowerer().emitForOf(cast<ForOfStmt>(stmt));
    break;
  case Stmt::Kind::Switch:
    lowering_.switchLowerer().emitSwitch(cast<SwitchStmt>(stmt));
    break;
  case Stmt::Kind::Labeled:
    lowering_.loopLowerer().emitLabeled(cast<LabeledStmt>(stmt));
    break;
  case Stmt::Kind::Return:
    emitReturn(cast<ReturnStmt>(stmt));
    break;
  case Stmt::Kind::Break:
  case Stmt::Kind::Continue:
    lowering_.loopLowerer().emitJump(stmt);
    break;
  case Stmt::Kind::Empty:
  case Stmt::Kind::TypeAlias:
  case Stmt::Kind::Enum:
    break;
  case Stmt::Kind::Class:
    // The class of a class expression gets its static fields where the
    // expression is.
    if (!cast<ClassDecl>(stmt)->expression) {
      lowering_.decoratorLowerer().emitClassDeclaration(cast<ClassDecl>(stmt));
    }
    break;
  case Stmt::Kind::Debugger:
    lowering_.builder_.CreateCall(lowering_.rt_.debugger);
    break;
  case Stmt::Kind::Throw:
    lowering_.throwLowerer().emitThrowStmt(cast<ThrowStmt>(stmt));
    break;
  case Stmt::Kind::Try:
    lowering_.tryLowerer().emitTry(cast<TryStmt>(stmt));
    break;
  }
}

void StmtLowerer::emitStatements(BlockStmt *block) {
  NestedGroup *group = block->group;
  for (size_t i = 0; i < block->body.size(); ++i) {
    if (group && i == group->firstUse) {
      lowering_.envLowerer().emitGroupCreation(group);
    }
    emitStmt(block->body[i]);
  }
}

void StmtLowerer::storeReplacing(llvm::Value *address, llvm::Value *value, const Type *type) {
  if (!type->isManaged()) {
    lowering_.builder_.CreateStore(value, address);
    return;
  }
  llvm::Value *previous =
      lowering_.builder_.CreateLoad(lowering_.moduleBuilder().lower(type), address);
  lowering_.builder_.CreateStore(value, address);
  lowering_.refCounts().releaseValue(previous, type);
}

void StmtLowerer::emitVarDecl(VarDeclStmt *decl) {
  for (VarDeclarator &d : decl->decls) {
    if (d.alias) { // another name for a generic function
      continue;
    }
    const VarSymbol *var = d.sym;
    llvm::Type *type = lowering_.moduleBuilder().lower(var->type);
    bool global = var->isGlobal && var->usedInFunction;
    llvm::Value *address = global ? lowering_.functionFrame().addressOf(var)
                                  : lowering_.functionFrame().createLocal(var, d.name);
    llvm::Value *value =
        d.init ? lowering_.exprLowerer().emitExpr(d.init) : llvm::Constant::getNullValue(type);
    // The slot may still hold a value from an earlier run of this declaration.
    lowering_.storeLowerer().storeVar(var, address, value, /*replacing=*/true);
    // A global that keeps its value for good: the groups in it are eternal.
    if (lowering_.isEternal(var)) {
      lowering_.publishLowerer().pinValue(value, var->type);
      if (!global) {
        lowering_.eternalSlots_.insert(address);
      }
    }
  }
}

void StmtLowerer::emitIf(IfStmt *stmt) {
  llvm::Value *cond = lowering_.exprLowerer().emitExpr(stmt->cond);
  llvm::BasicBlock *thenBlock = lowering_.functionFrame().newBlock("if.then");
  llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("if.end");
  llvm::BasicBlock *elseBlock =
      stmt->elseStmt ? lowering_.functionFrame().newBlock("if.else") : endBlock;
  lowering_.builder_.CreateCondBr(cond, thenBlock, elseBlock);

  lowering_.functionFrame().switchTo(thenBlock);
  emitStmt(stmt->thenStmt);
  lowering_.functionFrame().branchTo(endBlock);

  if (stmt->elseStmt) {
    lowering_.functionFrame().switchTo(elseBlock);
    emitStmt(stmt->elseStmt);
    lowering_.functionFrame().branchTo(endBlock);
  }
  lowering_.functionFrame().switchTo(endBlock);
}

void StmtLowerer::emitReturn(ReturnStmt *stmt) {
  // A generator emitted inline finishes, and one emitted as a coroutine
  // gives what it returns to whoever resumed it.
  if (lowering_.inline_) {
    llvm::Value *value = stmt->value ? lowering_.exprLowerer().emitExpr(stmt->value) : nullptr;
    if (value && lowering_.inline_->returnSlot) {
      storeReplacing(lowering_.inline_->returnSlot, value, stmt->value->type);
    } else if (value) {
      lowering_.refCounts().releaseValue(value, stmt->value->type);
    }
    lowering_.functionFrame().emitCleanups(lowering_.inline_->scopeDepth);
    lowering_.builder_.CreateBr(lowering_.inline_->exit);
    lowering_.functionFrame().startDeadBlock();
    return;
  }
  if (lowering_.coro_ && lowering_.currentFn_ && lowering_.currentFn_->isGenerator &&
      lowering_.async_) {
    lowering_.asyncGeneratorLowerer().emitAsyncGeneratorReturn(stmt);
    return;
  }
  if (lowering_.coro_ && lowering_.currentFn_ && lowering_.currentFn_->isGenerator) {
    lowering_.tryLowerer().emitReturnOf(stmt->value ? lowering_.exprLowerer().emitExpr(stmt->value)
                                                    : nullptr);
    return;
  }
  if (lowering_.async_) { // an async function fulfills its promise
    lowering_.asyncLowerer().emitAsyncReturnStmt(stmt);
    return;
  }
  // Not out of a `try` with a `finally`, which must run first.
  if (stmt->value && lowering_.tailGroup_ && lowering_.tailCallLowerer().hasTailCall(stmt->value) &&
      !lowering_.tryLowerer().finallyBetween(~size_t(0))) {
    lowering_.tailCallLowerer().emitTailReturn(stmt->value);
    return;
  }
  // In a function that recurses, the call a return gives (with numbers
  // added to it) is left unchecked: what it threw stays in tz_thrown, and
  // the caller checks. Then LLVM can turn `return f(n - 1) + f(n - 2)` into
  // a loop over the second call, at the price of that frame in the stack
  // of an error.
  const CallExpr *outer = lowering_.returnCall_;
  if (stmt->value && lowering_.currentFn_ && lowering_.recursive_.contains(lowering_.currentFn_) &&
      !lowering_.inline_ && !lowering_.coro_ &&
      !lowering_.tryLowerer().finallyBetween(~size_t(0))) {
    lowering_.returnCall_ = returnedCall(stmt->value);
  }
  emitReturnValue(stmt->value);
  lowering_.returnCall_ = outer;
}

const CallExpr *StmtLowerer::returnedCall(const Expr *value) {
  if (!value->type || !(value->type->isNumeric() || value->type->isBool())) {
    return nullptr;
  }
  if (auto *call = dyn_cast<CallExpr>(value)) {
    return call->builtin == Builtin::None && (call->func || call->method) ? call : nullptr;
  }
  if (auto *conv = dyn_cast<CastExpr>(value)) {
    return conv->isImplicit() && !conv->shortCircuit && conv->operand->type->isNumeric()
               ? returnedCall(conv->operand)
               : nullptr;
  }
  if (auto *unary = dyn_cast<UnaryExpr>(value)) {
    return unary->op == UnaryOp::Neg || unary->op == UnaryOp::Not || unary->op == UnaryOp::BitNot
               ? returnedCall(unary->operand)
               : nullptr;
  }
  auto *bin = dyn_cast<BinaryExpr>(value);
  if (!bin || !bin->lhs->type->isNumeric()) {
    return nullptr;
  }
  switch (bin->op) {
  case BinaryOp::Add:
  case BinaryOp::Sub:
  case BinaryOp::Mul:
  case BinaryOp::BitAnd:
  case BinaryOp::BitOr:
  case BinaryOp::BitXor:
  case BinaryOp::Lt:
  case BinaryOp::Le:
  case BinaryOp::Gt:
  case BinaryOp::Ge:
  case BinaryOp::Eq:
  case BinaryOp::Ne:
    return returnedCall(bin->rhs);
  default:
    return nullptr;
  }
}

// `return value`, or `return` without one (null).
void StmtLowerer::emitReturnValue(Expr *expr) {
  llvm::Value *value = expr ? lowering_.exprLowerer().emitExpr(expr) : nullptr;
  if (value && lowering_.returnType_ && lowering_.returnType_->isVoid() && !expr->type->isVoid()) {
    // A callback whose result is dropped (see FuncSymbol::discardsResult).
    lowering_.refCounts().releaseValue(value, expr->type);
    value = nullptr;
  }
  lowering_.tryLowerer().emitReturnOf(value && !expr->type->isVoid() ? value : nullptr);
}

void StmtLowerer::emitDiscarded(Expr *expr) {
  if (auto *assign = dyn_cast<AssignExpr>(expr)) {
    lowering_.assignLowerer().emitAssign(assign, /*needResult=*/false);
    return;
  }
  if (auto *seq = dyn_cast<SequenceExpr>(expr)) {
    for (Expr *e : seq->exprs) {
      emitDiscarded(e);
    }
    return;
  }
  llvm::Value *value = lowering_.exprLowerer().emitExpr(expr);
  if (expr->type->isManaged()) {
    lowering_.refCounts().releaseValue(value, expr->type);
  }
}

} // namespace tz
