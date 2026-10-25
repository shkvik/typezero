#include "compiler/memory/store_calls/store_calls.h"
#include "compiler/memory/store_exprs/store_exprs.h"
#include "compiler/memory/store_stmts/store_stmts.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>
#include <map>
#include <set>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

StoreCalls::StoreCalls(StoreScanner &scanner) : scanner_(scanner) {}

StoreScanner::Sources StoreCalls::call(const CallExpr *c) {
  scanner_.callee_.insert(c->callee);
  auto *member = llvm::dyn_cast<MemberExpr>(c->callee);
  StoreScanner::Sources receiver;
  if (member) {
    receiver = scanner_.storeExprs().expr(member->object);
  } else if (c->builtin == Builtin::None && !c->func) {
    scanner_.storeExprs().expr(c->callee);
  }
  std::vector<StoreScanner::Sources> args;
  for (const Expr *arg : c->args) {
    args.push_back(scanner_.storeExprs().expr(arg));
  }
  for (size_t i = 0; i < c->args.size(); ++i) {
    if (c->spreads(i)) {
      scanner_.store(args[i]);
    }
  }
  StoreScanner::Sources out;
  if (c->builtin != Builtin::None) {
    return builtin(c, receiver, args);
  }
  auto methodCall = [&](const FunctionDecl *fn, bool virtualCall) {
    auto one = [&](const FunctionDecl *m) {
      scanner_.storeStmts().bindArgs(m, args);
      if (m->thisVar) {
        scanner_.flowInto(scanner_.var(m->thisVar), receiver);
      } else {
        scanner_.store(receiver);
      }
      out.push_back(scanner_.result(m));
    };
    if (fn->impl) { // what a decorator made of it runs instead
      scanner_.store(receiver);
      scanner_.storeStmts().bindArgs(nullptr, args);
      return;
    }
    if (virtualCall) {
      forEachOverride(fn, one);
    } else {
      one(fn);
    }
  };
  if (c->superCall && !member) {
    // `super(args)`: the constructor of the class extended, and the
    // initializers, with this object.
    const FunctionDecl *ctor = c->method ? c->method->decl : nullptr;
    scanner_.storeStmts().bindArgs(ctor, args);
    StoreScanner::Sources self =
        scanner_.current_ && scanner_.current_->thisVar
            ? StoreScanner::Sources{scanner_.var(scanner_.current_->thisVar)}
            : StoreScanner::Sources{};
    for (const ClassDecl *cls =
             scanner_.current_ && scanner_.current_->cls ? scanner_.current_->cls->parent : nullptr;
         cls; cls = cls->parent) {
      if (cls->init && cls->init->thisVar) {
        scanner_.flowInto(scanner_.var(cls->init->thisVar), self);
      }
      for (const FunctionDecl *k : cls->constructors) {
        if (k->thisVar) {
          scanner_.flowInto(scanner_.var(k->thisVar), self);
        }
      }
    }
    if (scanner_.current_ && scanner_.current_->cls && scanner_.current_->cls->init &&
        scanner_.current_->cls->init->thisVar) {
      scanner_.flowInto(scanner_.var(scanner_.current_->cls->init->thisVar), self);
    }
    return out;
  }
  if (c->method) {
    methodCall(c->method->decl, c->virtualCall);
    return out;
  }
  if (!c->unionMethods.empty()) {
    for (const CallExpr::UnionMethod &m : c->unionMethods) {
      if (m.method) {
        methodCall(m.method->decl, m.virtualCall);
      } else {
        scanner_.storeStmts().bindArgs(nullptr, args);
      }
    }
    return out;
  }
  if (c->func) {
    scanner_.storeStmts().bindArgs(c->func->decl, args);
    out.push_back(scanner_.result(c->func->decl));
    return out;
  }
  // Through a function value, or a method of an interface: code not known.
  scanner_.store(receiver);
  scanner_.storeStmts().bindArgs(nullptr, args);

  return out;
}

StoreScanner::Sources StoreCalls::builtin(const CallExpr *c, const StoreScanner::Sources &receiver,
                                          const std::vector<StoreScanner::Sources> &args) {
  StoreScanner::Sources out;
  if (scanner_.isSite_(c)) {
    out.push_back(scanner_.node({StoreScanner::Kind::Site, c}));
  }
  // A callback that gets the receiver: an arrow written there gets it in
  // its third parameter (forEach of an array or a map...); one known only
  // as a value may keep it.
  for (const Expr *arg : c->args) {
    if (!arg->type || !arg->type->nonOptional()->isFunction()) {
      continue;
    }
    auto *fe = llvm::dyn_cast<FunctionExpr>(arg);
    if (!fe) {
      scanner_.store(receiver);
    } else if (fe->fn->params.size() >= 3 && fe->fn->params[2].sym) {
      scanner_.flowInto(scanner_.var(fe->fn->params[2].sym), receiver);
    }
  }
  switch (c->builtin) {
  case Builtin::ArrReverse:
  case Builtin::ArrSort:
  case Builtin::ArrSortWith:
    // The array itself is the result.
    out.insert(out.end(), receiver.begin(), receiver.end());
    break;
  case Builtin::ConsoleLog:
  case Builtin::ConsoleError:
  case Builtin::StringOf:
  case Builtin::ArrPop:
  case Builtin::ArrShift:
  case Builtin::ArrAt:
  case Builtin::ArrSlice:
  case Builtin::ArrIndexOf:
  case Builtin::ArrLastIndexOf:
  case Builtin::ArrIncludes:
  case Builtin::ArrJoin:
  case Builtin::ArrToString:
  case Builtin::ArrForEach:
  case Builtin::ArrFilter:
  case Builtin::ArrFind:
  case Builtin::ArrFindIndex:
  case Builtin::ArrFindLast:
  case Builtin::ArrFindLastIndex:
  case Builtin::ArrSome:
  case Builtin::ArrEvery:
  case Builtin::ObjectKeys:
  case Builtin::ObjectValues:
  case Builtin::ObjectEntries:
  case Builtin::MapGet:
  case Builtin::MapHas:
  case Builtin::MapDelete:
  case Builtin::MapClear:
  case Builtin::MapSize:
  case Builtin::MapForEach:
  case Builtin::JsonStringify:
  case Builtin::ArrayIsArray:
  case Builtin::WeakRefNew:
  case Builtin::WeakRefDeref:
    break;
  case Builtin::ArrPush:
  case Builtin::ArrUnshift:
  case Builtin::ArrSplice:
  case Builtin::ArrFill:
  case Builtin::ArrConcat:
  case Builtin::MapSet:
  case Builtin::SetAdd:
  case Builtin::RecordSet:
  case Builtin::RecordNullishSet:
    // The values go into the receiver (or a new array), which they do
    // not make stored; set() and add() give the receiver back.
    for (const StoreScanner::Sources &a : args) {
      scanner_.store(a);
    }
    if (c->builtin == Builtin::MapSet || c->builtin == Builtin::SetAdd) {
      out.insert(out.end(), receiver.begin(), receiver.end());
    }
    break;
  case Builtin::ArrMap:
  case Builtin::ArrFlatMap:
    // What the callback gives goes into the new array.
    for (const Expr *arg : c->args) {
      if (auto *fe = llvm::dyn_cast<FunctionExpr>(arg)) {
        scanner_.edge(scanner_.stored(), scanner_.result(fe->fn));
      }
    }
    break;
  default:
    scanner_.store(receiver);
    for (const StoreScanner::Sources &a : args) {
      scanner_.store(a);
    }
    break;
  }

  return out;
}

} // namespace tz
