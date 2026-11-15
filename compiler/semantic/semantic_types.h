#pragma once

#include "compiler/semantic/generics/narrowed_part/narrowed_part.h"
#include <vector>

namespace tz {

class Type;
struct TypeStorage;

class SemanticTypes {
public:
  static const Type *awaitedType(const Type *type);
  static bool isCopyingIntoRecord(const Type *from, const Type *to);
  static bool isImplicitlyConvertible(const Type *from, const Type *to);
  static std::vector<NarrowedPart> narrowingsOf(const Type *from, const Type *to);

private:
  static bool hasPromiseMember(const Type *type);
  static const Type *awaitedPlain(TypeStorage &types, const Type *type);
};

}
