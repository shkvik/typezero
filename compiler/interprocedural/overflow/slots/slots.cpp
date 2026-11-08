#include "compiler/interprocedural/overflow/slots/slots.h"
#include "compiler/interprocedural/overflow/call_ranges/call_ranges.h"
#include "compiler/interprocedural/overflow/evaluator/evaluator.h"
#include "compiler/interprocedural/overflow/executor/executor.h"
#include "compiler/interprocedural/overflow/loop_ranges/loop_ranges.h"
#include "compiler/interprocedural/overflow/narrowing/narrowing.h"
#include "compiler/interprocedural/overflow/overflow_helpers/overflow_helpers.h"
#include "compiler/interprocedural/overflow/overflow_warnings/overflow_warnings.h"
#include "compiler/interprocedural/overflow/program_scan/program_scan.h"
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

namespace {

constexpr unsigned kGrowthsBeforeWidening = 3;

}

Slots::Slots(OverflowCheck &check)
    : check(check) {}

Val Slots::readVar(const VarSymbol *var, const State &st) {
  auto info = this->check.vars_.find(var);
  bool own = info != this->check.vars_.end() && info->second.declared &&
             info->second.owner == this->check.owner && !info->second.writtenElsewhere;
  Val v;
  bool found = false;
  if (own) {
    auto value = st.vars.find(var);
    if (value != st.vars.end()) {
      v = value->second;
      found = true;
    }
  }
  if (!found) {
    std::optional<Val> s = this->check.summary(this->check.cur.vars, var);
    v = s ? this->check.overflowWarnings().fitVal(*s, var->type)
          : this->check.overflowWarnings().trusted(var->type);
  }

  return v;
}

void Slots::writeVar(const VarSymbol *var, Val v, State &st) {
  if (!var || !ValueRange::tracked(var->type)) {
    return;
  }
  v = this->check.overflowWarnings().fitVal(std::move(v), var->type);
  auto info = this->check.vars_.find(var);
  if (info != this->check.vars_.end() && info->second.shared) {
    this->check.contribute(this->check.next.vars[var], v);
  }
  if (info != this->check.vars_.end() && info->second.declared &&
      info->second.owner == this->check.owner && !info->second.writtenElsewhere) {
    st.vars[var] = std::move(v);
  }
}

Val Slots::readField(const Type *objectType, llvm::StringRef field, const Type *type) {
  if (!ValueRange::tracked(type)) {
    return Val();
  }
  if (std::optional<Val> v =
          this->check.summary(this->check.cur.fields, std::make_pair(objectType, field.str()))) {
    return this->check.overflowWarnings().fitVal(*v, type);
  }

  return this->check.overflowWarnings().trusted(type);
}

void Slots::storeField(const Type *objectType, llvm::StringRef field, const Type *type,
                       const Val &v) {
  if (this->check.quiet || !objectType || !ValueRange::tracked(type)) {
    return;
  }
  OverflowCheck::Slot &slot = this->check.next.fields[{objectType, field.str()}];
  slot.type = type;
  this->check.contribute(slot, this->check.overflowWarnings().fitVal(v, type));
}

bool Slots::isPrivateArray(const Expr *expr) const {
  expr = OverflowHelpers::stripCasts(expr);
  if (!expr) {
    return false;
  }
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    if (!name->var()) {
      return false;
    }
    auto info = this->check.vars_.find(name->var());
    return info != this->check.vars_.end() && info->second.declared && !info->second.aliased &&
           !info->second.writtenElsewhere && info->second.owner == this->check.owner;
  }
  if (isa<ArrayLitExpr>(expr) || isa<NewExpr>(expr)) {
    return true;
  }
  if (auto *call = dyn_cast<CallExpr>(expr)) {
    switch (call->builtin()) {
      case Builtin::ArrMap:
      case Builtin::ArrFilter:
      case Builtin::ArrFlatMap:
      case Builtin::ArrSlice:
      case Builtin::ArrConcat:
      case Builtin::ArrayFrom:
      case Builtin::StrSplit:
      case Builtin::StrSplitRegex:
        return true;
      default:
        return false;
    }
  }

  return false;
}

Val Slots::elementsOf(const Expr *arrayExpr, const Val &array, const Type *arrayType) {
  const Type *element = arrayType && arrayType->nonOptional()->isArray()
                            ? arrayType->nonOptional()->inner()
                            : nullptr;
  if (!element) {
    return Val();
  }
  if (!array.elem) {
    return this->check.overflowWarnings().trusted(element);
  }
  Val v = *array.elem;
  if (!isPrivateArray(arrayExpr)) {
    if (std::optional<Val> stored = this->check.summary(this->check.cur.elements, element)) {
      v = ValueRange::join(v, *stored);
    } else if (this->check.round == 0) {
      return this->check.overflowWarnings().trusted(element);
    }
  }

  return this->check.overflowWarnings().fitVal(v, element);
}

void Slots::storeElements(const Expr *arrayExpr, const Type *arrayType, const Val &v, State &st) {
  if (!arrayType || !arrayType->nonOptional()->isArray()) {
    return;
  }
  const Type *element = arrayType->nonOptional()->inner();
  this->check.contribute(this->check.next.elements[element], v);
  auto *name = dyn_cast_or_null<NameExpr>(OverflowHelpers::stripCasts(arrayExpr));
  if (name && name->var()) {
    auto found = st.vars.find(name->var());
    if (found != st.vars.end()) {
      Val &array = found->second;
      array.elem = array.elem ? ValueRange::share(ValueRange::join(*array.elem, v)) : nullptr;
    }
  }
}

const Reason *Slots::paramReason(const FunctionDecl *fn, size_t index) {
  auto found = this->check.next.dangerousCalls.find({fn, index});
  if (found == this->check.next.dangerousCalls.end() || found->second.empty()) {
    return nullptr;
  }
  Reason &r = this->check.reasons.emplace_back();
  const auto &calls = found->second;
  r.via = calls.front().second;
  r.origin = r.via ? r.via->origin : Origin::Input;
  r.loc = calls.front().first;
  for (const auto &call : calls) {
    if (!llvm::is_contained(r.calls, call.first)) {
      r.calls.push_back(call.first);
    }
  }

  return &r;
}

bool Slots::mergeSlot(OverflowCheck::Slot &into, const OverflowCheck::Slot *from, const Type *type,
                      bool narrowing) {
  if (narrowing) {
    if (!from) {
      return false;
    }
    Val v = into.val;
    Val n = from->val;
    if (!v.num.empty() && !n.num.empty()) {
      v.num = ValueRange::meet(v.num, n.num);
    }
    if (!v.len.empty() && !n.len.empty()) {
      v.len = ValueRange::meet(v.len, n.len);
    }
    bool changed = !ValueRange::sameVal(v, into.val);
    into.val = v;
    return changed;
  }
  if (!from) {
    return false;
  }
  Val joined = ValueRange::join(into.val, from->val);
  if (ValueRange::sameVal(joined, into.val)) {
    return false;
  }
  if (++into.growths > kGrowthsBeforeWidening || this->check.round >= kMaxRounds) {
    joined = OverflowHelpers::widenVal(into.val, joined, type);
  }
  into.val = joined;

  return true;
}

}
