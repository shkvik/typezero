#pragma once

#include "compiler/semantic/generics/narrowed_part/narrowed_part.h"
#include <vector>

namespace tz {

class Type;

class SemanticTypes {
public:
  static const Type *awaitedType(const Type *type);
  static bool copiesIntoRecord(const Type *from, const Type *to);
  static bool isImplicitlyConvertible(const Type *from, const Type *to);
  static std::vector<NarrowedPart> narrowingsOf(const Type *from, const Type *to);
};

}
