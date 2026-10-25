#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// `A extends B` of conditional types.
class ExtendsRelation {
public:
  explicit ExtendsRelation(Sema &sema);

  // `type extends pattern`, as TypeScript decides it for a conditional
  // type: a member of a union or a literal extends what its whole does, an
  // object one with at least the fields of the pattern (as a constraint
  // says), an array and a tuple by their elements, a function by its
  // parameters and its result; `infer` takes what stands in its place.
  bool extendsType(const Type *type, const Type *pattern, Sema::InferMatch *inference = nullptr);
  bool extendsTuple(const Type *type, const Type *pattern, Sema::InferMatch *inference);
  bool extendsFunction(const Type *type, const Type *pattern, Sema::InferMatch *inference);
  bool extendsObject(const Type *type, const Type *pattern, Sema::InferMatch *inference);

private:
  Sema &sema_;
};

} // namespace tz
