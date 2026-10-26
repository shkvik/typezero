#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ExtendsRelation {
public:
  explicit ExtendsRelation(Checker &checker);

  bool extendsType(const Type *type, const Type *pattern, Checker::InferMatch *inference = nullptr);
  bool extendsTuple(const Type *type, const Type *pattern, Checker::InferMatch *inference);
  bool extendsFunction(const Type *type, const Type *pattern, Checker::InferMatch *inference);
  bool extendsObject(const Type *type, const Type *pattern, Checker::InferMatch *inference);

private:
  Checker &checker_;
};

}
