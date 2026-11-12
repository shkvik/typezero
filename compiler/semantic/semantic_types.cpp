#include "compiler/semantic/semantic_types.h"
#include "compiler/semantic/generics/widening_relations/widening_relations.h"
#include "compiler/semantic/library/async/async_checker/async_checker.h"
#include "compiler/semantic/types/type_relations/type_relations.h"

namespace tz {

const Type *SemanticTypes::awaitedType(const Type *type) {
  return AsyncChecker::awaitedType(type);
}

bool SemanticTypes::isCopyingIntoRecord(const Type *from, const Type *to) {
  return TypeRelations::isCopyingIntoRecord(from, to);
}

bool SemanticTypes::isImplicitlyConvertible(const Type *from, const Type *to) {
  return TypeRelations::isImplicitlyConvertible(from, to);
}

std::vector<NarrowedPart> SemanticTypes::narrowingsOf(const Type *from, const Type *to) {
  return WideningRelations::narrowingsOf(from, to);
}

}
