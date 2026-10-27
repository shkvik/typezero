#include "compiler/whole_program/flow_calls/flow_calls.h"
#include "compiler/whole_program/flow_builtins/flow_builtins.h"
#include "compiler/whole_program/flow_exprs/flow_exprs.h"
#include "compiler/whole_program/flow_stmts/flow_stmts.h"
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

FlowCalls::FlowCalls(Flows &flows) : flows_(flows) {}

bool FlowCalls::mayCall(const Type *type, const FunctionDecl *fn) const {
  type = type ? type->nonOptional() : nullptr;
  if (!type || !type->isFunction() || !fn->sym) {
    return true;
  }
  llvm::ArrayRef<const Type *> given = type->params();
  const std::vector<const Type *> &takes = fn->sym->params;
  for (size_t i = 0; i < given.size() && i < takes.size(); ++i) {
    const Type *q = given[i], *p = takes[i];
    if (!q || !p || mentionsParam(q) || mentionsParam(p) || q->isUnknown() || p->isUnknown() ||
        q->isError() || p->isError()) {
      continue;
    }
    if ((type->hasRest() && i + 1 == given.size()) || (fn->sym->hasRest && i + 1 == takes.size())) {
      continue;
    }
    if (!flows_.convertible_(q, p)) {
      return false;
    }
  }

  return true;
}

void FlowCalls::functionValue(const FunctionDecl *fn) {
  if (fn && flows_.functionSet_.insert(fn).second) {
    flows_.functionValues_.push_back(fn);
  }
}

Refs FlowCalls::receiverConversion(const Expr *at, const Type *from, const FunctionDecl *method,
                                   const Refs &refs, const VarSymbol *initializes,
                                   std::string role) {
  if (!method || !method->cls || !method->cls->type || !from) {
    return refs;
  }
  const Type *to = method->cls->type;
  from = from->nonOptional();
  if (from == to || !from->isObject()) {
    return refs;
  }
  std::vector<Narrowing> found = flows_.narrowings_(from, to);
  if (found.empty()) {
    return refs;
  }
  int conv = flows_.node(NodeKind::Conversion, at, method->name);
  flows_.conversions.push_back({conv, from, to, at->loc(), std::move(found),
                                initializes ? flows_.var(initializes) : -1, std::move(role),
                                llvm::StringRef()});
  Refs out = refs;
  out.push_back({conv, 0, 0});

  return out;
}

void FlowCalls::passArguments(const CallExpr *call, const FunctionDecl *target,
                              const std::vector<Refs> &args) {
  if (!target) {
    for (const Refs &a : args) {
      flows_.escape(a);
    }
    return;
  }
  size_t count = target->params.size();
  bool rest = count > 0 && target->params.back().rest;
  for (size_t i = 0; i < args.size(); ++i) {
    bool spread = call->spreads(i);
    if (count == 0) {
      flows_.escape(args[i]);
      continue;
    }
    size_t k = std::min(i, count - 1);
    const VarSymbol *param = target->params[k].sym;
    if (!param) {
      continue;
    }
    if (rest && k == count - 1) {
      flows_.flow(spread ? copied(args[i]) : holding(args[i]), flows_.var(param));
    } else if (spread) {
      for (size_t j = k; j < count; ++j) {
        if (const VarSymbol *p = target->params[j].sym) {
          flows_.flow(shifted(args[i], 1), flows_.var(p));
        }
      }
    } else if (i < count) {
      flows_.flow(args[i], flows_.var(param));
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
    if (!done.insert(fn).second) {
      continue;
    }
    if (fn->thisVar) {
      flows_.flow(receiver, flows_.var(fn->thisVar));
    }
    for (const FunctionDecl *o : overridersOf(fn)) {
      todo.push_back(o);
    }
  }
}

std::vector<const FunctionDecl *> FlowCalls::overridersOf(const FunctionDecl *fn) {
  std::vector<const FunctionDecl *> out;
  if (fn->cls) {
    for (const ClassDecl *child : fn->cls->children) {
      for (std::vector<const ClassDecl *> todo{child}; !todo.empty();) {
        const ClassDecl *c = todo.back();
        todo.pop_back();
        for (const FunctionDecl *m : c->methods) {
          if (m->overrides == fn) {
            out.push_back(m);
          }
        }
        todo.insert(todo.end(), c->children.begin(), c->children.end());
      }
    }
  }

  return out;
}

Refs FlowCalls::call(CallExpr *c) {
  if (c->builtin == Builtin::PromiseNew) {
    for (const Expr *arg : c->args) {
      if (auto *f = llvm::dyn_cast<FunctionExpr>(FlowExprs::ownExpr(arg))) {
        for (size_t i = 0; i < f->fn->params.size(); ++i) {
          if (const VarSymbol *p = f->fn->params[i].sym) {
            flows_.runtimeFunctions_.insert(p);
          }
        }
      }
    }
  }
  auto *member = llvm::dyn_cast_or_null<MemberExpr>(c->callee);
  Refs receiver;
  const Type *receiverType = nullptr;
  bool superCtor = false;
  auto *callee = llvm::dyn_cast_or_null<NameExpr>(c->callee);
  if (member) {
    receiver = flows_.flowExprs().expr(member->object);
    receiverType = member->object->type;
  } else if (callee && callee->name == "super") {
    receiver = flows_.flowStmts().name(callee);
    receiverType =
        flows_.flowStmts().currentThis() ? flows_.flowStmts().currentThis()->type : nullptr;
    superCtor = true;
  } else if (!callee || !(callee->func || (callee->var && callee->var->lambda))) {
    flows_.flowExprs().expr(c->callee);
  }
  std::vector<Refs> args;
  for (Expr *arg : c->args) {
    args.push_back(flows_.flowExprs().expr(arg));
  }

  if (c->builtin != Builtin::None) {
    return flows_.flowBuiltins().builtin(c, member, receiver, args);
  }

  const FunctionDecl *target = c->func && c->func->decl       ? c->func->decl
                               : c->method && c->method->decl ? c->method->decl
                                                              : nullptr;
  if (!target) {
    if (callee && callee->var && callee->var->lambda) {
      target = callee->var->lambda;
    }
  }
  if (!target) {
    int result = flows_.node(NodeKind::CallResult, c, "");
    flows_.valueCalls_.push_back({c, args, result});
    if ((callee && callee->var && flows_.runtimeFunctions_.count(callee->var)) ||
        (member && member->name == "resolve")) {
      for (size_t i = 0; i < args.size(); ++i) {
        flows_.escape(args[i]);
        if (const Type *value = c->args[i]->type; value && !value->isVoid()) {
          flows_.flow(args[i], flows_.contents(Type::promise(value)));
        }
      }
    }
    return {{result, 0, 0}};
  }
  passArguments(c, target, args);
  if (target->takesThis() && (member || superCtor)) {
    const VarSymbol *initializes = nullptr;
    std::string role =
        "as 'this' of '" + (target->cls ? target->cls->name.str() + "." : std::string()) +
        (target->isConstructor ? std::string("constructor") : target->name.str()) + "'";
    if (superCtor && target->isConstructor) {
      initializes = target->thisVar;
    }
    Refs self = receiverConversion(c, receiverType, target, receiver, initializes, role);
    passThis(self, target);
  }

  return {{flows_.returned(target), 0, 0}};
}

void FlowCalls::callback(const Expr *arg, const Refs &values) {
  const FunctionDecl *fn = nullptr;
  if (auto *f = llvm::dyn_cast<FunctionExpr>(FlowExprs::ownExpr(arg))) {
    fn = f->fn;
  } else if (auto *n = llvm::dyn_cast<NameExpr>(FlowExprs::ownExpr(arg))) {
    fn = n->func && n->func->decl ? n->func->decl : n->var ? n->var->lambda : nullptr;
  }
  if (!fn) {
    return;
  }
  for (const Param &p : fn->params) {
    if (p.sym) {
      flows_.flow(values, flows_.var(p.sym));
    }
  }
}

}
