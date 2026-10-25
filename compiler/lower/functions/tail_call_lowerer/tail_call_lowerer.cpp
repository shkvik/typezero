#include "compiler/lower/functions/tail_call_lowerer/tail_call_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/truth_lowerer/truth_lowerer.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/errors/unwind_lowerer/unwind_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

// Calls `visit` on the value of every `return` in `stmt`, outside the
// functions declared in it.
void forEachReturn(Stmt *stmt, llvm::function_ref<void(Expr *)> visit) {
  if (!stmt) {
    return;
  }
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    for (Stmt *s : cast<BlockStmt>(stmt)->body) {
      forEachReturn(s, visit);
    }
    break;
  case Stmt::Kind::If:
    forEachReturn(cast<IfStmt>(stmt)->thenStmt, visit);
    forEachReturn(cast<IfStmt>(stmt)->elseStmt, visit);
    break;
  case Stmt::Kind::While:
    forEachReturn(cast<WhileStmt>(stmt)->body, visit);
    break;
  case Stmt::Kind::DoWhile:
    forEachReturn(cast<DoWhileStmt>(stmt)->body, visit);
    break;
  case Stmt::Kind::For:
    forEachReturn(cast<ForStmt>(stmt)->body, visit);
    break;
  case Stmt::Kind::ForOf:
    forEachReturn(cast<ForOfStmt>(stmt)->body, visit);
    break;
  case Stmt::Kind::Switch:
    for (SwitchCase &c : cast<SwitchStmt>(stmt)->cases) {
      for (Stmt *s : c.body) {
        forEachReturn(s, visit);
      }
    }
    break;
  case Stmt::Kind::Labeled:
    forEachReturn(cast<LabeledStmt>(stmt)->body, visit);
    break;
  case Stmt::Kind::Return:
    if (Expr *value = cast<ReturnStmt>(stmt)->value) {
      visit(value);
    }
    break;
  default:
    break;
  }
}

// The direct calls `value` returns as they are: itself, the branches of
// `?:`, and the right side of `&&`, `||` and `??`.
void tailCallsOf(Expr *value, std::vector<CallExpr *> &calls) {
  if (auto *call = dyn_cast<CallExpr>(value)) {
    if (call->func && call->builtin == Builtin::None) {
      calls.push_back(call);
    }
  } else if (auto *cond = dyn_cast<ConditionalExpr>(value)) {
    tailCallsOf(cond->thenExpr, calls);
    tailCallsOf(cond->elseExpr, calls);
  } else if (auto *bin = dyn_cast<BinaryExpr>(value)) {
    if (bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr || bin->op == BinaryOp::Nullish) {
      tailCallsOf(bin->rhs, calls);
    }
  } else if (auto *seq = dyn_cast<SequenceExpr>(value)) {
    tailCallsOf(seq->exprs.back(), calls);
  }
}

// In a function that returns nothing, the statements that are calls after
// which the function ends: the last statement of the body, or one right
// before a `return`, also in the branches of an `if` there.
void tailStatementsOf(Stmt *stmt, bool tail, std::vector<Stmt *> &stmts) {
  if (auto *block = dyn_cast<BlockStmt>(stmt)) {
    for (size_t i = 0; i < block->body.size(); ++i) {
      bool beforeReturn = i + 1 < block->body.size() && isa<ReturnStmt>(block->body[i + 1]) &&
                          !cast<ReturnStmt>(block->body[i + 1])->value;
      tailStatementsOf(block->body[i], beforeReturn || (tail && i + 1 == block->body.size()),
                       stmts);
    }
  } else if (auto *ifStmt = dyn_cast<IfStmt>(stmt)) {
    tailStatementsOf(ifStmt->thenStmt, tail, stmts);
    if (ifStmt->elseStmt) {
      tailStatementsOf(ifStmt->elseStmt, tail, stmts);
    }
  } else if (auto *expr = dyn_cast<ExprStmt>(stmt); expr && tail) {
    auto *call = dyn_cast<CallExpr>(expr->expr);
    if (call && call->func && call->builtin == Builtin::None) {
      stmts.push_back(stmt);
    }
  }
}

// The calls in tail position of `fn`.
std::vector<CallExpr *> tailCallsIn(FunctionDecl *fn) {
  std::vector<CallExpr *> calls;
  forEachReturn(fn->body, [&](Expr *value) { tailCallsOf(value, calls); });
  if (fn->sym->ret->isVoid()) {
    std::vector<Stmt *> stmts;
    tailStatementsOf(fn->body, true, stmts);
    for (Stmt *stmt : stmts) {
      calls.push_back(cast<CallExpr>(cast<ExprStmt>(stmt)->expr));
    }
  }

  return calls;
}

} // namespace

TailCallLowerer::TailCallLowerer(CodeGen &codegen) : codegen_(codegen) {}

void TailCallLowerer::findTailGroups(llvm::ArrayRef<FunctionDecl *> functions) {
  // The tail calls between them, to a function with the same return type:
  // converting the result would run after the call.
  std::vector<std::vector<size_t>> calls(functions.size());
  std::vector<bool> self(functions.size(), false);
  for (size_t i = 0; i < functions.size(); ++i) {
    FunctionDecl *fn = functions[i];
    // A generator's `return` gives a value to whoever iterates it, and
    // calling one runs none of its code; an async function's settles its
    // promise. A method a decorator may replace is called through its stub.
    if (!fn->body || !fn->sym || !fn->sym->ret || fn->isGenerator || fn->isAsync || fn->impl) {
      continue;
    }
    for (CallExpr *call : tailCallsIn(fn)) {
      auto target = llvm::find(functions, call->func->decl);
      if (target == functions.end() || call->func->ret != fn->sym->ret ||
          call->func->decl->isGenerator || call->func->decl->isAsync || call->func->decl->impl) {
        continue;
      }
      size_t j = size_t(target - functions.begin());
      self[i] = self[i] || j == i;
      if (!llvm::is_contained(calls[i], j)) {
        calls[i].push_back(j);
      }
    }
  }

  // Tarjan's algorithm finds the cycles.
  std::vector<int> index(functions.size(), -1), low(functions.size(), 0);
  std::vector<bool> onStack(functions.size(), false);
  std::vector<size_t> stack;
  int next = 0;
  std::function<void(size_t)> visit = [&](size_t v) {
    index[v] = low[v] = next++;
    stack.push_back(v);
    onStack[v] = true;
    for (size_t w : calls[v]) {
      if (index[w] < 0) {
        visit(w);
        low[v] = std::min(low[v], low[w]);
      } else if (onStack[w]) {
        low[v] = std::min(low[v], index[w]);
      }
    }
    if (low[v] != index[v]) {
      return;
    }
    std::vector<size_t> cycle;
    size_t w;
    do {
      w = stack.back();
      stack.pop_back();
      onStack[w] = false;
      cycle.push_back(w);
    } while (w != v);
    if (cycle.size() == 1 && !self[v]) {
      return;
    }
    llvm::sort(cycle);
    CodeGen::TailGroup &group = tailGroups_.emplace_back();
    for (size_t member : cycle) {
      group.members.push_back(functions[member]);
      codegen_.tailGroupOf_[functions[member]] = &group;
    }
    // The tail calls between them are jumps, which do not grow the stack
    // (see CodeGen::analyzeThrows).
    for (FunctionDecl *member : group.members) {
      for (CallExpr *call : tailCallsIn(member)) {
        if (llvm::is_contained(group.members, call->func->decl) &&
            call->func->ret == member->sym->ret) {
          codegen_.tailJumps_.insert(call);
        }
      }
    }
  };
  for (size_t v = 0; v < functions.size(); ++v) {
    if (index[v] < 0) {
      visit(v);
    }
  }
}

void TailCallLowerer::findTailGroups(FunctionDecl *closure) {
  const void *key = closure->group ? static_cast<const void *>(closure->group) : closure;
  if (!tailSearched_.insert(key).second) {
    return;
  }
  if (closure->group) {
    findTailGroups(closure->group->members);
  } else {
    findTailGroups(llvm::ArrayRef<FunctionDecl *>(closure));
  }
}

void TailCallLowerer::emitTailGroup(CodeGen::TailGroup &group) {
  if (group.emitted) {
    return;
  }
  group.emitted = true;
  FunctionDecl *first = group.members.front();
  bool closure = first->isClosure();
  bool merged = group.members.size() > 1;
  llvm::Function *function;
  if (!merged) {
    function = closure ? codegen_.closureLowerer().closureCode(first) : codegen_.functions_.lookup(first->sym);
  } else {
    // (env, which, the parameters of every member) -> the return type.
    std::vector<llvm::Type *> params;
    if (closure) {
      params.push_back(codegen_.builder_.getPtrTy());
    }
    params.push_back(codegen_.builder_.getInt32Ty());
    for (FunctionDecl *member : group.members) {
      for (const Type *param : member->sym->params) {
        params.push_back(codegen_.moduleBuilder().lower(param));
      }
    }
    auto *type = llvm::FunctionType::get(codegen_.moduleBuilder().lower(first->sym->ret), params, false);
    std::string name = "tz";
    for (FunctionDecl *member : group.members) {
      name += "." + member->name.str();
    }
    function = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, name + ".cycle",
                                      codegen_.module_.get());
    codegen_.functionBuilder().setFunctionAttributes(function);
  }

  codegen_.functionBuilder().beginFunction(function, first);
  unsigned next = 0;
  if (closure) {
    codegen_.envParam_ = function->getArg(next++);
    codegen_.envLowerer().bindCaptures(first);
  }
  llvm::Value *which = merged ? function->getArg(next++) : nullptr;
  // The arguments live in slots, which a tail call fills before it jumps.
  group.slots.assign(group.members.size(), {});
  group.starts.clear();
  for (size_t k = 0; k < group.members.size(); ++k) {
    FunctionDecl *member = group.members[k];
    for (const Type *param : member->sym->params) {
      llvm::AllocaInst *slot = codegen_.functionFrame().createStackSlot(codegen_.moduleBuilder().lower(param), member->name + ".arg");
      codegen_.builder_.CreateStore(function->getArg(next++), slot);
      group.slots[k].push_back(slot);
    }
    group.starts.push_back(codegen_.functionFrame().newBlock(member->name + ".start"));
  }
  if (merged) {
    llvm::SwitchInst *dispatch =
        codegen_.builder_.CreateSwitch(which, group.starts[0], unsigned(group.members.size()));
    for (size_t k = 1; k < group.members.size(); ++k) {
      dispatch->addCase(codegen_.builder_.getInt32(uint32_t(k)), group.starts[k]);
    }
  } else {
    codegen_.builder_.CreateBr(group.starts[0]);
  }

  for (size_t k = 0; k < group.members.size(); ++k) {
    FunctionDecl *member = group.members[k];
    codegen_.tailGroup_ = &group;
    codegen_.currentFn_ = member;
    codegen_.returnType_ = member->sym->ret;
    codegen_.tailStatements_.clear();
    if (codegen_.returnType_->isVoid()) {
      std::vector<Stmt *> stmts;
      tailStatementsOf(member->body, true, stmts);
      for (Stmt *stmt : stmts) {
        if (tailCall(cast<ExprStmt>(stmt)->expr)) {
          codegen_.tailStatements_.insert(stmt);
        }
      }
    }
    codegen_.functionFrame().switchTo(group.starts[k]);
    std::vector<llvm::Value *> args;
    for (size_t i = 0; i < group.slots[k].size(); ++i) {
      args.push_back(codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(member->sym->params[i]), group.slots[k][i]));
    }
    codegen_.functionBuilder().emitParamsAndBody(member, args);
    codegen_.functionBuilder().closeBody();
  }
  codegen_.unwindLowerer().finishFunctionUnwind();
  codegen_.tailGroup_ = nullptr;
  codegen_.tailStatements_.clear();
  codegen_.function_ = nullptr;
  if (!merged) {
    return;
  }

  // Each member's own function calls the cycle's.
  for (size_t k = 0; k < group.members.size(); ++k) {
    FunctionDecl *member = group.members[k];
    llvm::Function *own = closure ? codegen_.closureLowerer().closureCode(member) : codegen_.functions_.lookup(member->sym);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(codegen_.ctx_, "entry", own));
    std::vector<llvm::Value *> args;
    unsigned arg = 0;
    if (closure) {
      args.push_back(own->getArg(arg++));
    }
    args.push_back(b.getInt32(uint32_t(k)));
    for (size_t j = 0; j < group.members.size(); ++j) {
      for (const Type *param : group.members[j]->sym->params) {
        args.push_back(j == k ? static_cast<llvm::Value *>(own->getArg(arg++))
                              : llvm::PoisonValue::get(codegen_.moduleBuilder().lower(param)));
      }
    }
    llvm::CallInst *result = b.CreateCall(function, args);
    if (result->getType()->isVoidTy()) {
      b.CreateRetVoid();
    } else {
      b.CreateRet(result);
    }
  }
}

CallExpr *TailCallLowerer::tailCall(Expr *value) const {
  auto *call = dyn_cast<CallExpr>(value);
  if (!call || !call->func || call->builtin != Builtin::None || !codegen_.tailGroup_) {
    return nullptr;
  }
  if (!llvm::is_contained(codegen_.tailGroup_->members, call->func->decl) ||
      call->func->ret != codegen_.returnType_) {
    return nullptr;
  }

  return call;
}

bool TailCallLowerer::hasTailCall(Expr *value) const {
  std::vector<CallExpr *> calls;
  tailCallsOf(value, calls);

  return llvm::any_of(calls, [&](CallExpr *call) { return tailCall(call) != nullptr; });
}

// `return value` where a tail call is among what value may give: the parts
// that decide which are evaluated, then each way returns, or jumps.
void TailCallLowerer::emitTailReturn(Expr *value) {
  if (CallExpr *call = tailCall(value)) {
    emitTailJump(call);
    return;
  }
  if (!hasTailCall(value)) {
    codegen_.stmtLowerer().emitReturnValue(value);
    return;
  }
  if (auto *cond = dyn_cast<ConditionalExpr>(value)) {
    llvm::Value *test = codegen_.exprLowerer().emitExpr(cond->cond);
    llvm::BasicBlock *thenBlock = codegen_.functionFrame().newBlock("tail.then");
    llvm::BasicBlock *elseBlock = codegen_.functionFrame().newBlock("tail.else");
    codegen_.builder_.CreateCondBr(test, thenBlock, elseBlock);
    codegen_.functionFrame().switchTo(thenBlock);
    emitTailReturn(cond->thenExpr);
    codegen_.builder_.CreateUnreachable(); // ends the dead block after the return
    codegen_.functionFrame().switchTo(elseBlock);
    emitTailReturn(cond->elseExpr);
    return;
  }
  if (auto *seq = dyn_cast<SequenceExpr>(value)) {
    for (size_t i = 0; i + 1 < seq->exprs.size(); ++i) {
      codegen_.stmtLowerer().emitDiscarded(seq->exprs[i]);
    }
    emitTailReturn(seq->exprs.back());
    return;
  }
  auto *bin = cast<BinaryExpr>(value);
  llvm::BasicBlock *decidedBlock = codegen_.functionFrame().newBlock("tail.decided");
  llvm::BasicBlock *restBlock = codegen_.functionFrame().newBlock("tail.rest");
  bool logical = bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr;
  llvm::Value *lhs = nullptr;
  if (logical) {
    // The left side decides when falsy for `&&`, truthy for `||`, and is
    // then the result.
    bool isAnd = bin->op == BinaryOp::LogAnd;
    lhs = codegen_.exprLowerer().emitExpr(bin->lhs);
    llvm::Value *truth = codegen_.truthLowerer().emitTruth(lhs, bin->lhs->type);
    codegen_.builder_.CreateCondBr(truth, isAnd ? restBlock : decidedBlock,
                          isAnd ? decidedBlock : restBlock);
    codegen_.functionFrame().switchTo(decidedBlock);
    llvm::Value *result = codegen_.truthLowerer().decidedValue(bin, lhs);
    codegen_.functionFrame().emitCleanups(0);
    codegen_.builder_.CreateRet(result);
  } else if (bin->lhs->type->isOptional()) {
    // `a ?? f(x)`: a when it holds a value.
    lhs = codegen_.exprLowerer().emitExpr(bin->lhs);
    codegen_.builder_.CreateCondBr(codegen_.optionalLowerer().optionalHas(lhs, bin->lhs->type), decidedBlock, restBlock);
    codegen_.functionFrame().switchTo(decidedBlock);
    llvm::Value *result = bin->type->isOptional() ? lhs : codegen_.optionalLowerer().optionalValue(lhs, bin->lhs->type);
    codegen_.functionFrame().emitCleanups(0);
    codegen_.builder_.CreateRet(result);
  } else {
    // The left side is never undefined.
    codegen_.stmtLowerer().emitReturnValue(value);
    return;
  }
  codegen_.functionFrame().switchTo(restBlock);
  if (logical) { // undefined after `??` holds nothing to release
    codegen_.refCounts().releaseValue(lhs, bin->lhs->type);
  }
  emitTailReturn(bin->rhs);
}

void TailCallLowerer::emitTailJump(CallExpr *call) {
  size_t k =
      size_t(llvm::find(codegen_.tailGroup_->members, call->func->decl) - codegen_.tailGroup_->members.begin());
  // The callee owns its arguments, as for any call; then this call is done
  // with its own variables.
  std::vector<llvm::Value *> args = codegen_.throwLowerer().emitHeldValues(call->args);
  const FuncSymbol *func = call->func;
  for (size_t i = args.size(); i < func->params.size(); ++i) {
    args.push_back(codegen_.optionalLowerer().undefinedValue(func->params[i]));
  }
  codegen_.functionFrame().emitCleanups(0);
  for (size_t i = 0; i < args.size(); ++i) {
    codegen_.builder_.CreateStore(args[i], codegen_.tailGroup_->slots[k][i]);
  }
  codegen_.builder_.CreateBr(codegen_.tailGroup_->starts[k]);
  codegen_.functionFrame().startDeadBlock();
}

} // namespace tz
