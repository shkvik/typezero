#include "compiler/semantic/semantic_types.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/semantic_state/types/type_storage.h"

namespace tz {

const Type *SemanticTypes::awaitedType(const Type *type) {
  auto types = TypeStorage();
  int optionals = 0;
  const Type *inner = type;
  while (inner->isOptional()) {
    optionals += 1;
    inner = inner->inner();
  }
  const auto *awaited = SemanticTypes::awaitedPlain(types, inner);
  for (int index = 0; index < optionals; ++index) {
    if (awaited == nullptr || awaited->isVoid()) {
      return awaited;
    }
    awaited = Type::optional(awaited);
  }

  return awaited;
}

const Type *SemanticTypes::awaitedPlain(TypeStorage &types, const Type *type) {
  if (type->isPromise()) {
    return type->promised();
  }
  if (!type->isUnion() || !SemanticTypes::hasPromiseMember(type)) {
    return type;
  }
  const Type *result = nullptr;
  for (const Type *member : type->members()) {
    const Type *candidate = member->isPromise() ? member->promised() : member;
    result = result != nullptr ? types.literals.unionUnify(result, candidate) : candidate;
    if (result == nullptr || result->isError()) {
      return nullptr;
    }
  }

  return result;
}

bool SemanticTypes::hasPromiseMember(const Type *type) {
  for (const Type *member : type->members()) {
    if (member->isPromise()) {
      return true;
    }
  }

  return false;
}

bool SemanticTypes::isCopyingIntoRecord(const Type *from, const Type *to) {
  auto types = TypeStorage();

  return types.relations.isCopyingIntoRecord(from, to);
}

bool SemanticTypes::isImplicitlyConvertible(const Type *from, const Type *to) {
  auto types = TypeStorage();

  return types.relations.isImplicitlyConvertible(from, to);
}

std::vector<NarrowedPart> SemanticTypes::narrowingsOf(const Type *from, const Type *to) {
  auto types = TypeStorage();

  return types.references.narrowingsOf(from, to);
}

}
