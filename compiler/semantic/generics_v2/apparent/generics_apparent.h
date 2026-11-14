#pragma once

#include "compiler/program/types/types.h"

namespace tz {

class GenericsApparent {
public:
  const Type *apparentType(const Type *type) const;

private:
  bool isBoundedParam(const Type *type) const;
  bool hasBoundedMember(const Type *type) const;
  bool hasOneLiteralBase(const Type *type) const;


  const Type *apparentUnion(const Type *type) const;
};

}
