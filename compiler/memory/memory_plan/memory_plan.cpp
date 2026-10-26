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
    result = fnMayBeMember || hasCycles;
    break;
  case Type::Kind::Promise:
    result = hasCycles;
    break;
  case Type::Kind::Unknown:
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

}
