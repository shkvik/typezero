#pragma once

#include "compiler/program/types/type/type.h"
#include "llvm/ADT/SmallPtrSet.h"

namespace tz {

class TypeQueries {
public:
  static bool mentionsParam(const Type *type);
  static int subtypeDistance(const Type *from, const Type *to);

private:
  static bool mentionsParam(const Type *type, llvm::SmallPtrSetImpl<const Type *> &seen);
};

}
