#include "compiler/interprocedural/wide_writes/flow_graph/flow_calls/flow_calls.h"
#include "compiler/interprocedural/wide_writes/flow_graph/flow_builtins/flow_builtins.h"
#include "compiler/interprocedural/wide_writes/flow_graph/flow_exprs/flow_exprs.h"
#include "compiler/interprocedural/wide_writes/flow_graph/flow_stmts/flow_stmts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <tuple>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

FlowCalls::FlowCalls(FlowGraphBuilder &owner)
    : builder(owner) {}

bool FlowCalls::mayCall(const Type *type, const FunctionDecl *fn) const {
  type = type ? type->nonOptional() : nullptr;
  if (!type || !type->isFunction() || !fn->sym) {
    return true;
  }
  llvm::ArrayRef<const Type *> given = type->params();
  const std::vector<const Type *> &takes = fn->sym->params;
  for (size_t i = 0; i < given.size() && i < takes.size(); ++i) {
    const Type *q = given[i], *p = takes[i];
    if (!q || !p || TypeQueries::mentionsParam(q) || TypeQueries::mentionsParam(p) ||
        q->isUnknown() || p->isUnknown() || q->isError() || p->isError()) {
      continue;
    }
    if ((type->hasRest() && i + 1 == given.size()) || (fn->sym->hasRest && i + 1 == takes.size())) {
      continue;
    }
    if (!this->builder.isConvertible(q, p)) {
      return false;
    }
  }

  return true;
}

Refs FlowCalls::receiverConversion(const Expr *at, const Type *from, const FunctionDecl *method,
                                   const Refs &refs, const VarSymbol *initializes,
                                   std::string role) {
  if (!method || !method->cls() || !method->cls()->type || !from) {
    return refs;
  }
  const Type *to = method->cls()->type;
  from = from->nonOptional();
  if (from == to || !from->isObject()) {
    return refs;
  }
  std::vector<NarrowedPart> found = this->builder.findNarrowings(from, to);
  if (found.empty()) {
    return refs;
  }
  int conv = this->builder.findNode(NodeKind::Conversion, at, method->name);
  this->builder.addConversion({conv, from, to, at->loc(), std::move(found),
                               initializes ? this->builder.varNode(initializes) : -1,
                               std::move(role), llvm::StringRef()});
  Refs out = refs;
  out.push_back({conv, 0, 0});

  return out;
}

void FlowCalls::passArguments(const CallExpr *call, const FunctionDecl *target,
                              const std::vector<Refs> &args) {
  if (!target) {
    for (const Refs &a : args) {
      this->builder.escape(a);
    }
    return;
  }
  size_t count = target->params.size();
  bool hasRest = count > 0 && target->params.back().rest();
  for (size_t i = 0; i < args.size(); ++i) {
    bool isSpread = call->spreads(i);
    if (count == 0) {
      this->builder.escape(args[i]);
      continue;
    }
    size_t k = std::min(i, count - 1);
    const VarSymbol *param = target->params[k].sym;
    if (!param) {
      continue;
    }
    if (hasRest && k == count - 1) {
      this->builder.addFlow(isSpread ? FlowGraphBuilder::copied(args[i])
                                     : FlowGraphBuilder::holding(args[i]),
                            this->builder.varNode(param));
    } else if (isSpread) {
      for (size_t j = k; j < count; ++j) {
        if (const VarSymbol *p = target->params[j].sym) {
          this->builder.addFlow(FlowGraphBuilder::shifted(args[i], 1), this->builder.varNode(p));
        }
      }
    } else if (i < count) {
      this->builder.addFlow(args[i], this->builder.varNode(param));
    }
  }
}

void FlowCalls::passThis(const Refs &receiver, const FunctionDecl *method) {
  if (!method) {
    return;
  }
  std::vector<const FunctionDecl *> todo{method};
  std::set<const FunctionDecl *> done;
  while (!todo.empty()) {
    const FunctionDecl *fn = todo.back();
    todo.pop_back();
    auto [_, isFirstVisit] = done.insert(fn);
    if (!isFirstVisit) {
      continue;
    }
    if (fn->thisVar()) {
      this->builder.addFlow(receiver, this->builder.varNode(fn->thisVar()));
    }
    for (const FunctionDecl *o : this->overridersOf(fn)) {
      todo.push_back(o);
    }
  }
}

std::vector<const FunctionDecl *> FlowCalls::overridersOf(const FunctionDecl *fn) {
  std::vector<const FunctionDecl *> out;
  if (fn->cls()) {
    for (const ClassDecl *child : fn->cls()->children) {
      for (std::vector<const ClassDecl *> todo{child}; !todo.empty();) {
        const ClassDecl *c = todo.back();
        todo.pop_back();
        for (const FunctionDecl *m : c->methods) {
          if (m->overrides() == fn) {
            out.push_back(m);
          }
        }
        todo.insert(todo.end(), c->children.begin(), c->children.end());
      }
    }
  }

  return out;
}

std::vector<FlowRun::Step> FlowCalls::callParts(CallExpr *c, FlowRun &run) {
  if (c->builtin() == Builtin::PromiseNew) {
    this->markRuntimeParams(c);
  }
  auto *member = llvm::dyn_cast_or_null<MemberExpr>(c->callee);
  auto *callee = llvm::dyn_cast_or_null<NameExpr>(c->callee);
  auto parts = std::vector<FlowRun::Step>();
  if (member) {
    parts.push_back(FlowRun::WalkExpr{member->object});
  } else if (callee && callee->name == "super") {
    run.answers.push_back(this->builder.getFlowStmts().name(callee));
  } else {
    run.answers.push_back(Refs());
    bool isKnownFunction = callee && (callee->func() || (callee->var() && callee->var()->lambda()));
    if (!isKnownFunction) {
      parts.push_back(FlowRun::WalkExpr{c->callee});
      parts.push_back(FlowRun::DropAnswer{});
    }
  }
  for (Expr *arg : c->args) {
    parts.push_back(FlowRun::WalkExpr{arg});
  }
  parts.push_back(FlowRun::FinishCall{c});

  return parts;
}

void FlowCalls::markRuntimeParams(const CallExpr *c) {
  for (const Expr *arg : c->args) {
    auto *f = llvm::dyn_cast<FunctionExpr>(this->builder.getFlowExprs().ownExpr(arg));
    if (!f) {
      continue;
    }
    for (size_t i = 0; i < f->fn->params.size(); ++i) {
      if (const VarSymbol *p = f->fn->params[i].sym) {
        this->builder.markRuntimeFunction(p);
      }
    }
  }
}

void FlowCalls::finishCall(CallExpr *c, FlowRun &run) {
  std::vector<Refs> args = this->builder.takeAnswers(run, c->args.size());
  Refs receiver = this->builder.takeAnswer(run);
  auto *member = llvm::dyn_cast_or_null<MemberExpr>(c->callee);
  auto *callee = llvm::dyn_cast_or_null<NameExpr>(c->callee);
  bool isSuperCall = !member && callee && callee->name == "super";
  if (c->builtin() != Builtin::None) {
    Refs result = this->builder.getFlowBuiltins().builtin(c, member, receiver, args);
    return this->builder.pushAnswer(run, c, result);
  }

  const FunctionDecl *target = c->func() && c->func()->decl       ? c->func()->decl
                               : c->method() && c->method()->decl ? c->method()->decl
                                                                  : nullptr;
  if (!target) {
    if (callee && callee->var() && callee->var()->lambda()) {
      target = callee->var()->lambda();
    }
  }
  if (!target) {
    Refs result = this->valueCallResult(c, member, callee, args);
    return this->builder.pushAnswer(run, c, result);
  }
  this->passArguments(c, target, args);
  if (target->takesThis() && (member || isSuperCall)) {
    const Type *receiverType = nullptr;
    if (member) {
      receiverType = member->object->type;
    } else {
      const VarSymbol *self = this->builder.findCurrentThis();
      receiverType = self ? self->type : nullptr;
    }
    const VarSymbol *initializes = nullptr;
    std::string role =
        "as 'this' of '" + (target->cls() ? target->cls()->name.str() + "." : std::string()) +
        (target->isConstructor() ? std::string("constructor") : target->name.str()) + "'";
    if (isSuperCall && target->isConstructor()) {
      initializes = target->thisVar();
    }
    Refs self = this->receiverConversion(c, receiverType, target, receiver, initializes, role);
    this->passThis(self, target);
  }
  Refs result = {{this->builder.returnNode(target), 0, 0}};
  this->builder.pushAnswer(run, c, result);
}

Refs FlowCalls::valueCallResult(const CallExpr *c, const MemberExpr *member, const NameExpr *callee,
                                const std::vector<Refs> &args) {
  int result = this->builder.findNode(NodeKind::CallResult, c, "");
  this->builder.rememberValueCall({c, args, result});
  if ((callee && callee->var() && this->builder.isRuntimeFunction(callee->var())) ||
      (member && member->name == "resolve")) {
    for (size_t i = 0; i < args.size(); ++i) {
      this->builder.escape(args[i]);
      if (const Type *value = c->args[i]->type; value && !value->isVoid()) {
        this->builder.addFlow(args[i], this->builder.contentsNode(Type::promise(value)));
      }
    }
  }

  return {{result, 0, 0}};
}

void FlowCalls::callback(const Expr *arg, const Refs &values) {
  const FunctionDecl *fn = nullptr;
  const Expr *own = this->builder.getFlowExprs().ownExpr(arg);
  if (auto *f = llvm::dyn_cast<FunctionExpr>(own)) {
    fn = f->fn;
  } else if (auto *n = llvm::dyn_cast<NameExpr>(own)) {
    fn = n->func() && n->func()->decl ? n->func()->decl : n->var() ? n->var()->lambda() : nullptr;
  }
  if (!fn) {
    return;
  }
  for (const Param &p : fn->params) {
    if (p.sym) {
      this->builder.addFlow(values, this->builder.varNode(p.sym));
    }
  }
}

}
