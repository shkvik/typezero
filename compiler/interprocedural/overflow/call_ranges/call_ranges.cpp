#include "compiler/interprocedural/overflow/call_ranges/call_ranges.h"
#include "compiler/interprocedural/overflow/builtin_evaluator/builtin_evaluator.h"
#include "compiler/interprocedural/overflow/evaluator/evaluator.h"
#include "compiler/interprocedural/overflow/executor/executor.h"
#include "compiler/interprocedural/overflow/loop_ranges/loop_ranges.h"
#include "compiler/interprocedural/overflow/narrowing/narrowing.h"
#include "compiler/interprocedural/overflow/overflow_helpers/overflow_helpers.h"
#include "compiler/interprocedural/overflow/overflow_warnings/overflow_warnings.h"
#include "compiler/interprocedural/overflow/program_scan/program_scan.h"
#include "compiler/interprocedural/overflow/slots/slots.h"
#include "compiler/interprocedural/overflow/value_range/value_range.h"
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

CallRanges::CallRanges(OverflowCheck &check)
    : check(check) {}

bool CallRanges::mergeSummaries(bool narrowing) {
  bool changed = false;
  this->check.changed.clear();
  auto note = [&](bool grew, const void *map, const void *key, llvm::StringRef field) {
    if (grew) {
      changed = true;
      this->check.changed.insert({map, key, field.str()});
    }
  };
  for (auto &entry : this->check.cur.params) {
    const FunctionDecl *fn = entry.first;
    auto from = this->check.next.params.find(fn);
    for (size_t i = 0; i < entry.second.size(); ++i) {
      const OverflowCheck::Slot *slot =
          from != this->check.next.params.end() && i < from->second.size() ? &from->second[i]
                                                                           : nullptr;
      const Type *type =
          i < fn->params.size() && fn->params[i].sym ? fn->params[i].sym->type : nullptr;
      note(this->check.slots().mergeSlot(entry.second[i], slot, type, narrowing),
           &this->check.cur.params, fn, "");
      Num &num = entry.second[i].val.num;
      if (ValueRange::dangerous(num.worst())) {
        if (const Reason *why = this->check.slots().paramReason(fn, i)) {
          if (ValueRange::dangerous(num.loOrigin)) {
            num.loWhy = why;
          }
          if (ValueRange::dangerous(num.hiOrigin)) {
            num.hiWhy = why;
          }
        }
      }
    }
  }
  for (auto &entry : this->check.cur.rets) {
    auto from = this->check.next.rets.find(entry.first);
    const Type *type = entry.first->sym ? entry.first->sym->getKnownReturnType() : nullptr;
    note(this->check.slots().mergeSlot(
             entry.second, from != this->check.next.rets.end() ? &from->second : nullptr, type,
             narrowing),
         &this->check.cur.rets, entry.first, "");
  }
  for (auto &entry : this->check.cur.vars) {
    auto from = this->check.next.vars.find(entry.first);
    note(this->check.slots().mergeSlot(
             entry.second, from != this->check.next.vars.end() ? &from->second : nullptr,
             entry.first->type, narrowing),
         &this->check.cur.vars, entry.first, "");
  }
  for (auto &entry : this->check.cur.fields) {
    auto from = this->check.next.fields.find(entry.first);
    const OverflowCheck::Slot *slot =
        from != this->check.next.fields.end() ? &from->second : nullptr;
    note(this->check.slots().mergeSlot(entry.second, slot, slot ? slot->type : entry.second.type,
                                       narrowing),
         &this->check.cur.fields, entry.first.first, entry.first.second);
  }
  for (auto &entry : this->check.cur.elements) {
    auto from = this->check.next.elements.find(entry.first);
    note(this->check.slots().mergeSlot(
             entry.second, from != this->check.next.elements.end() ? &from->second : nullptr,
             entry.first, narrowing),
         &this->check.cur.elements, entry.first, "");
  }
  for (auto &entry : this->check.cur.dicts) {
    auto from = this->check.next.dicts.find(entry.first);
    const Type *value = entry.first->valueType();
    note(this->check.slots().mergeSlot(
             entry.second, from != this->check.next.dicts.end() ? &from->second : nullptr, value,
             narrowing),
         &this->check.cur.dicts, entry.first, "");
  }
  if (!narrowing) {
    for (auto &entry : this->check.next.preds) {
      OverflowCheck::Pred &into = this->check.cur.preds[entry.first];
      const OverflowCheck::Pred &from = entry.second;
      bool grew = false;
      auto merge = [&](std::vector<Num> &a, const std::vector<Num> &b) {
        if (a.size() < b.size()) {
          a.resize(b.size());
        }
        for (size_t i = 0; i < b.size(); ++i) {
          Num joined = ValueRange::join(a[i], b[i]);
          if (!ValueRange::sameNum(joined, a[i])) {
            a[i] = joined;
            grew = true;
          }
        }
      };
      merge(into.whenTrue, from.whenTrue);
      merge(into.whenFalse, from.whenFalse);
      note(grew, &this->check.cur.preds, entry.first, "");
    }
  }

  return changed;
}

void CallRanges::analyzeAll() {
  this->check.work = 0;
  this->check.owner = nullptr;
  this->check.frames.assign(1, OverflowCheck::Frame());
  this->check.targets.clear();
  this->check.reading = nullptr;
  this->check.loopRanges().forget();
  State st;
  for (const Stmt *item : this->check.program_.items) {
    if (isa<FunctionDecl>(item)) {
      continue;
    }
    this->check.executor().exec(item, st);
  }
  this->check.memos.resize(this->check.functions_.size());
  for (size_t i = 0; i < this->check.functions_.size(); ++i) {
    OverflowCheck::FunctionMemo &memo = this->check.memos[i];
    if (memo.valid && !this->check.reporting && !this->check.readsChanged(memo)) {
      this->check.work += memo.work;
      OverflowCheck::joinInto(this->check.next, memo.made);
      continue;
    }
    OverflowCheck::Summaries others = std::move(this->check.next);
    this->check.next = OverflowCheck::Summaries();
    memo.reads.clear();
    this->check.reading = &memo;
    uint64_t before = this->check.work;
    analyzeFunction(this->check.functions_[i]);
    this->check.reading = nullptr;
    memo.work = this->check.work - before;
    memo.made = std::move(this->check.next);
    memo.valid = this->check.round >= 1;
    this->check.next = std::move(others);
    OverflowCheck::joinInto(this->check.next, memo.made);
  }
}

Val CallRanges::paramValue(const FunctionDecl *fn, size_t index) {
  const Param &p = fn->params[index];
  const Type *type = p.sym->type;
  if (this->check.round == 0) {
    return this->check.overflowWarnings().trusted(type);
  }
  Val v;
  bool known = false;
  this->check.noteRead(&this->check.cur.params, fn);
  auto found = this->check.cur.params.find(fn);
  if (found != this->check.cur.params.end() && index < found->second.size()) {
    v = found->second[index].val;
    known = true;
  }
  if (!known || this->check.unknownCallers.contains(fn)) {
    v = ValueRange::join(v, this->check.overflowWarnings().trusted(type));
  }

  return this->check.overflowWarnings().fitVal(v, type);
}

void CallRanges::analyzeFunction(const FunctionDecl *fn) {
  this->check.owner = fn;
  this->check.frames.assign(1, OverflowCheck::Frame());
  this->check.frames.back().fn = fn;
  this->check.targets.clear();
  this->check.loopRanges().forget();
  State st;
  for (size_t i = 0; i < fn->params.size(); ++i) {
    const Param &p = fn->params[i];
    if (!p.sym || !ValueRange::tracked(p.sym->type)) {
      continue;
    }
    Val v = paramValue(fn, i);
    if (p.defaultValue()) {
      v = ValueRange::join(v, this->check.evaluator().eval(p.defaultValue(), st));
    }
    this->check.slots().writeVar(p.sym, v, st);
  }
  this->check.executor().exec(fn->body, st);
}

void CallRanges::passArgs(const FunctionDecl *fn, const CallExpr *call, llvm::ArrayRef<Val> args) {
  if (this->check.quiet || !fn) {
    return;
  }
  std::vector<OverflowCheck::Slot> &slots = this->check.next.params[fn];
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
    if (p.rest()) {
      Val element;
      for (size_t k = i; k < args.size(); ++k) {
        element = ValueRange::join(element, call && call->spreads(k) && args[k].elem ? *args[k].elem
                                                                                     : args[k]);
      }
      v.len = spread ? ValueRange::someLength()
                     : ValueRange::exactly(double(args.size() > i ? args.size() - i : 0));
      v.elem = ValueRange::share(element);
    } else if (i < args.size() && !spread) {
      v = args[i];
    } else if (spread) {
      v = this->check.overflowWarnings().trusted(p.sym->type);
    } else {
      continue;
    }
    slots[i].val = ValueRange::join(slots[i].val, v);
    if (ValueRange::dangerous(v.num.worst()) && call) {
      this->check.next.dangerousCalls[{fn, i}].push_back(
          {call->loc(), ValueRange::dangerous(v.num.hiOrigin) ? v.num.hiWhy : v.num.loWhy});
    }
  }
}

Val CallRanges::returned(const FunctionDecl *fn, const Type *type) {
  if (std::optional<Val> v = this->check.summary(this->check.cur.rets, fn)) {
    return this->check.overflowWarnings().fitVal(*v, type);
  }

  return this->check.overflowWarnings().trusted(type);
}

void CallRanges::calledUserCode(State &st) {
  for (auto &entry : st.vars) {
    auto info = this->check.vars_.find(entry.first);
    if (info != this->check.vars_.end() && info->second.aliased && !entry.second.len.empty()) {
      entry.second.len = ValueRange::someLength();
    }
  }
}

Val CallRanges::evalCall(const CallExpr *call, State &st) {
  if (call->builtin() != Builtin::None) {
    return this->check.builtinEvaluator().evalBuiltin(call, st);
  }
  auto *member = dyn_cast<MemberExpr>(call->callee);
  if (member) {
    this->check.evaluator().eval(member->object, st);
  } else if (!call->func()) {
    this->check.evaluator().eval(call->callee, st);
  }
  std::vector<Val> args;
  for (const Expr *arg : call->args) {
    args.push_back(this->check.evaluator().eval(arg, st));
  }
  const FunctionDecl *target = call->func()     ? call->func()->decl
                               : call->method() ? call->method()->decl
                                                : nullptr;
  if (target) {
    passArgs(target, call, args);
    this->check.loopRanges().noteCall(call, target, args);
  }
  for (const UnionMethod &m : call->unionMethods()) {
    if (m.method && m.method->decl) {
      passArgs(m.method->decl, call, args);
    }
  }
  calledUserCode(st);
  if (!target || call->virtualCall() || !call->unionMethods().empty()) {
    return this->check.overflowWarnings().trusted(call->type);
  }

  return returned(target, call->type);
}

Val CallRanges::evalNew(const NewExpr *ne, State &st) {
  std::vector<Val> args;
  const auto &argExprs = ne->ctor() ? ne->ctor()->args : ne->args;
  for (const Expr *arg : argExprs) {
    args.push_back(this->check.evaluator().eval(arg, st));
  }
  if (ne->cls()) {
    if (ne->ctor() && ne->ctor()->func() && ne->ctor()->func()->decl) {
      passArgs(ne->ctor()->func()->decl, ne->ctor(), args);
    }
    calledUserCode(st);
    return Val();
  }
  const Type *type = ne->type ? ne->type->nonOptional() : nullptr;
  if (type && type->isArray()) {
    Val v;
    v.len = args.empty() || args[0].num.empty() ? ValueRange::exactly(0)
                                                : ValueRange::narrowTo(args[0].num, 0, kLengthMax);
    const Type *element = type->inner();
    v.elem = ValueRange::share(
        ValueRange::isNumberType(element) ? ValueRange::numVal(ValueRange::exactly(0))
        : ValueRange::hasLength(element)  ? ValueRange::lengthVal(ValueRange::exactly(0))
                                          : Val());
    return v;
  }
  if (type && type->isDict() && !args.empty() && ValueRange::isNumberType(type->valueType())) {
    this->check.contribute(this->check.next.dicts[type],
                           this->check.overflowWarnings().trusted(type->valueType()));
  }

  return Val();
}

Val CallRanges::runCallback(const FunctionDecl *fn, llvm::ArrayRef<Val> args, size_t first,
                            State &st) {
  OverflowCheck::Frame frame;
  frame.fn = fn;
  frame.callback = true;
  frame.returned = ValueRange::deadState();
  frame.targets = this->check.targets.size();
  frame.quiet = this->check.quiet;
  this->check.frames.push_back(std::move(frame));
  for (size_t i = first; i < fn->params.size(); ++i) {
    const VarSymbol *p = fn->params[i].sym;
    if (!p || !ValueRange::tracked(p->type)) {
      continue;
    }
    this->check.slots().writeVar(
        p,
        i - first < args.size() ? args[i - first] : this->check.overflowWarnings().trusted(p->type),
        st);
  }
  this->check.executor().exec(fn->body, st);
  OverflowCheck::Frame done = std::move(this->check.frames.back());
  this->check.frames.pop_back();
  st = ValueRange::join(st, done.returned);

  return done.result;
}

Val CallRanges::evalCallback(const CallExpr *call, State &st) {
  auto *member = dyn_cast<MemberExpr>(call->callee);
  const Expr *receiver = member ? member->object : nullptr;
  size_t at = *OverflowHelpers::callbackArg(call->builtin());
  if (call->builtin() == Builtin::ArrayFrom) {
    receiver = call->args.empty() ? nullptr : call->args[0];
  }
  Val array = receiver ? this->check.evaluator().eval(receiver, st) : Val();
  const Type *arrayType = receiver && receiver->type ? receiver->type->nonOptional() : nullptr;
  Val init;
  bool hasInit = false;
  for (size_t i = 0; i < call->args.size(); ++i) {
    if (i == at || (call->builtin() == Builtin::ArrayFrom && i == 0)) {
      continue;
    }
    Val v = this->check.evaluator().eval(call->args[i], st);
    if ((call->builtin() == Builtin::ArrReduce || call->builtin() == Builtin::ArrReduceRight) &&
        i == 1) {
      init = v;
      hasInit = true;
    }
  }
  Val element;
  Num passes;
  if (call->builtin() == Builtin::MapForEach) {
    const Type *value = arrayType && arrayType->isDict() ? arrayType->valueType() : nullptr;
    if (std::optional<Val> v = this->check.summary(this->check.cur.dicts, arrayType)) {
      element = this->check.overflowWarnings().fitVal(*v, value);
    } else {
      element = this->check.overflowWarnings().trusted(value);
    }
    passes = ValueRange::someLength();
  } else if (arrayType && arrayType->isArray()) {
    element = this->check.slots().elementsOf(receiver, array, arrayType);
    passes = array.len.empty() ? ValueRange::someLength() : array.len;
  } else if (arrayType && arrayType->isString()) {
    element = ValueRange::lengthVal(ValueRange::range(1, 2));
    passes = array.len;
  } else {
    element = this->check.overflowWarnings().trusted(nullptr);
  }
  Val indexVal = ValueRange::numVal(
      passes.empty()
          ? ValueRange::make({0}, {kLengthMax - 1, Origin::Trusted})
          : ValueRange::make({0}, {std::max(passes.hi - 1, 0.0), passes.hiOrigin, passes.hiWhy}));
  const Expr *arg = at < call->args.size() ? OverflowHelpers::stripCasts(call->args[at]) : nullptr;
  auto *fe = dyn_cast_or_null<FunctionExpr>(arg);
  if (!fe || !this->check.inlined.contains(fe->fn)) {
    const FunctionDecl *named = nullptr;
    if (auto *name = dyn_cast_or_null<NameExpr>(arg)) {
      if (name->func()) {
        named = name->func()->decl;
      } else if (name->var()) {
        named = name->var()->function() ? name->var()->function() : name->var()->lambda();
      }
    }
    if (arg) {
      this->check.evaluator().eval(call->args[at], st);
    }
    if (named) {
      passArgs(named, nullptr, {element, indexVal, array});
    }
    calledUserCode(st);
    Val result =
        named ? returned(named, named->sym ? named->sym->getKnownReturnType() : nullptr) : Val();
    if (call->builtin() == Builtin::ArrMap || call->builtin() == Builtin::ArrayFrom) {
      Val v;
      v.len = passes.empty() ? ValueRange::someLength() : passes;
      v.elem = ValueRange::share(
          named ? result
                : this->check.overflowWarnings().trusted(
                      call->type && call->type->isArray() ? call->type->inner() : nullptr));
      return v;
    }
    return this->check.overflowWarnings().trusted(call->type);
  }
  const FunctionDecl *fn = fe->fn;
  bool reduce = call->builtin() == Builtin::ArrReduce || call->builtin() == Builtin::ArrReduceRight;
  const VarSymbol *acc = reduce && !fn->params.empty() ? fn->params[0].sym : nullptr;
  if (acc && ValueRange::tracked(acc->type)) {
    this->check.slots().writeVar(acc, hasInit ? init : element, st);
  }
  Val result;
  OverflowCheck::LoopSpec spec;
  spec.loc = call->loc();
  spec.exitAtHead = true;
  spec.passes = passes.empty() ? passes : ValueRange::add(passes, ValueRange::exactly(1));
  spec.simple = !OverflowHelpers::containsLoop(fn->body);
  spec.body = [&, fn, acc](State &in) {
    std::vector<Val> args;
    size_t first = 0;
    if (call->builtin() == Builtin::ArrSortWith) {
      args = {element, element};
    } else if (call->builtin() == Builtin::MapForEach) {
      args = {element,
              this->check.overflowWarnings().trusted(
                  fn->params.size() > 1 && fn->params[1].sym ? fn->params[1].sym->type : nullptr),
              Val()};
    } else {
      args = {element, indexVal, array};
    }
    if (acc) {
      first = 1;
    }
    Val r = runCallback(fn, args, first, in);
    if (acc && ValueRange::tracked(acc->type)) {
      this->check.slots().writeVar(acc, r, in);
    }
    result = r;
  };
  this->check.loopRanges().runLoop(spec, st);
  switch (call->builtin()) {
    case Builtin::ArrMap:
    case Builtin::ArrayFrom: {
      Val v;
      v.len = passes.empty() ? ValueRange::someLength() : passes;
      v.elem = ValueRange::share(result);
      return v;
    }
    case Builtin::ArrFilter: {
      Val v;
      v.len = passes.empty() ? ValueRange::someLength() : ValueRange::make({0}, passes.high());
      v.elem = ValueRange::share(element);
      return v;
    }
    case Builtin::ArrFlatMap: {
      Val v;
      v.len = ValueRange::someLength();
      v.elem = result.elem
                   ? result.elem
                   : ValueRange::share(this->check.overflowWarnings().trusted(
                         call->type && call->type->isArray() ? call->type->inner() : nullptr));
      return v;
    }
    case Builtin::ArrReduce:
    case Builtin::ArrReduceRight: {
      Val v = acc ? this->check.slots().readVar(acc, st)
                  : this->check.overflowWarnings().trusted(call->type);
      if (acc) {
        st.vars.erase(acc);
      }
      return this->check.overflowWarnings().fitVal(v, call->type);
    }
    case Builtin::ArrFind:
    case Builtin::ArrFindLast:
      return this->check.overflowWarnings().fitVal(element, call->type);
    case Builtin::ArrFindIndex:
    case Builtin::ArrFindLastIndex:
      return ValueRange::numVal(passes.empty()
                                    ? ValueRange::make({-1}, {kLengthMax - 1, Origin::Trusted})
                                    : ValueRange::make({-1}, {std::max(passes.hi - 1, -1.0),
                                                              passes.hiOrigin, passes.hiWhy}));
    case Builtin::ArrSortWith:
      return array;
    default:
      return Val();
  }
}

}
