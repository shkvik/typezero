#include "compiler/lower/functions/tail_call_lowerer/tail_call_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/truth_lowerer/truth_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

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

}

TailCallLowerer::TailCallLowerer(Lowering &lowering) : lowering_(lowering) {}

void TailCallLowerer::findTailGroups(llvm::ArrayRef<FunctionDecl *> functions) {
  std::vector<std::vector<size_t>> calls(functions.size());
  std::vector<bool> self(functions.size(), false);
  for (size_t i = 0; i < functions.size(); ++i) {
    FunctionDecl *fn = functions[i];
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
    Lowering::TailGroup &group = tailGroups_.emplace_back();
    for (size_t member : cycle) {
      group.members.push_back(functions[member]);
      lowering_.tailGroupOf_[functions[member]] = &group;
    }
    for (FunctionDecl *member : group.members) {
      for (CallExpr *call : tailCallsIn(member)) {
        if (llvm::is_contained(group.members, call->func->decl) &&
            call->func->ret == member->sym->ret) {
          lowering_.tailJumps_.insert(call);
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

void TailCallLowerer::emitTailGroup(Lowering::TailGroup &group) {
  if (group.emitted) {
    return;
  }
  group.emitted = true;
  FunctionDecl *first = group.members.front();
  bool closure = first->isClosure();
  bool merged = group.members.size() > 1;
  llvm::Function *function;
  if (!merged) {
    function = closure ? lowering_.closureLowerer().closureCode(first)
                       : lowering_.functions_.lookup(first->sym);
  } else {
    std::vector<llvm::Type *> params;
    if (closure) {
      params.push_back(lowering_.builder_.getPtrTy());
    }
    params.push_back(lowering_.builder_.getInt32Ty());
    for (FunctionDecl *member : group.members) {
      for (const Type *param : member->sym->params) {
        params.push_back(lowering_.moduleBuilder().lower(param));
      }
    }
    auto *type =
        llvm::FunctionType::get(lowering_.moduleBuilder().lower(first->sym->ret), params, false);
    std::string name = "tz";
    for (FunctionDecl *member : group.members) {
      name += "." + member->name.str();
    }
    function = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, name + ".cycle",
                                      lowering_.module_.get());
    lowering_.functionBuilder().setFunctionAttributes(function);
  }

  lowering_.functionBuilder().beginFunction(function, first);
  unsigned next = 0;
  if (closure) {
    lowering_.envParam_ = function->getArg(next++);
    lowering_.envLowerer().bindCaptures(first);
  }
  llvm::Value *which = merged ? function->getArg(next++) : nullptr;
  group.slots.assign(group.members.size(), {});
  group.starts.clear();
  for (size_t k = 0; k < group.members.size(); ++k) {
    FunctionDecl *member = group.members[k];
    for (const Type *param : member->sym->params) {
      llvm::AllocaInst *slot = lowering_.functionFrame().createStackSlot(
          lowering_.moduleBuilder().lower(param), member->name + ".arg");
      lowering_.builder_.CreateStore(function->getArg(next++), slot);
      group.slots[k].push_back(slot);
    }
    group.starts.push_back(lowering_.functionFrame().newBlock(member->name + ".start"));
  }
  if (merged) {
    llvm::SwitchInst *dispatch =
        lowering_.builder_.CreateSwitch(which, group.starts[0], unsigned(group.members.size()));
    for (size_t k = 1; k < group.members.size(); ++k) {
      dispatch->addCase(lowering_.builder_.getInt32(uint32_t(k)), group.starts[k]);
    }
  } else {
    lowering_.builder_.CreateBr(group.starts[0]);
  }

  for (size_t k = 0; k < group.members.size(); ++k) {
    FunctionDecl *member = group.members[k];
    lowering_.tailGroup_ = &group;
    lowering_.currentFn_ = member;
    lowering_.returnType_ = member->sym->ret;
    lowering_.tailStatements_.clear();
    if (lowering_.returnType_->isVoid()) {
      std::vector<Stmt *> stmts;
      tailStatementsOf(member->body, true, stmts);
      for (Stmt *stmt : stmts) {
        if (tailCall(cast<ExprStmt>(stmt)->expr)) {
          lowering_.tailStatements_.insert(stmt);
        }
      }
    }
    lowering_.functionFrame().switchTo(group.starts[k]);
    std::vector<llvm::Value *> args;
    for (size_t i = 0; i < group.slots[k].size(); ++i) {
      args.push_back(lowering_.builder_.CreateLoad(
          lowering_.moduleBuilder().lower(member->sym->params[i]), group.slots[k][i]));
    }
    lowering_.functionBuilder().emitParamsAndBody(member, args);
    lowering_.functionBuilder().closeBody();
  }
  lowering_.tailGroup_ = nullptr;
  lowering_.tailStatements_.clear();
  lowering_.function_ = nullptr;
  if (!merged) {
    return;
  }

  for (size_t k = 0; k < group.members.size(); ++k) {
    FunctionDecl *member = group.members[k];
    llvm::Function *own = closure ? lowering_.closureLowerer().closureCode(member)
                                  : lowering_.functions_.lookup(member->sym);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(lowering_.ctx_, "entry", own));
    std::vector<llvm::Value *> args;
    unsigned arg = 0;
    if (closure) {
      args.push_back(own->getArg(arg++));
    }
    args.push_back(b.getInt32(uint32_t(k)));
    for (size_t j = 0; j < group.members.size(); ++j) {
      for (const Type *param : group.members[j]->sym->params) {
        args.push_back(j == k ? static_cast<llvm::Value *>(own->getArg(arg++))
                              : llvm::PoisonValue::get(lowering_.moduleBuilder().lower(param)));
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
  if (!call || !call->func || call->builtin != Builtin::None || !lowering_.tailGroup_) {
    return nullptr;
  }
  if (!llvm::is_contained(lowering_.tailGroup_->members, call->func->decl) ||
      call->func->ret != lowering_.returnType_) {
    return nullptr;
  }

  return call;
}

bool TailCallLowerer::hasTailCall(Expr *value) const {
  std::vector<CallExpr *> calls;
  tailCallsOf(value, calls);

  return llvm::any_of(calls, [&](CallExpr *call) { return tailCall(call) != nullptr; });
}

void TailCallLowerer::emitTailReturn(Expr *value) {
  if (CallExpr *call = tailCall(value)) {
    emitTailJump(call);
    return;
  }
  if (!hasTailCall(value)) {
    lowering_.stmtLowerer().emitReturnValue(value);
    return;
  }
  if (auto *cond = dyn_cast<ConditionalExpr>(value)) {
    llvm::Value *test = lowering_.exprLowerer().emitExpr(cond->cond);
    llvm::BasicBlock *thenBlock = lowering_.functionFrame().newBlock("tail.then");
    llvm::BasicBlock *elseBlock = lowering_.functionFrame().newBlock("tail.else");
    lowering_.builder_.CreateCondBr(test, thenBlock, elseBlock);
    lowering_.functionFrame().switchTo(thenBlock);
    emitTailReturn(cond->thenExpr);
    lowering_.builder_.CreateUnreachable();
    lowering_.functionFrame().switchTo(elseBlock);
    emitTailReturn(cond->elseExpr);
    return;
  }
  if (auto *seq = dyn_cast<SequenceExpr>(value)) {
    for (size_t i = 0; i + 1 < seq->exprs.size(); ++i) {
      lowering_.stmtLowerer().emitDiscarded(seq->exprs[i]);
    }
    emitTailReturn(seq->exprs.back());
    return;
  }
  auto *bin = cast<BinaryExpr>(value);
  llvm::BasicBlock *decidedBlock = lowering_.functionFrame().newBlock("tail.decided");
  llvm::BasicBlock *restBlock = lowering_.functionFrame().newBlock("tail.rest");
  bool logical = bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr;
  llvm::Value *lhs = nullptr;
  if (logical) {
    bool isAnd = bin->op == BinaryOp::LogAnd;
    lhs = lowering_.exprLowerer().emitExpr(bin->lhs);
    llvm::Value *truth = lowering_.truthLowerer().emitTruth(lhs, bin->lhs->type);
    lowering_.builder_.CreateCondBr(truth, isAnd ? restBlock : decidedBlock,
                                    isAnd ? decidedBlock : restBlock);
    lowering_.functionFrame().switchTo(decidedBlock);
    llvm::Value *result = lowering_.truthLowerer().decidedValue(bin, lhs);
    lowering_.functionFrame().emitCleanups(0);
    lowering_.builder_.CreateRet(result);
  } else if (bin->lhs->type->isOptional()) {
    lhs = lowering_.exprLowerer().emitExpr(bin->lhs);
    lowering_.builder_.CreateCondBr(lowering_.optionalLowerer().optionalHas(lhs, bin->lhs->type),
                                    decidedBlock, restBlock);
    lowering_.functionFrame().switchTo(decidedBlock);
    llvm::Value *result = bin->type->isOptional()
                              ? lhs
                              : lowering_.optionalLowerer().optionalValue(lhs, bin->lhs->type);
    lowering_.functionFrame().emitCleanups(0);
    lowering_.builder_.CreateRet(result);
  } else {
    lowering_.stmtLowerer().emitReturnValue(value);
    return;
  }
  lowering_.functionFrame().switchTo(restBlock);
  if (logical) {
    lowering_.refCounts().releaseValue(lhs, bin->lhs->type);
  }
  emitTailReturn(bin->rhs);
}

void TailCallLowerer::emitTailJump(CallExpr *call) {
  size_t k = size_t(llvm::find(lowering_.tailGroup_->members, call->func->decl) -
                    lowering_.tailGroup_->members.begin());
  std::vector<llvm::Value *> args = lowering_.exprLowerer().emitExprs(call->args);
  const FuncSymbol *func = call->func;
  for (size_t i = args.size(); i < func->params.size(); ++i) {
    args.push_back(lowering_.optionalLowerer().undefinedValue(func->params[i]));
  }
  lowering_.functionFrame().emitCleanups(0);
  for (size_t i = 0; i < args.size(); ++i) {
    lowering_.builder_.CreateStore(args[i], lowering_.tailGroup_->slots[k][i]);
  }
  lowering_.builder_.CreateBr(lowering_.tailGroup_->starts[k]);
  lowering_.functionFrame().startDeadBlock();
}

}
