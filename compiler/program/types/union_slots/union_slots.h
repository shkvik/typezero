#pragma once

#include "compiler/program/types/type/type.h"

namespace tz {

class UnionSlots {
public:
  static bool hasParamMember(const Type *type);
  static UnionSlot unionSlot(const Type *type);
  static std::string unionProblem(const Type *type);
  static const Type *unionWithout(const Type *type, const Type *without);
};

}
