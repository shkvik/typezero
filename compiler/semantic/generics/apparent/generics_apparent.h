#pragma once

#include "compiler/program/types/types.h"

namespace tz {

class SemanticState;

class GenericsApparent {
public:
  explicit GenericsApparent(SemanticState &owner);

  const Type *apparentType(const Type *type) const;
  const Type *numberParam(const Type *param, const Type *only = nullptr) const;

private:
  bool isBoundedParam(const Type *type) const;
  bool hasBoundedMember(const Type *type) const;
  bool hasOneLiteralBase(const Type *type) const;


  const Type *apparentUnion(const Type *type) const;
  const Type *commonNumericAlternative(const Type *param) const;

  SemanticState &state;
};

}
