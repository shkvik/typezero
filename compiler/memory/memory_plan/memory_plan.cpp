#include "compiler/memory/memory_plan/memory_plan.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

bool MemoryPlan::refMayBeMember(const Type *type) const {
  if (!hasCycles) {
    return false;
  }
  auto found = refMember.find(type);
  if (found != refMember.end()) {
    return found->second;
  }
  bool result = false;
  switch (type->kind()) {
  case Type::Kind::Object:
    if (type->isAnyObject()) {
      result = anyCyclicObject;
    } else if (!type->isWeakRef()) {
      result = isCyclic(type) || llvm::any_of(subtypes, [&](const Type *s) {
                 return isCyclic(s) && s->isSubtypeOf(type);
               });
    }
    break;
  case Type::Kind::Array:
    result = isCyclic(type) || cyclicArrayReps.contains(type);
    break;
  case Type::Kind::Dict:
    result = isCyclic(type);
    break;
  case Type::Kind::Function:
    // The environment of a closure, or a promise: `resolve` of `new Promise`
    // has its promise as its environment, and a promise may be a member
    // (promiseIsMember, runtime/core/async/async.c).
    result = fnMayBeMember || hasCycles;
    break;
  case Type::Kind::Promise:
    // By the type of its value, or of its error (promiseIsMember); a
    // promise of one type may be seen as one of another.
    result = hasCycles;
    break;
  case Type::Kind::Unknown:
    // An object, a promise, or the box of a value that may hold a member,
    // which is a member itself (Lowering::boxValue).
    result = hasCycles;
    break;
  case Type::Kind::Union:
    result = llvm::any_of(type->members(), [&](const Type *m) { return mayHoldMember(m); });
    break;
  default:
    break;
  }
  refMember[type] = result;

  return result;
}

bool MemoryPlan::mayHoldMember(const Type *type) const {
  if (!hasCycles) {
    return false;
  }
  switch (type->kind()) {
  case Type::Kind::Optional:
    return mayHoldMember(type->inner());
  case Type::Kind::Result:
    return mayHoldMember(type->okType()) || mayHoldMember(type->errType());
  default:
    return refMayBeMember(type);
  }
}

bool MemoryPlan::promiseIsMember(const Type *value) const {
  return hasCycles && (anyCyclicObject || (value && mayHoldMember(value)));
}

} // namespace tz
