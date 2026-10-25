#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// `A extends B` of conditional types.
class ExtendsRelation {
public:
  explicit ExtendsRelation(Checker &checker);

  // `type extends pattern`, as TypeScript decides it for a conditional
  // type: a member of a union or a literal extends what its whole does, an
  // object one with at least the fields of the pattern (as a constraint
  // says), an array and a tuple by their elements, a function by its
  // parameters and its result; `infer` takes what stands in its place.
  bool extendsType(const Type *type, const Type *pattern, Checker::InferMatch *inference = nullptr);
  bool extendsTuple(const Type *type, const Type *pattern, Checker::InferMatch *inference);
  bool extendsFunction(const Type *type, const Type *pattern, Checker::InferMatch *inference);
  bool extendsObject(const Type *type, const Type *pattern, Checker::InferMatch *inference);

private:
  Checker &checker_;
};

} // namespace tz
