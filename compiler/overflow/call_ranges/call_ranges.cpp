#include "compiler/overflow/call_ranges/call_ranges.h"
#include "compiler/check/checker/checker.h"
#include "compiler/check/facts/facts.h"
#include "compiler/overflow/builtin_evaluator/builtin_evaluator.h"
#include "compiler/overflow/evaluator/evaluator.h"
#include "compiler/overflow/executor/executor.h"
#include "compiler/overflow/loop_ranges/loop_ranges.h"
#include "compiler/overflow/narrowing/narrowing.h"
#include "compiler/overflow/overflow_helpers/overflow_helpers.h"
#include "compiler/overflow/overflow_warnings/overflow_warnings.h"
#include "compiler/overflow/program_scan/program_scan.h"
#include "compiler/overflow/slots/slots.h"
#include "compiler/overflow/value_range/value_range.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

CallRanges::CallRanges(OverflowCheck &check) : check_(check) {}

bool CallRanges::mergeSummaries(bool narrowing) {
  bool changed = false;
  for (auto &entry : check_.cur_.params) {
    const FunctionDecl *fn = entry.first;
    auto from = check_.next_.params.find(fn);
    for (size_t i = 0; i < entry.second.size(); ++i) {
      const OverflowCheck::Slot *slot =
          from != check_.next_.params.end() && i < from->second.size() ? &from->second[i] : nullptr;
      const Type *type =
          i < fn->params.size() && fn->params[i].sym ? fn->params[i].sym->type : nullptr;
      changed |= check_.slots().mergeSlot(entry.second[i], slot, type, narrowing);
      Num &num = entry.second[i].val.num;
      if (dangerous(num.worst())) {
        if (const Reason *why = check_.slots().paramReason(fn, i)) {
          if (dangerous(num.loOrigin)) {
            num.loWhy = why;
          }
          if (dangerous(num.hiOrigin)) {
            num.hiWhy = why;
          }
        }
      }
    }
  }
  for (auto &entry : check_.cur_.rets) {
    auto from = check_.next_.rets.find(entry.first);
    const Type *type = entry.first->sym ? entry.first->sym->ret : nullptr;
    changed |= check_.slots().mergeSlot(
        entry.second, from != check_.next_.rets.end() ? &from->second : nullptr, type, narrowing);
  }
  for (auto &entry : check_.cur_.vars) {
    auto from = check_.next_.vars.find(entry.first);
    changed |= check_.slots().mergeSlot(entry.second,
                                        from != check_.next_.vars.end() ? &from->second : nullptr,
                                        entry.first->type, narrowing);
  }
  for (auto &entry : check_.cur_.fields) {
    auto from = check_.next_.fields.find(entry.first);
    const OverflowCheck::Slot *slot = from != check_.next_.fields.end() ? &from->second : nullptr;
    changed |= check_.slots().mergeSlot(entry.second, slot, slot ? slot->type : entry.second.type,
                                        narrowing);
  }
  for (auto &entry : check_.cur_.elements) {
    auto from = check_.next_.elements.find(entry.first);
    changed |= check_.slots().mergeSlot(
        entry.second, from != check_.next_.elements.end() ? &from->second : nullptr, entry.first,
        narrowing);
  }
  for (auto &entry : check_.cur_.dicts) {
    auto from = check_.next_.dicts.find(entry.first);
    const Type *value = entry.first->valueType();
    changed |= check_.slots().mergeSlot(
        entry.second, from != check_.next_.dicts.end() ? &from->second : nullptr, value, narrowing);
  }
  if (!narrowing) {
    for (auto &entry : check_.next_.preds) {
      OverflowCheck::Pred &into = check_.cur_.preds[entry.first];
      const OverflowCheck::Pred &from = entry.second;
      auto merge = [&](std::vector<Num> &a, const std::vector<Num> &b) {
        if (a.size() < b.size()) {
          a.resize(b.size());
        }
        for (size_t i = 0; i < b.size(); ++i) {
          Num joined = join(a[i], b[i]);
          if (!sameNum(joined, a[i])) {
            a[i] = joined;
            changed = true;
          }
        }
      };
      merge(into.whenTrue, from.whenTrue);
      merge(into.whenFalse, from.whenFalse);
    }
  }

  return changed;
}

void CallRanges::analyzeAll() {
  check_.work_ = 0;
  check_.owner_ = nullptr;
  check_.frames_.assign(1, OverflowCheck::Frame());
  check_.targets_.clear();
  check_.tries_.clear();
  State st;
  for (const Stmt *item : check_.program_.items) {
    if (isa<FunctionDecl>(item)) {
      continue;
    }
    check_.executor().exec(item, st);
  }
  for (const FunctionDecl *fn : check_.functions_) {
    analyzeFunction(fn);
  }
}

Val CallRanges::paramValue(const FunctionDecl *fn, size_t index) {
  const Param &p = fn->params[index];
  const Type *type = p.sym->type;
  if (check_.round_ == 0) {
    return check_.overflowWarnings().trusted(type);
  }
  Val v;
  bool known = false;
  auto found = check_.cur_.params.find(fn);
  if (found != check_.cur_.params.end() && index < found->second.size()) {
    v = found->second[index].val;
    known = true;
  }
  if (!known || check_.unknownCallers_.contains(fn)) {
    v = join(v, check_.overflowWarnings().trusted(type));
  }

  return check_.overflowWarnings().fitVal(v, type);
}

void CallRanges::analyzeFunction(const FunctionDecl *fn) {
  check_.owner_ = fn;
  check_.frames_.assign(1, OverflowCheck::Frame());
  check_.frames_.back().fn = fn;
  check_.targets_.clear();
  check_.tries_.clear();
  State st;
  for (size_t i = 0; i < fn->params.size(); ++i) {
    const Param &p = fn->params[i];
    if (!p.sym || !tracked(p.sym->type)) {
      continue;
    }
    Val v = paramValue(fn, i);
    if (p.defaultValue) {
      v = join(v, check_.evaluator().eval(p.defaultValue, st));
    }
    check_.slots().writeVar(p.sym, v, st);
  }
  check_.executor().exec(fn->body, st);
}

// Adds what a call passes to the summaries of the parameters.
void CallRanges::passArgs(const FunctionDecl *fn, const CallExpr *call, llvm::ArrayRef<Val> args) {
  if (check_.quiet_ || !fn) {
    return;
  }
  std::vector<OverflowCheck::Slot> &slots = check_.next_.params[fn];
  if (slots.size() < fn->params.size()) {
    slots.resize(fn->params.size());
  }
  bool spread = call && call->hasSpread();
  for (size_t i = 0; i < fn->params.size(); ++i) {
    const Param &p = fn->params[i];
    if (!p.sym) {
      continue;
    }
    Val v;
    if (p.rest) {
      // The arguments from here on, as one array.
      Val element;
      for (size_t k = i; k < args.size(); ++k) {
        element = join(element, call && call->spreads(k) && args[k].elem ? *args[k].elem : args[k]);
      }
      v.len = spread ? someLength() : exactly(double(args.size() > i ? args.size() - i : 0));
      v.elem = share(element);
    } else if (i < args.size() && !spread) {
      v = args[i];
    } else if (spread) {
      v = check_.overflowWarnings().trusted(p.sym->type);
    } else {
      continue; // left out: the default value, which the function itself gives
    }
    slots[i].val = join(slots[i].val, v);
    if (dangerous(v.num.worst()) && call) {
      check_.next_.dangerousCalls[{fn, i}].push_back(
          {call->loc(), dangerous(v.num.hiOrigin) ? v.num.hiWhy : v.num.loWhy});
    }
  }
}

Val CallRanges::returned(const FunctionDecl *fn, const Type *type) {
  if (std::optional<Val> v = check_.summary(check_.cur_.rets, fn)) {
    return check_.overflowWarnings().fitVal(*v, type);
  }

  return check_.overflowWarnings().trusted(type);
}

// User code may change the length of any array that has other names.
void CallRanges::calledUserCode(State &st) {
  for (auto &entry : st.vars) {
    auto info = check_.vars_.find(entry.first);
    if (info != check_.vars_.end() && info->second.aliased && !entry.second.len.empty()) {
      entry.second.len = someLength();
    }
  }
  check_.mayThrow(st);
}

Val CallRanges::evalCall(const CallExpr *call, State &st) {
  if (call->builtin != Builtin::None) {
    return check_.builtinEvaluator().evalBuiltin(call, st);
  }
  auto *member = dyn_cast<MemberExpr>(call->callee);
  if (member) {
    check_.evaluator().eval(member->object, st);
  } else if (!call->func) {
    check_.evaluator().eval(call->callee, st);
  }
  std::vector<Val> args;
  for (const Expr *arg : call->args) {
    args.push_back(check_.evaluator().eval(arg, st));
  }
  const FunctionDecl *target = call->func     ? call->func->decl
                               : call->method ? call->method->decl
                                              : nullptr;
  if (target) {
    passArgs(target, call, args);
  }
  for (const CallExpr::UnionMethod &m : call->unionMethods) {
    if (m.method && m.method->decl) {
      passArgs(m.method->decl, call, args);
    }
  }
  calledUserCode(st);
  if (!target || call->virtualCall || !call->unionMethods.empty()) {
    return check_.overflowWarnings().trusted(call->type);
  }

  return returned(target, call->type);
}

Val CallRanges::evalNew(const NewExpr *ne, State &st) {
  std::vector<Val> args;
  const auto &argExprs = ne->ctor ? ne->ctor->args : ne->args;
  for (const Expr *arg : argExprs) {
    args.push_back(check_.evaluator().eval(arg, st));
  }
  if (ne->cls) {
    if (ne->ctor && ne->ctor->func && ne->ctor->func->decl) {
      passArgs(ne->ctor->func->decl, ne->ctor, args);
    }
    calledUserCode(st);
    return Val();
  }
  const Type *type = ne->type ? ne->type->nonOptional() : nullptr;
  if (type && type->isArray()) {
    Val v;
    v.len = args.empty() || args[0].num.empty() ? exactly(0) : narrowTo(args[0].num, 0, kLengthMax);
    const Type *element = type->inner();
    v.elem = share(isNumberType(element) ? numVal(exactly(0))
                   : hasLength(element)  ? lengthVal(exactly(0))
                                         : Val());
    return v;
  }
  if (type && type->isDict() && !args.empty() && isNumberType(type->valueType())) {
    check_.contribute(check_.next_.dicts[type],
                      check_.overflowWarnings().trusted(type->valueType()));
  }

  return Val();
}

Val CallRanges::runCallback(const FunctionDecl *fn, llvm::ArrayRef<Val> args, size_t first,
                            State &st) {
  OverflowCheck::Frame frame;
  frame.fn = fn;
  frame.callback = true;
  frame.returned = deadState();
  frame.targets = check_.targets_.size();
  frame.quiet = check_.quiet_;
  check_.frames_.push_back(std::move(frame));
  for (size_t i = first; i < fn->params.size(); ++i) {
    const VarSymbol *p = fn->params[i].sym;
    if (!p || !tracked(p->type)) {
      continue;
    }
    check_.slots().writeVar(
        p, i - first < args.size() ? args[i - first] : check_.overflowWarnings().trusted(p->type),
        st);
  }
  check_.executor().exec(fn->body, st);
  OverflowCheck::Frame done = std::move(check_.frames_.back());
  check_.frames_.pop_back();
  st = join(st, done.returned);

  return done.result;
}

Val CallRanges::evalCallback(const CallExpr *call, State &st) {
  auto *member = dyn_cast<MemberExpr>(call->callee);
  const Expr *receiver = member ? member->object : nullptr;
  size_t at = *callbackArg(call->builtin);
  if (call->builtin == Builtin::ArrayFrom) {
    receiver = call->args.empty() ? nullptr : call->args[0];
  }
  Val array = receiver ? check_.evaluator().eval(receiver, st) : Val();
  const Type *arrayType = receiver && receiver->type ? receiver->type->nonOptional() : nullptr;
  Val init;
  bool hasInit = false;
  for (size_t i = 0; i < call->args.size(); ++i) {
    if (i == at || (call->builtin == Builtin::ArrayFrom && i == 0)) {
      continue;
    }
    Val v = check_.evaluator().eval(call->args[i], st);
    if ((call->builtin == Builtin::ArrReduce || call->builtin == Builtin::ArrReduceRight) &&
        i == 1) {
      init = v;
      hasInit = true;
    }
  }
  Val element;
  Num passes;
  if (call->builtin == Builtin::MapForEach) {
    const Type *value = arrayType && arrayType->isDict() ? arrayType->valueType() : nullptr;
    if (std::optional<Val> v = check_.summary(check_.cur_.dicts, arrayType)) {
      element = check_.overflowWarnings().fitVal(*v, value);
    } else {
      element = check_.overflowWarnings().trusted(value);
    }
    passes = someLength();
  } else if (arrayType && arrayType->isArray()) {
    element = check_.slots().elementsOf(receiver, array, arrayType);
    passes = array.len.empty() ? someLength() : array.len;
  } else if (arrayType && arrayType->isString()) {
    element = lengthVal(range(1, 2));
    passes = array.len;
  } else {
    element = check_.overflowWarnings().trusted(nullptr);
  }
  Val indexVal = numVal(
      passes.empty() ? make({0}, {kLengthMax - 1, Origin::Trusted})
                     : make({0}, {std::max(passes.hi - 1, 0.0), passes.hiOrigin, passes.hiWhy}));
  const Expr *arg = at < call->args.size() ? stripCasts(call->args[at]) : nullptr;
  auto *fe = dyn_cast_or_null<FunctionExpr>(arg);
  if (!fe || !check_.inlined_.contains(fe->fn)) {
    // A function by name, called for each element.
    const FunctionDecl *named = nullptr;
    if (auto *name = dyn_cast_or_null<NameExpr>(arg)) {
      if (name->func) {
        named = name->func->decl;
      } else if (name->var) {
        named = name->var->function ? name->var->function : name->var->lambda;
      }
    }
    if (arg) {
      check_.evaluator().eval(call->args[at], st);
    }
    if (named) {
      passArgs(named, nullptr, {element, indexVal, array});
    }
    calledUserCode(st);
    Val result = named ? returned(named, named->sym ? named->sym->ret : nullptr) : Val();
    if (call->builtin == Builtin::ArrMap || call->builtin == Builtin::ArrayFrom) {
      Val v;
      v.len = passes.empty() ? someLength() : passes;
      v.elem =
          share(named ? result
                      : check_.overflowWarnings().trusted(
                            call->type && call->type->isArray() ? call->type->inner() : nullptr));
      return v;
    }
    return check_.overflowWarnings().trusted(call->type);
  }
  const FunctionDecl *fn = fe->fn;
  bool reduce = call->builtin == Builtin::ArrReduce || call->builtin == Builtin::ArrReduceRight;
  const VarSymbol *acc = reduce && !fn->params.empty() ? fn->params[0].sym : nullptr;
  if (acc && tracked(acc->type)) {
    check_.slots().writeVar(acc, hasInit ? init : element, st);
  }
  Val result;
  OverflowCheck::LoopSpec spec;
  spec.loc = call->loc();
  spec.exitAtHead = true;
  spec.passes = passes.empty() ? passes : add(passes, exactly(1));
  spec.simple = !containsLoop(fn->body);
  spec.body = [&, fn, acc](State &in) {
    std::vector<Val> args;
    size_t first = 0;
    if (call->builtin == Builtin::ArrSortWith) {
      args = {element, element};
    } else if (call->builtin == Builtin::MapForEach) {
      args = {element,
              check_.overflowWarnings().trusted(
                  fn->params.size() > 1 && fn->params[1].sym ? fn->params[1].sym->type : nullptr),
              Val()};
    } else {
      args = {element, indexVal, array};
    }
    if (acc) {
      first = 1;
    }
    Val r = runCallback(fn, args, first, in);
    if (acc && tracked(acc->type)) {
      check_.slots().writeVar(acc, r, in);
    }
    result = r;
  };
  check_.loopRanges().runLoop(spec, st);
  switch (call->builtin) {
  case Builtin::ArrMap:
  case Builtin::ArrayFrom: {
    Val v;
    v.len = passes.empty() ? someLength() : passes;
    v.elem = share(result);
    return v;
  }
  case Builtin::ArrFilter: {
    Val v;
    v.len = passes.empty() ? someLength() : make({0}, passes.high());
    v.elem = share(element);
    return v;
  }
  case Builtin::ArrFlatMap: {
    Val v;
    v.len = someLength();
    v.elem = result.elem
                 ? result.elem
                 : share(check_.overflowWarnings().trusted(
                       call->type && call->type->isArray() ? call->type->inner() : nullptr));
    return v;
  }
  case Builtin::ArrReduce:
  case Builtin::ArrReduceRight: {
    Val v = acc ? check_.slots().readVar(acc, st) : check_.overflowWarnings().trusted(call->type);
    if (acc) {
      st.vars.erase(acc);
    }
    return check_.overflowWarnings().fitVal(v, call->type);
  }
  case Builtin::ArrFind:
  case Builtin::ArrFindLast:
    return check_.overflowWarnings().fitVal(element, call->type);
  case Builtin::ArrFindIndex:
  case Builtin::ArrFindLastIndex:
    return numVal(passes.empty()
                      ? make({-1}, {kLengthMax - 1, Origin::Trusted})
                      : make({-1}, {std::max(passes.hi - 1, -1.0), passes.hiOrigin, passes.hiWhy}));
  case Builtin::ArrSortWith:
    return array;
  default:
    return Val();
  }
}

} // namespace tz
