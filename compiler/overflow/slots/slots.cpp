#include "compiler/overflow/slots/slots.h"
#include "compiler/check/checker/checker.h"
#include "compiler/check/facts/facts.h"
#include "compiler/overflow/call_ranges/call_ranges.h"
#include "compiler/overflow/evaluator/evaluator.h"
#include "compiler/overflow/executor/executor.h"
#include "compiler/overflow/loop_ranges/loop_ranges.h"
#include "compiler/overflow/narrowing/narrowing.h"
#include "compiler/overflow/overflow_helpers/overflow_helpers.h"
#include "compiler/overflow/overflow_warnings/overflow_warnings.h"
#include "compiler/overflow/program_scan/program_scan.h"
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

namespace {

constexpr unsigned kGrowthsBeforeWidening = 3;

}

Slots::Slots(OverflowCheck &check) : check_(check) {}

Val Slots::readVar(const VarSymbol *var, const State &st) {
  auto info = check_.vars_.find(var);
  bool own = info != check_.vars_.end() && info->second.declared &&
             info->second.owner == check_.owner_ && !info->second.writtenElsewhere;
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
    std::optional<Val> s = check_.summary(check_.cur_.vars, var);
    v = s ? check_.overflowWarnings().fitVal(*s, var->type)
          : check_.overflowWarnings().trusted(var->type);
  }

  return v;
}

void Slots::writeVar(const VarSymbol *var, Val v, State &st) {
  if (!var || !tracked(var->type)) {
    return;
  }
  v = check_.overflowWarnings().fitVal(std::move(v), var->type);
  auto info = check_.vars_.find(var);
  if (info != check_.vars_.end() && info->second.shared) {
    check_.contribute(check_.next_.vars[var], v);
  }
  if (info != check_.vars_.end() && info->second.declared && info->second.owner == check_.owner_ &&
      !info->second.writtenElsewhere) {
    st.vars[var] = std::move(v);
  }
}

Val Slots::readField(const Type *objectType, llvm::StringRef field, const Type *type) {
  if (!tracked(type)) {
    return Val();
  }
  if (std::optional<Val> v =
          check_.summary(check_.cur_.fields, std::make_pair(objectType, field.str()))) {
    return check_.overflowWarnings().fitVal(*v, type);
  }

  return check_.overflowWarnings().trusted(type);
}

void Slots::storeField(const Type *objectType, llvm::StringRef field, const Type *type,
                       const Val &v) {
  if (check_.quiet_ || !objectType || !tracked(type)) {
    return;
  }
  OverflowCheck::Slot &slot = check_.next_.fields[{objectType, field.str()}];
  slot.type = type;
  check_.contribute(slot, check_.overflowWarnings().fitVal(v, type));
}

bool Slots::isPrivateArray(const Expr *expr) const {
  expr = stripCasts(expr);
  if (!expr) {
    return false;
  }
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    if (!name->var) {
      return false;
    }
    auto info = check_.vars_.find(name->var);
    return info != check_.vars_.end() && info->second.declared && !info->second.aliased &&
           !info->second.writtenElsewhere && info->second.owner == check_.owner_;
  }
  if (isa<ArrayLitExpr>(expr) || isa<NewExpr>(expr)) {
    return true;
  }
  if (auto *call = dyn_cast<CallExpr>(expr)) {
    switch (call->builtin) {
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
    return check_.overflowWarnings().trusted(element);
  }
  Val v = *array.elem;
  if (!isPrivateArray(arrayExpr)) {
    if (std::optional<Val> stored = check_.summary(check_.cur_.elements, element)) {
      v = join(v, *stored);
    } else if (check_.round_ == 0) {
      return check_.overflowWarnings().trusted(element);
    }
  }

  return check_.overflowWarnings().fitVal(v, element);
}

void Slots::storeElements(const Expr *arrayExpr, const Type *arrayType, const Val &v, State &st) {
  if (!arrayType || !arrayType->nonOptional()->isArray()) {
    return;
  }
  const Type *element = arrayType->nonOptional()->inner();
  check_.contribute(check_.next_.elements[element], v);
  auto *name = dyn_cast_or_null<NameExpr>(stripCasts(arrayExpr));
  if (name && name->var) {
    auto found = st.vars.find(name->var);
    if (found != st.vars.end()) {
      Val &array = found->second;
      array.elem = array.elem ? share(join(*array.elem, v)) : nullptr;
    }
  }
}

const Reason *Slots::paramReason(const FunctionDecl *fn, size_t index) {
  auto found = check_.next_.dangerousCalls.find({fn, index});
  if (found == check_.next_.dangerousCalls.end() || found->second.empty()) {
    return nullptr;
  }
  Reason &r = check_.reasons_.emplace_back();
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
      v.num = meet(v.num, n.num);
    }
    if (!v.len.empty() && !n.len.empty()) {
      v.len = meet(v.len, n.len);
    }
    bool changed = !sameVal(v, into.val);
    into.val = v;
    return changed;
  }
  if (!from) {
    return false;
  }
  Val joined = join(into.val, from->val);
  if (sameVal(joined, into.val)) {
    return false;
  }
  if (++into.growths > kGrowthsBeforeWidening || check_.round_ >= kMaxRounds) {
    joined = widenVal(into.val, joined, type);
  }
  into.val = joined;

  return true;
}

}
