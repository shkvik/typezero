#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class ExtendsRelation {
public:
  explicit ExtendsRelation(CheckState &owner);

  bool extendsType(const Type *type, const Type *pattern, InferMatch *inference = nullptr);
  bool extendsTuple(const Type *type, const Type *pattern, InferMatch *inference);
  bool extendsFunction(const Type *type, const Type *pattern, InferMatch *inference);
  bool extendsObject(const Type *type, const Type *pattern, InferMatch *inference);

private:
  static std::vector<const Type *> distributed(const Type *type);

  CheckState &state;
};

}
