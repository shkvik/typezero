#include "compiler/semantic/generics_v2/apparent/generics_apparent.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <vector>

namespace tz {

GenericsApparent::GenericsApparent(SemanticState &owner)
    : state(owner) {}

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

const Type *GenericsApparent::numberParam(const Type *param, const Type *only) const {
  const auto *known = this->state.generics.parameters.findNumberParam(param, only);
  if (known != nullptr) {
    return known;
  }
  const Type *constraint = only != nullptr ? only : this->commonNumericAlternative(param);
  auto *type = Type::param(param->name());
  type->setConstraint(constraint != nullptr ? constraint : Type::doubleTy(), {});
  this->state.generics.parameters.rememberNumberParam(param, only, type);

  return type;
}

const Type *GenericsApparent::commonNumericAlternative(const Type *param) const {
  const Type *common = nullptr;
  for (const Type *candidate : param->alternatives()) {
    if (candidate->isNumeric()) {
      common = common != nullptr ? this->state.types.relations.commonNumericType(common, candidate)
                                 : candidate;
    }
  }

  return common;
}

}
