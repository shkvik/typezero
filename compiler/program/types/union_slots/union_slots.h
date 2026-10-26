#pragma once

#include "compiler/program/types/type/type.h"

namespace tz {

UnionSlot unionSlot(const Type *type);

std::string unionProblem(const Type *type);

bool hasParamMember(const Type *type);

const Type *unionWithout(const Type *type, const Type *without);

}
