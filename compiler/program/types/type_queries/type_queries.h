#pragma once

#include "compiler/program/types/type/type.h"

namespace tz {

int subtypeDistance(const Type *from, const Type *to);

bool mentionsParam(const Type *type);

bool mentionsParamBesides(const Type *type, llvm::ArrayRef<const Type *> allowed);

}
