#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ExtendsRelation {
public:
  explicit ExtendsRelation(Checker &owner);

  bool extendsType(const Type *type, const Type *pattern, Checker::InferMatch *inference = nullptr);
  bool extendsTuple(const Type *type, const Type *pattern, Checker::InferMatch *inference);
  bool extendsFunction(const Type *type, const Type *pattern, Checker::InferMatch *inference);
  bool extendsObject(const Type *type, const Type *pattern, Checker::InferMatch *inference);

private:
  static std::vector<const Type *> distributed(const Type *type);

  Checker &checker;
};

}
