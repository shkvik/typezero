#include "compiler/semantic/flow_v2/joins/flow_joins.h"
#include "compiler/program/types/union_slots/union_slots.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

Flow FlowJoins::join(const Flow &left, const Flow &right) const {
  if (left.unreachable) {
    return right;
  }
  if (right.unreachable) {
    return left;
  }
  Flow result;
  result.facts = left.facts.meet(right.facts);
  this->joinDefined(left, right, result);
  for (const NarrowedVar &narrowed : left.narrowed) {
    auto both = this->narrowedOnBoth(narrowed, right);
    if (both.has_value()) {
      result.narrowed.push_back(*both);
    }
  }
  this->widenWithUndefined(left, right, result);
  this->widenWithUndefined(right, left, result);
  this->joinMarks(left, right, result);
  result.beforeSuper = left.beforeSuper || right.beforeSuper;

  return result;
}

void FlowJoins::joinDefined(const Flow &left, const Flow &right, Flow &result) const {
  for (const VarSymbol *variable : left.defined) {
    if (right.defined.contains(variable)) {
      result.defined.insert(variable);
    }
  }
  for (const FieldPath &path : left.definedFields) {
    if (llvm::is_contained(right.definedFields, path)) {
      result.definedFields.push_back(path);
    }
  }
  for (const auto &narrowed : left.narrowedFields) {
    if (llvm::is_contained(right.narrowedFields, narrowed)) {
      result.narrowedFields.push_back(narrowed);
    }
  }
}

const FlowJoins::NarrowedVar *FlowJoins::narrowedIn(const Flow &flow,
                                                    const VarSymbol *variable) const {
  for (const NarrowedVar &narrowed : flow.narrowed) {
    if (narrowed.first == variable) {
      return &narrowed;
    }
  }

  return nullptr;
}

bool FlowJoins::isPlainUnion(const Type *either, const VarSymbol *variable) const {
  if (either->isError() || !UnionSlots::unionProblem(either).empty()) {
    return false;
  }

  return either != variable->type;
}

std::optional<FlowJoins::NarrowedVar> FlowJoins::narrowedOnBoth(const NarrowedVar &narrowed,
                                                                const Flow &other) const {
  if (llvm::is_contained(other.narrowed, narrowed)) {
    return narrowed;
  }
  const VarSymbol *variable = narrowed.first;
  const Type *type = narrowed.second;
  const NarrowedVar *otherNarrowed = this->narrowedIn(other, variable);
  bool isGeneric = otherNarrowed == nullptr || variable->type->nonOptional()->isParam() ||
                   type->isParam() || otherNarrowed->second->isParam();
  if (isGeneric) {
    return std::nullopt;
  }
  const auto *either = Type::unionOf({type, otherNarrowed->second});
  if (!this->isPlainUnion(either, variable)) {
    return std::nullopt;
  }
  if (variable->type->isOptional() && either == variable->type->inner()) {
    return std::nullopt;
  }

  return NarrowedVar{variable, either};
}

void FlowJoins::widenWithUndefined(const Flow &empty, const Flow &other, Flow &result) const {
  for (const VarSymbol *variable : empty.undefinedVars) {
    auto widened = this->widenedWithUndefined(variable, other, result);
    if (widened.has_value()) {
      result.narrowed.push_back(*widened);
    }
  }
}

std::optional<FlowJoins::NarrowedVar> FlowJoins::widenedWithUndefined(const VarSymbol *variable,
                                                                      const Flow &other,
                                                                      const Flow &result) const {
  const NarrowedVar *narrowed = this->narrowedIn(other, variable);
  const Type *base = variable->type->nonOptional();
  bool isSkipped = narrowed == nullptr || base->isParam() || base->isJson() ||
                   narrowed->second->isParam() || this->narrowedIn(result, variable) != nullptr;
  if (isSkipped) {
    return std::nullopt;
  }
  const auto *either = Type::unionOf({narrowed->second, Type::undefinedTy()});
  if (!this->isPlainUnion(either, variable)) {
    return std::nullopt;
  }

  return NarrowedVar{variable, either};
}

void FlowJoins::joinMarks(const Flow &left, const Flow &right, Flow &result) const {
  for (const VarSymbol *variable : left.undefinedVars) {
    if (right.undefinedVars.contains(variable)) {
      result.undefinedVars.insert(variable);
    }
  }
  for (const VarSymbol *variable : left.okResults) {
    if (right.okResults.contains(variable)) {
      result.okResults.insert(variable);
    }
  }
  for (const VarSymbol *variable : left.errResults) {
    if (right.errResults.contains(variable)) {
      result.errResults.insert(variable);
    }
  }
  for (llvm::StringRef field : left.setFields) {
    if (llvm::is_contained(right.setFields, field)) {
      result.setFields.push_back(field);
    }
  }
  for (const EmitterKey &key : left.errorListened) {
    if (llvm::is_contained(right.errorListened, key)) {
      result.errorListened.push_back(key);
    }
  }
  result.errorPending = left.errorPending;
  for (const auto &pending : right.errorPending) {
    if (!this->hasPending(result.errorPending, pending.first)) {
      result.errorPending.push_back(pending);
    }
  }
}

bool FlowJoins::hasPending(llvm::ArrayRef<std::pair<EmitterKey, llvm::SMLoc>> pending,
                           const EmitterKey &key) const {
  for (const auto &entry : pending) {
    if (entry.first == key) {
      return true;
    }
  }

  return false;
}

}
