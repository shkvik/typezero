#include "compiler/lower/functions/tail_call_lowerer/tail_call_lowerer.h"
#include "compiler/check/checker/checker.h"
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

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

TailCallLowerer::TailCallLowerer(Lowering &owner)
    : lowering(owner) {}

void TailCallLowerer::findTailGroups(llvm::ArrayRef<FunctionDecl *> functions) {
  std::vector<std::vector<size_t>> calls(functions.size());
  std::vector<bool> self(functions.size(), false);
  for (size_t i = 0; i < functions.size(); ++i) {
    FunctionDecl *fn = functions[i];
    if (!fn->body || !fn->sym || !fn->sym->ret || fn->isGenerator || fn->isAsync || fn->impl()) {
      continue;
    }
    for (CallExpr *call : tailCallsIn(fn)) {
      auto target = llvm::find(functions, call->func()->decl);
      if (target == functions.end() || call->func()->ret != fn->sym->ret ||
          call->func()->decl->isGenerator || call->func()->decl->isAsync ||
          call->func()->decl->impl()) {
        continue;
      }
      size_t j = size_t(target - functions.begin());
      self[i] = self[i] || j == i;
      if (!llvm::is_contained(calls[i], j)) {
        calls[i].push_back(j);
      }
    }
  }

  std::vector<int> index(functions.size(), -1);
  std::vector<int> low(functions.size(), 0);
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
    Lowering::TailGroup &group = this->tailGroups.emplace_back();
    for (size_t member : cycle) {
      group.members.push_back(functions[member]);
      this->lowering.tailGroupOf_[functions[member]] = &group;
    }
    for (FunctionDecl *member : group.members) {
      for (CallExpr *call : tailCallsIn(member)) {
        if (llvm::is_contained(group.members, call->func()->decl) &&
            call->func()->ret == member->sym->ret) {
          this->lowering.tailJumps_.insert(call);
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
  if (!this->tailSearched.insert(key).second) {
    return;
  }
  if (closure->group) {
    this->findTailGroups(closure->group->members);
  } else {
    this->findTailGroups(llvm::ArrayRef<FunctionDecl *>(closure));
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
    function = closure ? this->lowering.closureLowerer().closureCode(first)
                       : this->lowering.functions_.lookup(first->sym);
  } else {
    std::vector<llvm::Type *> params;
    if (closure) {
      params.push_back(this->lowering.builder_.getPtrTy());
    }
    params.push_back(this->lowering.builder_.getInt32Ty());
    for (FunctionDecl *member : group.members) {
      for (const Type *param : member->sym->params) {
        params.push_back(this->lowering.moduleBuilder().lower(param));
      }
    }
    auto *type = llvm::FunctionType::get(this->lowering.moduleBuilder().lower(first->sym->ret),
                                         params, false);
    std::string name = "tz";
    for (FunctionDecl *member : group.members) {
      name += "." + member->name.str();
    }
    function = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, name + ".cycle",
                                      this->lowering.module_.get());
    this->lowering.functionBuilder().setFunctionAttributes(function);
  }

  this->lowering.functionBuilder().beginFunction(function, first);
  unsigned next = 0;
  if (closure) {
    this->lowering.envParam_ = function->getArg(next++);
    this->lowering.envLowerer().bindCaptures(first);
  }
  llvm::Value *which = merged ? function->getArg(next++) : nullptr;
  group.slots.assign(group.members.size(), {});
  group.starts.clear();
  for (size_t k = 0; k < group.members.size(); ++k) {
    FunctionDecl *member = group.members[k];
    for (const Type *param : member->sym->params) {
      llvm::AllocaInst *slot = this->lowering.functionFrame().createStackSlot(
          this->lowering.moduleBuilder().lower(param), member->name + ".arg");
      this->lowering.builder_.CreateStore(function->getArg(next++), slot);
      group.slots[k].push_back(slot);
    }
    group.starts.push_back(this->lowering.functionFrame().newBlock(member->name + ".start"));
  }
  if (merged) {
    llvm::SwitchInst *dispatch = this->lowering.builder_.CreateSwitch(
        which, group.starts[0], unsigned(group.members.size()));
    for (size_t k = 1; k < group.members.size(); ++k) {
      dispatch->addCase(this->lowering.builder_.getInt32(uint32_t(k)), group.starts[k]);
    }
  } else {
    this->lowering.builder_.CreateBr(group.starts[0]);
  }

  for (size_t k = 0; k < group.members.size(); ++k) {
    FunctionDecl *member = group.members[k];
    this->lowering.tailGroup_ = &group;
    this->lowering.currentFn_ = member;
    this->lowering.returnType_ = member->sym->ret;
    this->lowering.tailStatements_.clear();
    if (this->lowering.returnType_->isVoid()) {
      std::vector<Stmt *> stmts;
      tailStatementsOf(member->body, true, stmts);
      for (Stmt *stmt : stmts) {
        if (this->tailCall(cast<ExprStmt>(stmt)->expr)) {
          this->lowering.tailStatements_.insert(stmt);
        }
      }
    }
    this->lowering.functionFrame().switchTo(group.starts[k]);
    std::vector<llvm::Value *> args;
    for (size_t i = 0; i < group.slots[k].size(); ++i) {
      args.push_back(this->lowering.builder_.CreateLoad(
          this->lowering.moduleBuilder().lower(member->sym->params[i]), group.slots[k][i]));
    }
    this->lowering.functionBuilder().emitParamsAndBody(member, args);
    this->lowering.functionBuilder().closeBody();
  }
  this->lowering.tailGroup_ = nullptr;
  this->lowering.tailStatements_.clear();
  this->lowering.function_ = nullptr;
  if (!merged) {
    return;
  }

  for (size_t k = 0; k < group.members.size(); ++k) {
    FunctionDecl *member = group.members[k];
    llvm::Function *own = closure ? this->lowering.closureLowerer().closureCode(member)
                                  : this->lowering.functions_.lookup(member->sym);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(this->lowering.ctx_, "entry", own));
    std::vector<llvm::Value *> args;
    unsigned arg = 0;
    if (closure) {
      args.push_back(own->getArg(arg++));
    }
    args.push_back(b.getInt32(uint32_t(k)));
    for (size_t j = 0; j < group.members.size(); ++j) {
      for (const Type *param : group.members[j]->sym->params) {
        args.push_back(j == k
                           ? static_cast<llvm::Value *>(own->getArg(arg++))
                           : llvm::PoisonValue::get(this->lowering.moduleBuilder().lower(param)));
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
  if (!call || !call->func() || call->builtin() != Builtin::None || !this->lowering.tailGroup_) {
    return nullptr;
  }
  if (!llvm::is_contained(this->lowering.tailGroup_->members, call->func()->decl) ||
      call->func()->ret != this->lowering.returnType_) {
    return nullptr;
  }

  return call;
}

bool TailCallLowerer::hasTailCall(Expr *value) const {
  std::vector<CallExpr *> calls;
  tailCallsOf(value, calls);

  return llvm::any_of(calls, [&](CallExpr *call) { return this->tailCall(call) != nullptr; });
}

void TailCallLowerer::emitTailReturn(Expr *value) {
  CallExpr *call = this->tailCall(value);
  if (call) {
    this->emitTailJump(call);
    return;
  }
  if (!this->hasTailCall(value)) {
    this->lowering.stmtLowerer().emitReturnValue(value);
    return;
  }
  auto *cond = dyn_cast<ConditionalExpr>(value);
  if (cond) {
    llvm::Value *test = this->lowering.exprLowerer().emitExpr(cond->cond);
    llvm::BasicBlock *thenBlock = this->lowering.functionFrame().newBlock("tail.then");
    llvm::BasicBlock *elseBlock = this->lowering.functionFrame().newBlock("tail.else");
    this->lowering.builder_.CreateCondBr(test, thenBlock, elseBlock);
    this->lowering.functionFrame().switchTo(thenBlock);
    this->emitTailReturn(cond->thenExpr);
    this->lowering.builder_.CreateUnreachable();
    this->lowering.functionFrame().switchTo(elseBlock);
    this->emitTailReturn(cond->elseExpr);
    return;
  }
  auto *seq = dyn_cast<SequenceExpr>(value);
  if (seq) {
    for (size_t i = 0; i + 1 < seq->exprs.size(); ++i) {
      this->lowering.stmtLowerer().emitDiscarded(seq->exprs[i]);
    }
    this->emitTailReturn(seq->exprs.back());
    return;
  }
  auto *bin = cast<BinaryExpr>(value);
  llvm::BasicBlock *decidedBlock = this->lowering.functionFrame().newBlock("tail.decided");
  llvm::BasicBlock *restBlock = this->lowering.functionFrame().newBlock("tail.rest");
  bool logical = bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr;
  llvm::Value *lhs = nullptr;
  if (logical) {
    bool isAnd = bin->op == BinaryOp::LogAnd;
    lhs = this->lowering.exprLowerer().emitExpr(bin->lhs);
    llvm::Value *truth = this->lowering.truthLowerer().emitTruth(lhs, bin->lhs->type);
    this->lowering.builder_.CreateCondBr(truth, isAnd ? restBlock : decidedBlock,
                                         isAnd ? decidedBlock : restBlock);
    this->lowering.functionFrame().switchTo(decidedBlock);
    llvm::Value *result = this->lowering.truthLowerer().decidedValue(bin, lhs);
    this->lowering.functionFrame().emitCleanups(0);
    this->lowering.builder_.CreateRet(result);
  } else if (bin->lhs->type->isOptional()) {
    lhs = this->lowering.exprLowerer().emitExpr(bin->lhs);
    this->lowering.builder_.CreateCondBr(
        this->lowering.optionalLowerer().optionalHas(lhs, bin->lhs->type), decidedBlock, restBlock);
    this->lowering.functionFrame().switchTo(decidedBlock);
    llvm::Value *result = bin->type->isOptional()
                              ? lhs
                              : this->lowering.optionalLowerer().optionalValue(lhs, bin->lhs->type);
    this->lowering.functionFrame().emitCleanups(0);
    this->lowering.builder_.CreateRet(result);
  } else {
    this->lowering.stmtLowerer().emitReturnValue(value);
    return;
  }
  this->lowering.functionFrame().switchTo(restBlock);
  if (logical) {
    this->lowering.refCounts().releaseValue(lhs, bin->lhs->type);
  }
  this->emitTailReturn(bin->rhs);
}

void TailCallLowerer::emitTailJump(CallExpr *call) {
  size_t k = size_t(llvm::find(this->lowering.tailGroup_->members, call->func()->decl) -
                    this->lowering.tailGroup_->members.begin());
  std::vector<llvm::Value *> args = this->lowering.exprLowerer().emitExprs(call->args);
  const FuncSymbol *func = call->func();
  for (size_t i = args.size(); i < func->params.size(); ++i) {
    args.push_back(this->lowering.optionalLowerer().undefinedValue(func->params[i]));
  }
  this->lowering.functionFrame().emitCleanups(0);
  for (size_t i = 0; i < args.size(); ++i) {
    this->lowering.builder_.CreateStore(args[i], this->lowering.tailGroup_->slots[k][i]);
  }
  this->lowering.builder_.CreateBr(this->lowering.tailGroup_->starts[k]);
  this->lowering.functionFrame().startDeadBlock();
}

}
