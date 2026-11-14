#include "compiler/semantic/generics_v2/apparent/generics_apparent.h"
#include <vector>

namespace tz {

const Type *GenericsApparent::apparentType(const Type *type) const {
  if (this->isBoundedParam(type)) {
    return type->constraint();
  }
  if (type->isLiteral()) {
    return type->literalBase();
  }
  if (type->isUnion() && this->hasOneLiteralBase(type)) {
    return type->members().front()->literalBase();
  }
  if (type->isUnion() && this->hasBoundedMember(type)) {
    return this->apparentUnion(type);
  }

  return type;
}

bool GenericsApparent::isBoundedParam(const Type *type) const {
  return type->isParam() && type->constraint() != nullptr && !type->isListParam();
}

bool GenericsApparent::hasOneLiteralBase(const Type *type) const {
  const Type *first = type->members().front();
  if (!first->isLiteral()) {
    return false;
  }
  const Type *base = first->literalBase();
  for (const Type *member : type->members()) {
    if (!member->isLiteral() || member->literalBase() != base) {
      return false;
    }
  }

  return true;
}

bool GenericsApparent::hasBoundedMember(const Type *type) const {
  for (const Type *member : type->members()) {
    if (this->isBoundedParam(member)) {
      return true;
    }
  }

  return false;
}

const Type *GenericsApparent::apparentUnion(const Type *type) const {
  auto members = std::vector<const Type *>();
  for (const Type *member : type->members()) {
    const Type *seen = this->isBoundedParam(member) ? member->constraint() : member;
    if (!seen->isObject() || seen->isClass()) {
      return type;
    }
    members.push_back(seen);
  }
  const auto *joined = Type::unionOf(members);

  return joined->isTagged() ? type : joined;
}

}
